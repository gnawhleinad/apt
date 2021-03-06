// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: acquire-worker.cc,v 1.34 2001/05/22 04:42:54 jgg Exp $
/* ######################################################################

   Acquire Worker 

   The worker process can startup either as a Configuration prober
   or as a queue runner. As a configuration prober it only reads the
   configuration message and 
   
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include <config.h>

#include <apt-pkg/acquire.h>
#include <apt-pkg/acquire-worker.h>
#include <apt-pkg/acquire-item.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/error.h>
#include <apt-pkg/fileutl.h>
#include <apt-pkg/strutl.h>
#include <apt-pkg/hashes.h>

#include <string>
#include <vector>
#include <iostream>
#include <sstream>

#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>

#include <apti18n.h>
									/*}}}*/

using namespace std;

static void ChangeOwnerAndPermissionOfFile(char const * const requester, char const * const file, char const * const user, char const * const group, mode_t const mode) /*{{{*/
{
   if (getuid() == 0 && strlen(user) != 0 && strlen(group) != 0) // if we aren't root, we can't chown, so don't try it
   {
      // ensure the file is owned by root and has good permissions
      struct passwd const * const pw = getpwnam(user);
      struct group const * const gr = getgrnam(group);
      if (pw != NULL && gr != NULL && chown(file, pw->pw_uid, gr->gr_gid) != 0)
	 _error->WarningE(requester, "chown to %s:%s of file %s failed", user, group, file);
   }
   if (chmod(file, mode) != 0)
      _error->WarningE(requester, "chmod 0%o of file %s failed", mode, file);
}
									/*}}}*/
// Worker::Worker - Constructor for Queue startup			/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgAcquire::Worker::Worker(Queue *Q,MethodConfig *Cnf,
			   pkgAcquireStatus *Log) : Log(Log)
{
   OwnerQ = Q;
   Config = Cnf;
   Access = Cnf->Access;
   CurrentItem = 0;
   TotalSize = 0;
   CurrentSize = 0;
   
   Construct();   
}
									/*}}}*/
// Worker::Worker - Constructor for method config startup		/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgAcquire::Worker::Worker(MethodConfig *Cnf)
{
   OwnerQ = 0;
   Config = Cnf;
   Access = Cnf->Access;
   CurrentItem = 0;
   TotalSize = 0;
   CurrentSize = 0;
   
   Construct();   
}
									/*}}}*/
// Worker::Construct - Constructor helper				/*{{{*/
// ---------------------------------------------------------------------
/* */
void pkgAcquire::Worker::Construct()
{
   NextQueue = 0;
   NextAcquire = 0;
   Process = -1;
   InFd = -1;
   OutFd = -1;
   OutReady = false;
   InReady = false;
   Debug = _config->FindB("Debug::pkgAcquire::Worker",false);
}
									/*}}}*/
// Worker::~Worker - Destructor						/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgAcquire::Worker::~Worker()
{
   close(InFd);
   close(OutFd);
   
   if (Process > 0)
   {
      /* Closing of stdin is the signal to exit and die when the process
         indicates it needs cleanup */
      if (Config->NeedsCleanup == false)
	 kill(Process,SIGINT);
      ExecWait(Process,Access.c_str(),true);
   }   
}
									/*}}}*/
// Worker::Start - Start the worker process				/*{{{*/
// ---------------------------------------------------------------------
/* This forks the method and inits the communication channel */
bool pkgAcquire::Worker::Start()
{
   // Get the method path
   string Method = _config->FindDir("Dir::Bin::Methods") + Access;
   if (FileExists(Method) == false)
   {
      _error->Error(_("The method driver %s could not be found."),Method.c_str());
      if (Access == "https")
	 _error->Notice(_("Is the package %s installed?"), "apt-transport-https");
      return false;
   }

   if (Debug == true)
      clog << "Starting method '" << Method << '\'' << endl;

   // Create the pipes
   int Pipes[4] = {-1,-1,-1,-1};
   if (pipe(Pipes) != 0 || pipe(Pipes+2) != 0)
   {
      _error->Errno("pipe","Failed to create IPC pipe to subprocess");
      for (int I = 0; I != 4; I++)
	 close(Pipes[I]);
      return false;
   }
   for (int I = 0; I != 4; I++)
      SetCloseExec(Pipes[I],true);
   
   // Fork off the process
   Process = ExecFork();
   if (Process == 0)
   {
      // Setup the FDs
      dup2(Pipes[1],STDOUT_FILENO);
      dup2(Pipes[2],STDIN_FILENO);
      SetCloseExec(STDOUT_FILENO,false);
      SetCloseExec(STDIN_FILENO,false);      
      SetCloseExec(STDERR_FILENO,false);
      
      const char *Args[2];
      Args[0] = Method.c_str();
      Args[1] = 0;
      execv(Args[0],(char **)Args);
      cerr << "Failed to exec method " << Args[0] << endl;
      _exit(100);
   }

   // Fix up our FDs
   InFd = Pipes[0];
   OutFd = Pipes[3];
   SetNonBlock(Pipes[0],true);
   SetNonBlock(Pipes[3],true);
   close(Pipes[1]);
   close(Pipes[2]);
   OutReady = false;
   InReady = true;
   
   // Read the configuration data
   if (WaitFd(InFd) == false ||
       ReadMessages() == false)
      return _error->Error(_("Method %s did not start correctly"),Method.c_str());

   RunMessages();
   if (OwnerQ != 0)
      SendConfiguration();
   
   return true;
}
									/*}}}*/
// Worker::ReadMessages - Read all pending messages into the list	/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgAcquire::Worker::ReadMessages()
{
   if (::ReadMessages(InFd,MessageQueue) == false)
      return MethodFailure();
   return true;
}
									/*}}}*/
// Worker::RunMessage - Empty the message queue				/*{{{*/
// ---------------------------------------------------------------------
/* This takes the messages from the message queue and runs them through
   the parsers in order. */
bool pkgAcquire::Worker::RunMessages()
{
   while (MessageQueue.empty() == false)
   {
      string Message = MessageQueue.front();
      MessageQueue.erase(MessageQueue.begin());

      if (Debug == true)
	 clog << " <- " << Access << ':' << QuoteString(Message,"\n") << endl;
      
      // Fetch the message number
      char *End;
      int Number = strtol(Message.c_str(),&End,10);
      if (End == Message.c_str())
	 return _error->Error("Invalid message from method %s: %s",Access.c_str(),Message.c_str());

      string URI = LookupTag(Message,"URI");
      pkgAcquire::Queue::QItem *Itm = 0;
      if (URI.empty() == false)
	 Itm = OwnerQ->FindItem(URI,this);

      // update used mirror
      string UsedMirror = LookupTag(Message,"UsedMirror", "");
      if (!UsedMirror.empty() && 
          Itm && 
          Itm->Description.find(" ") != string::npos) 
      {
         Itm->Description.replace(0, Itm->Description.find(" "), UsedMirror);
         // FIXME: will we need this as well?
         //Itm->ShortDesc = UsedMirror;
      }
      
      // Determine the message number and dispatch
      switch (Number)
      {
	 // 100 Capabilities
	 case 100:
	 if (Capabilities(Message) == false)
	    return _error->Error("Unable to process Capabilities message from %s",Access.c_str());
	 break;
	 
	 // 101 Log
	 case 101:
	 if (Debug == true)
	    clog << " <- (log) " << LookupTag(Message,"Message") << endl;
	 break;
	 
	 // 102 Status
	 case 102:
	 Status = LookupTag(Message,"Message");
	 break;
	    
         // 103 Redirect
         case 103:
         {
            if (Itm == 0)
            {
               _error->Error("Method gave invalid 103 Redirect message");
               break;
            }
 
            string NewURI = LookupTag(Message,"New-URI",URI.c_str());
            Itm->URI = NewURI;

	    ItemDone();

	    pkgAcquire::Item *Owner = Itm->Owner;
	    pkgAcquire::ItemDesc Desc = *Itm;

	    // Change the status so that it can be dequeued
	    Owner->Status = pkgAcquire::Item::StatIdle;
	    // Mark the item as done (taking care of all queues)
	    // and then put it in the main queue again
	    OwnerQ->ItemDone(Itm);
	    OwnerQ->Owner->Enqueue(Desc);

	    if (Log != 0)
	       Log->Done(Desc);
            break;
         }
   
	 // 200 URI Start
	 case 200:
	 {
	    if (Itm == 0)
	    {
	       _error->Error("Method gave invalid 200 URI Start message");
	       break;
	    }
	    
	    CurrentItem = Itm;
	    CurrentSize = 0;
	    TotalSize = strtoull(LookupTag(Message,"Size","0").c_str(), NULL, 10);
	    ResumePoint = strtoull(LookupTag(Message,"Resume-Point","0").c_str(), NULL, 10);
	    Itm->Owner->Start(Message,strtoull(LookupTag(Message,"Size","0").c_str(), NULL, 10));

	    // Display update before completion
	    if (Log != 0 && Log->MorePulses == true)
	       Log->Pulse(Itm->Owner->GetOwner());
	    
	    if (Log != 0)
	       Log->Fetch(*Itm);

	    break;
	 }
	 
	 // 201 URI Done
	 case 201:
	 {
	    if (Itm == 0)
	    {
	       _error->Error("Method gave invalid 201 URI Done message");
	       break;
	    }
	    
	    pkgAcquire::Item *Owner = Itm->Owner;
	    pkgAcquire::ItemDesc Desc = *Itm;

	    if (RealFileExists(Owner->DestFile))
	       ChangeOwnerAndPermissionOfFile("201::URIDone", Owner->DestFile.c_str(), "root", "root", 0644);

	    // Display update before completion
	    if (Log != 0 && Log->MorePulses == true)
	       Log->Pulse(Owner->GetOwner());
	    
	    OwnerQ->ItemDone(Itm);
	    unsigned long long const ServerSize = strtoull(LookupTag(Message,"Size","0").c_str(), NULL, 10);
            bool isHit = StringToBool(LookupTag(Message,"IMS-Hit"),false) ||
                         StringToBool(LookupTag(Message,"Alt-IMS-Hit"),false);
            // Using the https method the server might return 200, but the
            // If-Modified-Since condition is not satsified, libcurl will
            // discard the download. In this case, however, TotalSize will be
            // set to the actual size of the file, while ServerSize will be set
            // to 0. Therefore, if the item is marked as a hit and the
            // downloaded size (ServerSize) is 0, we ignore TotalSize.
	    if (TotalSize != 0 && (!isHit || ServerSize != 0) && ServerSize != TotalSize)
	       _error->Warning("Size of file %s is not what the server reported %s %llu",
			       Owner->DestFile.c_str(), LookupTag(Message,"Size","0").c_str(),TotalSize);

	    // see if there is a hash to verify
	    HashStringList RecivedHashes;
	    HashStringList expectedHashes = Owner->HashSums();
	    for (HashStringList::const_iterator hs = expectedHashes.begin(); hs != expectedHashes.end(); ++hs)
	    {
	       std::string const tagname = hs->HashType() + "-Hash";
	       std::string const hashsum = LookupTag(Message, tagname.c_str());
	       if (hashsum.empty() == false)
		  RecivedHashes.push_back(HashString(hs->HashType(), hashsum));
	    }

	    if(_config->FindB("Debug::pkgAcquire::Auth", false) == true)
	    {
	       std::clog << "201 URI Done: " << Owner->DescURI() << endl
		  << "RecivedHash:" << endl;
	       for (HashStringList::const_iterator hs = RecivedHashes.begin(); hs != RecivedHashes.end(); ++hs)
		  std::clog <<  "\t- " << hs->toStr() << std::endl;
	       std::clog << "ExpectedHash:" << endl;
	       for (HashStringList::const_iterator hs = expectedHashes.begin(); hs != expectedHashes.end(); ++hs)
		  std::clog <<  "\t- " << hs->toStr() << std::endl;
	       std::clog << endl;
	    }
	    Owner->Done(Message, ServerSize, RecivedHashes, Config);
	    ItemDone();

	    // Log that we are done
	    if (Log != 0)
	    {
	       if (isHit)
	       {
		  /* Hide 'hits' for local only sources - we also manage to
		     hide gets */
		  if (Config->LocalOnly == false)
		     Log->IMSHit(Desc);
	       }	       
	       else
		  Log->Done(Desc);
	    }
	    break;
	 }	 
	 
	 // 400 URI Failure
	 case 400:
	 {
	    if (Itm == 0)
	    {
	       std::string const msg = LookupTag(Message,"Message");
	       _error->Error("Method gave invalid 400 URI Failure message: %s", msg.c_str());
	       break;
	    }

	    // Display update before completion
	    if (Log != 0 && Log->MorePulses == true)
	       Log->Pulse(Itm->Owner->GetOwner());

	    pkgAcquire::Item *Owner = Itm->Owner;
	    pkgAcquire::ItemDesc Desc = *Itm;

	    if (RealFileExists(Owner->DestFile))
	       ChangeOwnerAndPermissionOfFile("400::URIFailure", Owner->DestFile.c_str(), "root", "root", 0644);

	    OwnerQ->ItemDone(Itm);

	    // set some status
	    if(LookupTag(Message,"FailReason") == "Timeout" || 
	       LookupTag(Message,"FailReason") == "TmpResolveFailure" ||
	       LookupTag(Message,"FailReason") == "ResolveFailure" ||
	       LookupTag(Message,"FailReason") == "ConnectionRefused") 
	       Owner->Status = pkgAcquire::Item::StatTransientNetworkError;

	    Owner->Failed(Message,Config);
	    ItemDone();

	    if (Log != 0)
	       Log->Fail(Desc);

	    break;
	 }	 
	 
	 // 401 General Failure
	 case 401:
	 _error->Error("Method %s General failure: %s",Access.c_str(),LookupTag(Message,"Message").c_str());
	 break;
	 
	 // 403 Media Change
	 case 403:
	 MediaChange(Message); 
	 break;
      }      
   }
   return true;
}
									/*}}}*/
// Worker::Capabilities - 100 Capabilities handler			/*{{{*/
// ---------------------------------------------------------------------
/* This parses the capabilities message and dumps it into the configuration
   structure. */
bool pkgAcquire::Worker::Capabilities(string Message)
{
   if (Config == 0)
      return true;
   
   Config->Version = LookupTag(Message,"Version");
   Config->SingleInstance = StringToBool(LookupTag(Message,"Single-Instance"),false);
   Config->Pipeline = StringToBool(LookupTag(Message,"Pipeline"),false);
   Config->SendConfig = StringToBool(LookupTag(Message,"Send-Config"),false);
   Config->LocalOnly = StringToBool(LookupTag(Message,"Local-Only"),false);
   Config->NeedsCleanup = StringToBool(LookupTag(Message,"Needs-Cleanup"),false);
   Config->Removable = StringToBool(LookupTag(Message,"Removable"),false);

   // Some debug text
   if (Debug == true)
   {
      clog << "Configured access method " << Config->Access << endl;
      clog << "Version:" << Config->Version <<
	      " SingleInstance:" << Config->SingleInstance <<
	      " Pipeline:" << Config->Pipeline << 
	      " SendConfig:" << Config->SendConfig << 
	      " LocalOnly: " << Config->LocalOnly << 
	      " NeedsCleanup: " << Config->NeedsCleanup << 
	      " Removable: " << Config->Removable << endl;
   }
   
   return true;
}
									/*}}}*/
// Worker::MediaChange - Request a media change				/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgAcquire::Worker::MediaChange(string Message)
{
   int status_fd = _config->FindI("APT::Status-Fd",-1);
   if(status_fd > 0) 
   {
      string Media = LookupTag(Message,"Media");
      string Drive = LookupTag(Message,"Drive"); 
      ostringstream msg,status;
      ioprintf(msg,_("Please insert the disc labeled: "
		     "'%s' "
		     "in the drive '%s' and press enter."),
	       Media.c_str(),Drive.c_str());
      status << "media-change: "  // message
	     << Media  << ":"     // media
	     << Drive  << ":"     // drive
	     << msg.str()         // l10n message
	     << endl;

      std::string const dlstatus = status.str();
      FileFd::Write(status_fd, dlstatus.c_str(), dlstatus.size());
   }

   if (Log == 0 || Log->MediaChange(LookupTag(Message,"Media"),
				    LookupTag(Message,"Drive")) == false)
   {
      char S[300];
      snprintf(S,sizeof(S),"603 Media Changed\nFailed: true\n\n");
      if (Debug == true)
	 clog << " -> " << Access << ':' << QuoteString(S,"\n") << endl;
      OutQueue += S;
      OutReady = true;
      return true;
   }

   char S[300];
   snprintf(S,sizeof(S),"603 Media Changed\n\n");
   if (Debug == true)
      clog << " -> " << Access << ':' << QuoteString(S,"\n") << endl;
   OutQueue += S;
   OutReady = true;
   return true;
}
									/*}}}*/
// Worker::SendConfiguration - Send the config to the method		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgAcquire::Worker::SendConfiguration()
{
   if (Config->SendConfig == false)
      return true;

   if (OutFd == -1)
      return false;

   /* Write out all of the configuration directives by walking the
      configuration tree */
   std::ostringstream Message;
   Message << "601 Configuration\n";
   _config->Dump(Message, NULL, "Config-Item: %F=%V\n", false);
   Message << '\n';

   if (Debug == true)
      clog << " -> " << Access << ':' << QuoteString(Message.str(),"\n") << endl;
   OutQueue += Message.str();
   OutReady = true;

   return true;
}
									/*}}}*/
// Worker::QueueItem - Add an item to the outbound queue		/*{{{*/
// ---------------------------------------------------------------------
/* Send a URI Acquire message to the method */
bool pkgAcquire::Worker::QueueItem(pkgAcquire::Queue::QItem *Item)
{
   if (OutFd == -1)
      return false;
   
   string Message = "600 URI Acquire\n";
   Message.reserve(300);
   Message += "URI: " + Item->URI;
   Message += "\nFilename: " + Item->Owner->DestFile;
   HashStringList const hsl = Item->Owner->HashSums();
   for (HashStringList::const_iterator hs = hsl.begin(); hs != hsl.end(); ++hs)
      Message += "\nExpected-" + hs->HashType() + ": " + hs->HashValue();
   if(Item->Owner->FileSize > 0)
   {
      string MaximumSize;
      strprintf(MaximumSize, "%llu", Item->Owner->FileSize);
      Message += "\nMaximum-Size: " + MaximumSize;
   }
   Message += Item->Owner->Custom600Headers();
   Message += "\n\n";

   if (RealFileExists(Item->Owner->DestFile))
   {
      std::string SandboxUser = _config->Find("APT::Sandbox::User");
      ChangeOwnerAndPermissionOfFile("Item::QueueURI", Item->Owner->DestFile.c_str(),
                                     SandboxUser.c_str(), "root", 0600);
   }

   if (Debug == true)
      clog << " -> " << Access << ':' << QuoteString(Message,"\n") << endl;
   OutQueue += Message;
   OutReady = true;
   
   return true;
}
									/*}}}*/
// Worker::OutFdRead - Out bound FD is ready				/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgAcquire::Worker::OutFdReady()
{
   int Res;
   do
   {
      Res = write(OutFd,OutQueue.c_str(),OutQueue.length());
   }
   while (Res < 0 && errno == EINTR);

   if (Res <= 0)
      return MethodFailure();

   OutQueue.erase(0,Res);
   if (OutQueue.empty() == true)
      OutReady = false;
   
   return true;
}
									/*}}}*/
// Worker::InFdRead - In bound FD is ready				/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgAcquire::Worker::InFdReady()
{
   if (ReadMessages() == false)
      return false;
   RunMessages();
   return true;
}
									/*}}}*/
// Worker::MethodFailure - Called when the method fails			/*{{{*/
// ---------------------------------------------------------------------
/* This is called when the method is believed to have failed, probably because
   read returned -1. */
bool pkgAcquire::Worker::MethodFailure()
{
   _error->Error("Method %s has died unexpectedly!",Access.c_str());
   
   // do not reap the child here to show meaningfull error to the user
   ExecWait(Process,Access.c_str(),false);
   Process = -1;
   close(InFd);
   close(OutFd);
   InFd = -1;
   OutFd = -1;
   OutReady = false;
   InReady = false;
   OutQueue = string();
   MessageQueue.erase(MessageQueue.begin(),MessageQueue.end());
   
   return false;
}
									/*}}}*/
// Worker::Pulse - Called periodically 					/*{{{*/
// ---------------------------------------------------------------------
/* */
void pkgAcquire::Worker::Pulse()
{
   if (CurrentItem == 0)
      return;
 
   struct stat Buf;
   if (stat(CurrentItem->Owner->DestFile.c_str(),&Buf) != 0)
      return;
   CurrentSize = Buf.st_size;
   
   // Hmm? Should not happen...
   if (CurrentSize > TotalSize && TotalSize != 0)
      TotalSize = CurrentSize;
}
									/*}}}*/
// Worker::ItemDone - Called when the current item is finished		/*{{{*/
// ---------------------------------------------------------------------
/* */
void pkgAcquire::Worker::ItemDone()
{
   CurrentItem = 0;
   CurrentSize = 0;
   TotalSize = 0;
   Status = string();
}
									/*}}}*/
