// @(#)root/tree:$Name:  $:$Id: TDSet.cxx,v 1.32 2006/06/03 05:49:00 brun Exp $
// Author: Fons Rademakers   11/01/02

/*************************************************************************
 * Copyright (C) 1995-2001, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

//////////////////////////////////////////////////////////////////////////
//                                                                      //
// TDSet                                                                //
//                                                                      //
// This class implements a data set to be used for PROOF processing.    //
// The TDSet defines the class of which objects will be processed,      //
// the directory in the file where the objects of that type can be      //
// found and the list of files to be processed. The files can be        //
// specified as logical file names (LFN's) or as physical file names    //
// (PFN's). In case of LFN's the resolution to PFN's will be done       //
// according to the currently active GRID interface.                    //
// Examples:                                                            //
//   TDSet treeset("TTree", "AOD");                                     //
//   treeset.Add("lfn:/alien.cern.ch/alice/prod2002/file1");            //
//   ...                                                                //
//   treeset.AddFriend(friendset);                                      //
//                                                                      //
// or                                                                   //
//                                                                      //
//   TDSet objset("MyEvent", "*", "/events");                           //
//   objset.Add("root://cms.cern.ch/user/prod2002/hprod_1.root");       //
//   ...                                                                //
//   objset.Add(set2003);                                               //
//                                                                      //
// Validity of file names will only be checked at processing time       //
// (typically on the PROOF master server), not at creation time.        //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

#include "TDSet.h"

#include "Riostream.h"
#include "TClass.h"
#include "TClassTable.h"
#include "TCut.h"
#include "TError.h"
#include "TFile.h"
#include "TFileInfo.h"
#include "TKey.h"
#include "TList.h"
#include "TMap.h"
#include "TROOT.h"
#include "TTimeStamp.h"
#include "TTree.h"
#include "TUrl.h"
#include "TRegexp.h"
#include "TVirtualPerfStats.h"
#include "TVirtualProof.h"
#include "TChainProof.h"
#include "TPluginManager.h"
#include "TChain.h"
#include "TChainElement.h"
#include "TSystem.h"
#include "THashList.h"
#include <set>
#include <queue>

ClassImp(TDSetElement)
ClassImp(TDSet)

//______________________________________________________________________________
TDSetElement::TDSetElement(const char *file, const char *objname, const char *dir,
                           Long64_t first, Long64_t num,
                           const char *msd)
{
   // Create a TDSet element.

   fFileName = file;
   if (first < 0) {
      Warning("TDSetElement", "first must be >= 0, %d is not allowed - setting to 0", first);
      fFirst = 0;
   } else {
      fFirst = first;
   }
   if (num < -1) {
      Warning("TDSetElement", "num must be >= -1, %d is not allowed - setting to -1", num);
      fNum   = -1;
   } else {
      fNum   = num;
   }
   fMsd         = msd;
   fTDSetOffset = 0;
   fEventList   = 0;
   fFriends     = 0;
   fValid       = kFALSE;
   fEntries     = -1;

   if (objname)
      fObjName = objname;
   if (dir)
      fDirectory = dir;
}

//______________________________________________________________________________
TDSetElement::TDSetElement(const TDSetElement& elem) : TObject()
{
   // copy constructor
   fFileName = elem.GetFileName();
   fObjName = elem.GetObjName();
   fDirectory = elem.GetDirectory();
   fFirst = elem.fFirst;
   fNum = elem.fNum;
   fMsd = elem.fMsd;
   fTDSetOffset = elem.fTDSetOffset;
   fEventList = 0;
   fValid = elem.fValid;
   fEntries = elem.fEntries;
   fIsTree = elem.fIsTree;
   fFriends = 0;
}

//______________________________________________________________________________
TDSetElement::~TDSetElement()
{
   // Clean up the element.
   DeleteFriends();
}

//______________________________________________________________________________
const char *TDSetElement::GetObjName() const
{
   // Return object name.

   return fObjName;
}

//______________________________________________________________________________
const char *TDSetElement::GetDirectory() const
{
   // Return directory where to look for object.

   return fDirectory;
}

//______________________________________________________________________________
void TDSetElement::Print(Option_t *opt) const
{
   // Print a TDSetElement. When option="a" print full data.

   if (opt && opt[0] == 'a') {
      cout << IsA()->GetName()
           << " file=\"" << fFileName
           << "\" dir=\"" << fDirectory
           << "\" obj=\"" << fObjName
           << "\" first=" << fFirst
           << " num=" << fNum
           << " msd=\"" << fMsd
           << "\"" << endl;
   } else
      cout << "\tLFN: " << fFileName << endl;
}

//______________________________________________________________________________
void TDSetElement::Validate(Bool_t isTree)
{
   // Validate by opening the file.

   Long64_t entries = TDSet::GetEntries(isTree, fFileName,
                                        fDirectory, fObjName);
   if (entries < 0) return; // Error should be reported by GetEntries()
   if (fFirst < entries) {
      if (fNum == -1) {
         fNum = entries - fFirst;
         fValid = kTRUE;
      } else {
         if (fNum <= entries - fFirst) {
            fValid = kTRUE;
         } else {
            Error("Validate", "TDSetElement has only %d entries starting"
                  " with entry %d, while %d were requested",
                  entries - fFirst, fFirst, fNum);
         }
      }
   } else {
      Error("Validate", "TDSetElement has only %d entries with"
            " first entry requested as %d", entries, fFirst);
   }
}

//______________________________________________________________________________
void TDSetElement::Validate(TDSetElement *elem)
{
   // Validate by checking against another element.

   // NOTE: Since this function validates against another TDSetElement,
   //       if the other TDSetElement (elem) did not use -1 to request all
   //       entries, this TDSetElement may get less than all entries if it
   //       requests all (with -1). For the application it was developed for
   //       (TProofSuperMaster::ValidateDSet) it works, since the design was
   //       to send the elements to their mass storage domain and let them
   //       look at the file and send the info back to the supermaster. The
   //       ability to set fValid was also required to be only exist in
   //       TDSetElement through certain function and not be set externally.
   //       TDSetElement may need to be extended for more general applications.

   if (!elem || !elem->GetValid()) {
      Error("Validate", "TDSetElement to validate against is not valid");
      return;
   }

   if (!strcmp(GetFileName(), elem->GetFileName()) &&
       !strcmp(GetDirectory(), elem->GetDirectory()) &&
       !strcmp(GetObjName(), elem->GetObjName())) {
      Long64_t entries = elem->fFirst + elem->fNum;
      if (fFirst < entries) {
         if (fNum == -1) {
            fNum = entries - fFirst;
            fValid = kTRUE;
         } else {
            if (fNum <= entries - fFirst) {
               fValid = kTRUE;
            } else {
               Error("Validate", "TDSetElement requests %d entries starting"
                     " with entry %d, while TDSetElement to validate against"
                     " has only %d entries", fNum, fFirst, entries);
            }
         }
      } else {
         Error("Validate", "TDSetElement to validate against has only %d"
               " entries, but this TDSetElement requested %d as its first"
               " entry", entries, fFirst);
      }
   } else {
      Error("Validate", "TDSetElements do not refer to same objects");
   }
}

//______________________________________________________________________________
Int_t TDSetElement::Compare(const TObject *obj) const
{
   //Compare elements by filename (and the fFirst).

   if (this == obj) return 0;

   const TDSetElement *elem = dynamic_cast<const TDSetElement*>(obj);
   if (!elem) {
      if (obj)
         return fFileName.CompareTo(obj->GetName());
      return -1;
   }

   Int_t order = fFileName.CompareTo(elem->GetFileName());
   if (order == 0) {
      if (GetFirst() < elem->GetFirst())
         return -1;
      else if (GetFirst() > elem->GetFirst())
         return 1;
      return 0;
   }
   return order;
}


//______________________________________________________________________________
void TDSetElement::AddFriend(TDSetElement *friendElement, const char *alias)
{
   // Add friend TDSetElement to this set. The friend element will be copied to this object.

   if (!friendElement) {
      Error("AddFriend", "The friend TDSetElement is null!");
      return;
   }
   if (!fFriends)
      fFriends = new FriendsList_t;
   fFriends->push_back(std::make_pair(new TDSetElement(*friendElement), TString(alias)));
}

//______________________________________________________________________________
void TDSetElement::DeleteFriends()
{
   // Deletes the list of friends and all the friends on the list.
   if (!fFriends)
      return;
   for (FriendsList_t::iterator i = fFriends->begin();
             i != fFriends->end(); ++i) {
      delete i->first;
   }
   delete fFriends;
   fFriends = 0;
}

//______________________________________________________________________________
TDSet::TDSet()
{
   // Default ctor.

   fElements = new TList;
   fElements->SetOwner();
   fIsTree    = kFALSE;
   fIterator  = 0;
   fCurrent   = 0;
   fEventList = 0;

   // Add to the global list
   gROOT->GetListOfDataSets()->Add(this);
}

//______________________________________________________________________________
TDSet::TDSet(const char *name,
             const char *objname, const char *dir, const char *type)
{
   // Create a named TDSet object. The "type" defines the class of which objects
   // will be processed (default 'TTree'). The optional "objname" argument
   // specifies the name of the objects of the specified class.
   // If the "objname" is not given the behaviour depends on the 'type':
   // for 'TTree' the first TTree is analyzed; for other types, all objects of
   // the class found in the specified directory are processed.
   // The "dir" argument specifies in which directory the objects are
   // to be found, the top level directory ("/") is the default.
   // Directories can be specified using wildcards, e.g. "*" or "/*"
   // means to look in all top level directories, "/dir/*" in all
   // directories under "/dir", and "/*/*" to look in all directories
   // two levels deep.
   // For backward compatibility the type can also be passed via 'name',
   // in which case 'type' is ignored.

   fElements = new TList;
   fElements->SetOwner();
   fIterator = 0;
   fCurrent  = 0;
   fEventList = 0;

   fType = "TTree";
   TClass *c = 0;
   // Check name
   if (name && strlen(name) > 0) {
      // In the old constructor signature it was the 'type'
      if (!type) {
         if ((c = gROOT->GetClass(name)))
            fType = name;
         else
            // Default type is 'TTree'
            fName = name;
      } else {
         // Set name
         fName = name;
         // Check type
         if (strlen(type) > 0)
            if ((c = gROOT->GetClass(type)))
               fType = type;
      }
   } else if (type && strlen(type) > 0) {
      // Check the type
      if ((c = gROOT->GetClass(type)))
         fType = type;
   }
   // The correct class type
   c = gROOT->GetClass(fType);

   fIsTree = (c->InheritsFrom("TTree")) ? kTRUE : kFALSE;

   if (objname)
      fObjName = objname;

   if (dir)
      fDir = dir;

   // Default name is the object name
   if (fName.Length() <= 0)
      fName = fObjName;
   // We set the default title to the 'type'
   fTitle = fType;

   // Add to the global list
   gROOT->GetListOfDataSets()->Add(this);
}

//______________________________________________________________________________
TDSet::~TDSet()
{
   // Cleanup.
   delete fElements;
   delete fIterator;

   gROOT->GetListOfDataSets()->Remove(this);
}


//______________________________________________________________________________
Int_t TDSet::Process(const char *selector, Option_t *option, Long64_t nentries,
                     Long64_t first, TEventList *evl)
{
   // Process TDSet on currently active PROOF session.
   // Returns -1 in case of error, 0 otherwise.

   if (!IsValid() || !fElements->GetSize()) {
      Error("Process", "not a correctly initialized TDSet");
      return -1;
   }

   if (gProof)
      return gProof->Process(this, selector, option, nentries, first, evl);

   Error("Process", "no active PROOF session");
   return -1;
}

//______________________________________________________________________________
void TDSet::AddInput(TObject *obj)
{
   // Add objects that might be needed during the processing of
   // the selector (see Process()).

   if (gProof) {
      gProof->AddInput(obj);
   } else {
      Error("AddInput","No PROOF session active");
   }
}

//______________________________________________________________________________
void TDSet::ClearInput()
{
   // Clear input object list.

   if (gProof)
      gProof->ClearInput();
}

//______________________________________________________________________________
TObject *TDSet::GetOutput(const char *name)
{
   // Get specified object that has been produced during the processing
   // (see Process()).

   if (gProof)
      return gProof->GetOutput(name);
   return 0;
}

//______________________________________________________________________________
TList *TDSet::GetOutputList()
{
   // Get list with all object created during processing (see Process()).

   if (gProof)
      return gProof->GetOutputList();
   return 0;
}

//______________________________________________________________________________
void TDSet::Print(const Option_t *opt) const
{
   // Print TDSet basic or full data. When option="a" print full data.

   cout <<"OBJ: " << IsA()->GetName() << "\ttype " << GetName() << "\t"
        << fObjName << "\tin " << GetTitle()
        << "\telements " << GetListOfElements()->GetSize() << endl;

   if (opt && opt[0] == 'a') {
      TIter next(GetListOfElements());
      TObject *obj;
      while ((obj = next())) {
         obj->Print(opt);
      }
   }
}

//______________________________________________________________________________
void TDSet::SetObjName(const char *objname)
{
   // Set/change object name.

   if (objname)
      fObjName = objname;
}

//______________________________________________________________________________
void TDSet::SetDirectory(const char *dir)
{
   // Set/change directory.

   if (dir)
      fDir = dir;
}

//______________________________________________________________________________
Bool_t TDSet::Add(const char *file, const char *objname, const char *dir,
                  Long64_t first, Long64_t num, const char *msd)
{
   // Add file to list of files to be analyzed. Optionally with the
   // objname and dir arguments the default, TDSet wide, objname and
   // dir can be overridden.

   if (!file || !*file) {
      Error("Add", "file name must be specified");
      return kFALSE;
   }

   // check, if it already exists in the TDSet
   TDSetElement *el;
   Reset();
   while ((el = Next())) {
      if (!(strcmp(el->GetFileName(), file))) {
         Warning("Add", "duplicate, %40s is already in dataset, ignored", file);
         return kFALSE;
      }
   }
   if (!objname)
      objname = GetObjName();
   if (!dir)
      dir = GetDirectory();

   fElements->Add(new TDSetElement(file, objname, dir, first, num, msd));

   return kTRUE;
}

//______________________________________________________________________________
Bool_t TDSet::Add(TDSet *dset)
{
   // Add specified data set to the this set.

   if (!dset)
      return kFALSE;

   if (fType != dset->GetType()) {
      Error("Add", "cannot add a set with a different type");
      return kFALSE;
   }

   TDSetElement *el;
   TIter next(dset->fElements);
   TObject *last = (dset == this) ? fElements->Last() : 0;
   while ((el = (TDSetElement*) next())) {
      Add(el->GetFileName(), el->GetObjName(), el->GetDirectory(),
          el->GetFirst(), el->GetNum(), el->GetMsd());
      if (el == last) break;
   }

   return kTRUE;
}

//______________________________________________________________________________
Bool_t TDSet::Add(TList *fileinfo)
{
   // Add files passed as list of TfileInfo objects

   if (!fileinfo)
      return kFALSE;

   TFileInfo *fi = 0;
   TIter next(fileinfo);
   while ((fi = (TFileInfo *) next())) {
      Add(fi->GetFirstUrl()->GetUrl(kTRUE), 0, 0,
          fi->GetFirst(), fi->GetEntries());
   }

   return kTRUE;
}

//______________________________________________________________________________
Int_t TDSet::ExportFileList(const char *fpath, Option_t *opt)
{
   // Export TDSetElements files as list of TFileInfo objects in file
   // 'fpath'. If the file exists already the action fails, unless
   // 'opt == "F"'.
   // Return 0 on success, -1 otherwise

   if (!fElements)
      return -1;
   if (fElements->GetSize() <= 0)
      return 0;

   Bool_t force = (opt[0] == 'F' || opt[0] == 'f');

   if (gSystem->AccessPathName(fpath, kFileExists) == kFALSE) {
      if (force) {
         // Try removing the file
         if (gSystem->Unlink(fpath)) {
            Info("ExportFileList","error removing dataset file: %s", fpath);
            return -1;
         }
      }
   }

   // Create the file list
   TList *fileinfo = new TList;
   fileinfo->SetOwner();
   TFileInfo *fi = 0;

   TDSetElement *dse = 0;
   TIter next(fElements);
   while ((dse = (TDSetElement *) next())) {
      Long64_t last = (dse->GetNum() > 0) ? dse->GetFirst()+dse->GetNum()-1 : -1;
      fi = new TFileInfo(dse->GetFileName(), (Long64_t)(-1), 0,
                         0, dse->GetEntries(), dse->GetFirst(),
                         last);
      fileinfo->Add(fi);
   }

   // Write to file
   TFile *f = TFile::Open(fpath, "RECREATE");
   if (f) {
      f->cd();
      fileinfo->Write("fileList", TObject::kSingleKey);
      f->Close();
   } else {
      Info("ExportFileList","error creating dataset file: %s", fpath);
      SafeDelete(fileinfo);
      return -1;
   }

   // Cleanup
   SafeDelete(f);
   SafeDelete(fileinfo);

   // We are done
   return 0;
}

//______________________________________________________________________________
void TDSet::AddFriend(TDSet *friendset, const char* alias)
{
   // Add friend dataset to this set. Only possible if the TDSet type is
   // a TTree or derived class. The friendset will be owned by this class
   // and deleted in its destructor.

   if (!friendset) {
      Error("AddFriend", "The friend TDSet is null!");
      return;
   }

   if (!fIsTree) {
      Error("AddFriend", "a friend set can only be added to a TTree TDSet");
      return;
   }
   TList* thisList = GetListOfElements();
   TList* friendsList = friendset->GetListOfElements();
   if (thisList->GetSize() != friendsList->GetSize() && friendsList->GetSize() != 1) {
      Error("AddFriend", "The friend Set has %d elements while the main one has %d",
            thisList->GetSize(), friendsList->GetSize());
      return;
   }
   TIter next(thisList);
   TIter next2(friendsList);
   TDSetElement* friendElem = 0;
   TString aliasString(alias);
   if (friendsList->GetSize() == 1)
      friendElem = dynamic_cast<TDSetElement*> (friendsList->First());
   while(TDSetElement* e = dynamic_cast<TDSetElement*> (next())) {
      if (friendElem) // just one elem in the friend TDSet
         e->AddFriend(friendElem, aliasString);
      else
         e->AddFriend(dynamic_cast<TDSetElement*> (next2()), aliasString);
   }
}

//______________________________________________________________________________
void TDSet::Reset()
{
   // Reset or initialize access to the elements.

   if (!fIterator) {
      fIterator = new TIter(fElements);
   } else {
      fIterator->Reset();
   }
}

//______________________________________________________________________________
TDSetElement *TDSet::Next(Long64_t /*totalEntries*/)
{
   // Returns next TDSetElement.

   if (!fIterator) {
      fIterator = new TIter(fElements);
   }

   fCurrent = (TDSetElement *) fIterator->Next();
   return fCurrent;
}

//______________________________________________________________________________
Long64_t TDSet::GetEntries(Bool_t isTree, const char *filename, const char *path,
                           const char *objname)
{
   // Returns number of entries in tree or objects in file. Returns -1 in
   // case of error.

   Double_t start = 0;
   if (gPerfStats != 0) start = TTimeStamp();

   TFile *file = TFile::Open(filename);

   if (gPerfStats != 0) {
      gPerfStats->FileOpenEvent(file, filename, double(TTimeStamp())-start);
   }

   if (file == 0) {
      ::SysError("TDSet::GetEntries", "cannot open file %s", filename);
      return -1;
   }

   TDirectory *dirsave = gDirectory;
   if (!file->cd(path)) {
      ::Error("TDSet::GetEntries", "cannot cd to %s", path);
      delete file;
      return -1;
   }

   TDirectory *dir = gDirectory;
   dirsave->cd();

   Long64_t entries;
   if (isTree) {

      TString on(objname);
      TString sreg(objname);
      // If a wild card we will use the first object of the type
      // requested compatible with the reg expression we got
      if (sreg.Length() <= 0 || sreg == "" || sreg.Contains("*")) {
         if (sreg.Contains("*"))
            sreg.ReplaceAll("*", ".*");
         else
            sreg = ".*";
         TRegexp re(sreg);
         if (dir->GetListOfKeys()) {
            TIter nxk(dir->GetListOfKeys());
            TKey *k = 0;
            Bool_t notfound = kTRUE;
            while ((k = (TKey *) nxk())) {
               if (!strcmp(k->GetClassName(), "TTree")) {
                  TString kn(k->GetName());
                  if (kn.Index(re) != kNPOS) {
                     if (notfound) {
                        on = kn;
                        notfound = kFALSE;
                     } else if (kn != on) {
                       ::Warning("TDSet::GetEntries",
                                 "additional tree found in the file: %s", kn.Data());
                     }
                  }
               }
            }
         }
      }

      TKey *key = dir->GetKey(on);
      if (key == 0) {
         ::Error("TDSet::GetEntries", "cannot find tree \"%s\" in %s",
                 objname, filename);
         delete file;
         return -1;
      }
      TTree *tree = (TTree *) key->ReadObj();
      if (tree == 0) {
         // Error always reported?
         delete file;
         return -1;
      }
      entries = tree->GetEntries();
      delete tree;

   } else {
      TList *keys = dir->GetListOfKeys();
      entries = keys->GetSize();
   }

   delete file;
   return entries;
}

//______________________________________________________________________________
Int_t TDSet::Draw(const char *varexp, const TCut &selection, Option_t *option,
                  Long64_t nentries, Long64_t firstentry)
{
   // Draw expression varexp for specified entries.
   // This function accepts a TCut objects as argument.
   // Use the operator+ to concatenate cuts.
   // Example:
   //   dset.Draw("x",cut1+cut2+cut3);

   return Draw(varexp, selection.GetTitle(), option, nentries, firstentry);
}

//______________________________________________________________________________
Int_t TDSet::Draw(const char *varexp, const char *selection, Option_t *option,
                  Long64_t nentries, Long64_t firstentry)
{
   // Draw expression varexp for specified entries.
   // See TTree::Draw().

   if (!IsValid() || !fElements->GetSize()) {
      Error("Draw", "not a correctly initialized TDSet");
      return -1;
   }

   if (gProof)
      return gProof->DrawSelect(this, varexp, selection, option, nentries,
                                firstentry);

   Error("Draw", "no active PROOF session");
   return -1;
}

//_______________________________________________________________________
void TDSet::StartViewer()
{
   // Start the TTreeViewer on this TTree.

   if (gROOT->IsBatch()) {
      Warning("StartViewer", "viewer cannot run in batch mode");
      return;
   }

   if (!gProof) {
      Error("StartViewer", "no PROOF found");
      return;
   }
   if (!IsTree()) {
      Error("StartViewer", "TDSet contents should be of type TTree (or subtype)");
      return;
   }
   TChainProof *w = TChainProof::MakeChainProof(this, gProof);
   // TODO: w should be freed somewhere, probably in the TTreeViewer destructor.
   if (!w) {
      Error("StartViewer", "failure creating a TChainProof");
      return;
   }

   TPluginHandler *h;
   if ((h = gROOT->GetPluginManager()->FindHandler("TVirtualTreeViewer"))) {
      if (h->LoadPlugin() == -1)
         return;
      h->ExecPlugin(1,w);
   }
}

//_______________________________________________________________________
TTree* TDSet::GetTreeHeader(TVirtualProof* proof)
{
   // Returns a tree header containing the branches' structure of the dataset.

   return proof->GetTreeHeader(this);
}

//______________________________________________________________________________
Bool_t TDSet::ElementsValid() const
{
   // Check if all elemnts are valid.

   TIter nextElem(GetListOfElements());
   while (TDSetElement *elem = dynamic_cast<TDSetElement*>(nextElem())) {
      if (!elem->GetValid()) return kFALSE;
   }
   return kTRUE;
}

//______________________________________________________________________________
void TDSet::Validate()
{
   // Validate the TDSet by opening files.

   TIter nextElem(GetListOfElements());
   while (TDSetElement *elem = dynamic_cast<TDSetElement*>(nextElem())) {
      if (!elem->GetValid()) elem->Validate(IsTree());
   }
}

//______________________________________________________________________________
void TDSet::Validate(TDSet* dset)
{
   // Validate the TDSet against another TDSet.
   // Only validates elements in common from input TDSet.

   THashList bestElements;
   bestElements.SetOwner();
   TList namedHolder;
   namedHolder.SetOwner();
   TIter nextOtherElem(dset->GetListOfElements());
   while (TDSetElement *elem = dynamic_cast<TDSetElement*>(nextOtherElem())) {
      if (!elem->GetValid()) continue;
      TString dir_file_obj = elem->GetDirectory();
      dir_file_obj += "_";
      dir_file_obj += elem->GetFileName();
      dir_file_obj += "_";
      dir_file_obj += elem->GetObjName();
      TPair *p = dynamic_cast<TPair*>(bestElements.FindObject(dir_file_obj));
      if (p) {
         TDSetElement *prevelem = dynamic_cast<TDSetElement*>(p->Value());
         Long64_t entries = prevelem->GetFirst()+prevelem->GetNum();
         if (entries<elem->GetFirst()+elem->GetNum()) {
            bestElements.Remove(p);
            bestElements.Add(new TPair(p->Key(), elem));
            delete p;
         }
      } else {
         TNamed* named = new TNamed(dir_file_obj, dir_file_obj);
         namedHolder.Add(named);
         bestElements.Add(new TPair(named, elem));
      }
   }

   TIter nextElem(GetListOfElements());
   while (TDSetElement *elem = dynamic_cast<TDSetElement*>(nextElem())) {
      if (!elem->GetValid()) {
         TString dir_file_obj = elem->GetDirectory();
         dir_file_obj += "_";
         dir_file_obj += elem->GetFileName();
         dir_file_obj += "_";
         dir_file_obj += elem->GetObjName();
         if (TPair *p = dynamic_cast<TPair*>(bestElements.FindObject(dir_file_obj))) {
            TDSetElement* validelem = dynamic_cast<TDSetElement*>(p->Value());
            elem->Validate(validelem);
         }
      }
   }
}
