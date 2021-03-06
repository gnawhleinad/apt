// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: indexrecords.cc,v 1.1.2.4 2003/12/30 02:11:43 mdz Exp $
									/*}}}*/
// Include Files							/*{{{*/
#include<config.h>

#include <apt-pkg/indexrecords.h>
#include <apt-pkg/tagfile.h>
#include <apt-pkg/error.h>
#include <apt-pkg/strutl.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/fileutl.h>
#include <apt-pkg/hashes.h>
#include <apt-pkg/gpgv.h>

#include <stdlib.h>
#include <time.h>
#include <clocale>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <apti18n.h>
									/*}}}*/

using std::string;

APT_PURE string indexRecords::GetDist() const
{
   return this->Dist;
}

APT_PURE string indexRecords::GetSuite() const
{
   return this->Suite;
}

APT_PURE bool indexRecords::GetSupportsAcquireByHash() const
{
   return this->SupportsAcquireByHash;
}

APT_PURE bool indexRecords::CheckDist(const string MaybeDist) const
{
   return (this->Dist == MaybeDist
	   || this->Suite == MaybeDist);
}

APT_PURE string indexRecords::GetExpectedDist() const
{
   return this->ExpectedDist;
}

APT_PURE time_t indexRecords::GetValidUntil() const
{
   return this->ValidUntil;
}

APT_PURE indexRecords::checkSum *indexRecords::Lookup(const string MetaKey)
{
   std::map<std::string, indexRecords::checkSum* >::const_iterator sum = Entries.find(MetaKey);
   if (sum == Entries.end())
      return NULL;
   return sum->second;
}

APT_PURE bool indexRecords::Exists(string const &MetaKey) const
{
   return Entries.count(MetaKey) == 1;
}

bool indexRecords::Load(const string Filename)				/*{{{*/
{
   FileFd Fd;
   if (OpenMaybeClearSignedFile(Filename, Fd) == false)
      return false;

   pkgTagFile TagFile(&Fd, Fd.Size());
   if (_error->PendingError() == true)
   {
      strprintf(ErrorText, _("Unable to parse Release file %s"),Filename.c_str());
      return false;
   }

   pkgTagSection Section;
   const char *Start, *End;
   if (TagFile.Step(Section) == false)
   {
      strprintf(ErrorText, _("No sections in Release file %s"), Filename.c_str());
      return false;
   }
   // FIXME: find better tag name
   SupportsAcquireByHash = Section.FindB("Acquire-By-Hash", false);

   Suite = Section.FindS("Suite");
   Dist = Section.FindS("Codename");

   bool FoundHashSum = false;
   for (int i=0;HashString::SupportedHashes()[i] != NULL; i++)
   {
      if (!Section.Find(HashString::SupportedHashes()[i], Start, End))
	 continue;

      string Name;
      string Hash;
      unsigned long long Size;
      while (Start < End)
      {
	 if (!parseSumData(Start, End, Name, Hash, Size))
	    return false;

         if (Entries.find(Name) == Entries.end())
         {
            indexRecords::checkSum *Sum = new indexRecords::checkSum;
            Sum->MetaKeyFilename = Name;
            Sum->Size = Size;
	    std::string SizeStr;
	    strprintf(SizeStr, "%llu", Size);
	    Sum->Hashes.push_back(HashString("Checksum-FileSize", SizeStr));
#if __GNUC__ >= 4
       #pragma GCC diagnostic push
       #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
            Sum->Hash = HashString(HashString::SupportedHashes()[i],Hash);
#if __GNUC__ >= 4
       #pragma GCC diagnostic pop
#endif
            Entries[Name] = Sum;
         }
         Entries[Name]->Hashes.push_back(HashString(HashString::SupportedHashes()[i],Hash));
         FoundHashSum = true;
      }
   }

   if(FoundHashSum == false)
   {
      strprintf(ErrorText, _("No Hash entry in Release file %s"), Filename.c_str());
      return false;
   }

   string Label = Section.FindS("Label");
   string StrDate = Section.FindS("Date");
   string StrValidUntil = Section.FindS("Valid-Until");

   // if we have a Valid-Until header in the Release file, use it as default
   if (StrValidUntil.empty() == false)
   {
      if(RFC1123StrToTime(StrValidUntil.c_str(), ValidUntil) == false)
      {
	 strprintf(ErrorText, _("Invalid 'Valid-Until' entry in Release file %s"), Filename.c_str());
	 return false;
      }
   }
   // get the user settings for this archive and use what expires earlier
   int MaxAge = _config->FindI("Acquire::Max-ValidTime", 0);
   if (Label.empty() == false)
      MaxAge = _config->FindI(("Acquire::Max-ValidTime::" + Label).c_str(), MaxAge);
   int MinAge = _config->FindI("Acquire::Min-ValidTime", 0);
   if (Label.empty() == false)
      MinAge = _config->FindI(("Acquire::Min-ValidTime::" + Label).c_str(), MinAge);

   if(MaxAge == 0 &&
      (MinAge == 0 || ValidUntil == 0)) // No user settings, use the one from the Release file
      return true;

   time_t date;
   if (RFC1123StrToTime(StrDate.c_str(), date) == false)
   {
      strprintf(ErrorText, _("Invalid 'Date' entry in Release file %s"), Filename.c_str());
      return false;
   }

   if (MinAge != 0 && ValidUntil != 0) {
      time_t const min_date = date + MinAge;
      if (ValidUntil < min_date)
	 ValidUntil = min_date;
   }
   if (MaxAge != 0) {
      time_t const max_date = date + MaxAge;
      if (ValidUntil == 0 || ValidUntil > max_date)
	 ValidUntil = max_date;
   }

   return true;
}
									/*}}}*/
std::vector<string> indexRecords::MetaKeys()				/*{{{*/
{
   std::vector<std::string> keys;
   std::map<string,checkSum *>::iterator I = Entries.begin();
   while(I != Entries.end()) {
      keys.push_back((*I).first);
      ++I;
   }
   return keys;
}
									/*}}}*/
bool indexRecords::parseSumData(const char *&Start, const char *End,	/*{{{*/
				   string &Name, string &Hash, unsigned long long &Size)
{
   Name = "";
   Hash = "";
   Size = 0;
   /* Skip over the first blank */
   while ((*Start == '\t' || *Start == ' ' || *Start == '\n' || *Start == '\r')
	  && Start < End)
      Start++;
   if (Start >= End)
      return false;

   /* Move EntryEnd to the end of the first entry (the hash) */
   const char *EntryEnd = Start;
   while ((*EntryEnd != '\t' && *EntryEnd != ' ')
	  && EntryEnd < End)
      EntryEnd++;
   if (EntryEnd == End)
      return false;

   Hash.append(Start, EntryEnd-Start);

   /* Skip over intermediate blanks */
   Start = EntryEnd;
   while (*Start == '\t' || *Start == ' ')
      Start++;
   if (Start >= End)
      return false;
   
   EntryEnd = Start;
   /* Find the end of the second entry (the size) */
   while ((*EntryEnd != '\t' && *EntryEnd != ' ' )
	  && EntryEnd < End)
      EntryEnd++;
   if (EntryEnd == End)
      return false;
   
   Size = strtoull (Start, NULL, 10);
      
   /* Skip over intermediate blanks */
   Start = EntryEnd;
   while (*Start == '\t' || *Start == ' ')
      Start++;
   if (Start >= End)
      return false;
   
   EntryEnd = Start;
   /* Find the end of the third entry (the filename) */
   while ((*EntryEnd != '\t' && *EntryEnd != ' ' && 
           *EntryEnd != '\n' && *EntryEnd != '\r')
	  && EntryEnd < End)
      EntryEnd++;

   Name.append(Start, EntryEnd-Start);
   Start = EntryEnd; //prepare for the next round
   return true;
}
									/*}}}*/

APT_PURE bool indexRecords::IsAlwaysTrusted() const
{
   if (Trusted == ALWAYS_TRUSTED)
      return true;
   return false;
}
APT_PURE bool indexRecords::IsNeverTrusted() const
{
   if (Trusted == NEVER_TRUSTED)
      return true;
   return false;
}
void indexRecords::SetTrusted(bool const Trusted)
{
   if (Trusted == true)
      this->Trusted = ALWAYS_TRUSTED;
   else
      this->Trusted = NEVER_TRUSTED;
}

indexRecords::indexRecords(const string &ExpectedDist) :
   Trusted(CHECK_TRUST), d(NULL), ExpectedDist(ExpectedDist), ValidUntil(0),
   SupportsAcquireByHash(false)
{
}

indexRecords::~indexRecords() {}
