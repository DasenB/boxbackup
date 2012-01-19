// --------------------------------------------------------------------------
//
// File
//		Name:    BackupClientDirectoryRecord.cpp
//		Purpose: Implementation of record about directory for
//			 backup client
//		Created: 2003/10/08
//
// --------------------------------------------------------------------------

#include "Box.h"

#ifdef HAVE_DIRENT_H
	#include <dirent.h>
#endif

#include <errno.h>
#include <string.h>

#include "BackupClientDirectoryRecord.h"
#include "autogen_BackupProtocolClient.h"
#include "BackupClientContext.h"
#include "IOStream.h"
#include "MemBlockStream.h"
#include "CommonException.h"
#include "CollectInBufferStream.h"
#include "BackupStoreFile.h"
#include "BackupClientInodeToIDMap.h"
#include "FileModificationTime.h"
#include "BackupDaemon.h"
#include "BackupStoreException.h"
#include "Archive.h"
#include "PathUtils.h"
#include "Logging.h"
#include "ReadLoggingStream.h"

#include "MemLeakFindOn.h"

typedef std::map<std::string, BackupStoreDirectory::Entry *> DecryptedEntriesMap_t;

// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientDirectoryRecord::BackupClientDirectoryRecord()
//		Purpose: Constructor
//		Created: 2003/10/08
//
// --------------------------------------------------------------------------
BackupClientDirectoryRecord::BackupClientDirectoryRecord(int64_t ObjectID, const std::string &rSubDirName)
	: mObjectID(ObjectID),
	  mSubDirName(rSubDirName),
	  mInitialSyncDone(false),
	  mSyncDone(false),
	  mpPendingEntries(0)
{
	::memset(mStateChecksum, 0, sizeof(mStateChecksum));
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientDirectoryRecord::~BackupClientDirectoryRecord()
//		Purpose: Destructor
//		Created: 2003/10/08
//
// --------------------------------------------------------------------------
BackupClientDirectoryRecord::~BackupClientDirectoryRecord()
{
	// Make deletion recursive
	DeleteSubDirectories();
	
	// Delete maps
	if(mpPendingEntries != 0)
	{
		delete mpPendingEntries;
		mpPendingEntries = 0;
	}
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientDirectoryRecord::DeleteSubDirectories();
//		Purpose: Delete all sub directory entries
//		Created: 2003/10/09
//
// --------------------------------------------------------------------------
void BackupClientDirectoryRecord::DeleteSubDirectories()
{
	// Delete all pointers
	for(std::map<std::string, BackupClientDirectoryRecord *>::iterator i = mSubDirectories.begin();
		i != mSubDirectories.end(); ++i)
	{
		delete i->second;
	}
	
	// Empty list
	mSubDirectories.clear();
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientDirectoryRecord::SyncDirectory(i
//			 BackupClientDirectoryRecord::SyncParams &,
//			 int64_t, const std::string &,
//			 const std::string &, bool)
//		Purpose: Recursively synchronise a local directory
//			 with the server.
//		Created: 2003/10/08
//
// --------------------------------------------------------------------------
void BackupClientDirectoryRecord::SyncDirectory(
	BackupClientDirectoryRecord::SyncParams &rParams,
	int64_t ContainingDirectoryID,
	const std::string &rLocalPath,
	const std::string &rRemotePath,
	bool ThisDirHasJustBeenCreated)
{
	BackupClientContext& rContext(rParams.mrContext);
	ProgressNotifier& rNotifier(rContext.GetProgressNotifier());

	// Signal received by daemon?
	if(rParams.mrRunStatusProvider.StopRun())
	{
		// Yes. Stop now.
		THROW_EXCEPTION(BackupStoreException, SignalReceived)
	}

	// Start by making some flag changes, marking this sync as not done,
	// and on the immediate sub directories.
	mSyncDone = false;
	for(std::map<std::string, BackupClientDirectoryRecord *>::iterator
		i  = mSubDirectories.begin();
		i != mSubDirectories.end(); ++i)
	{
		i->second->mSyncDone = false;
	}

	// Work out the time in the future after which the file should
	// be uploaded regardless. This is a simple way to avoid having
	// too many problems with file servers when they have clients
	// with badly out of sync clocks.
	rParams.mUploadAfterThisTimeInTheFuture = GetCurrentBoxTime() +
		rParams.mMaxFileTimeInFuture;
	
	// Build the current state checksum to compare against while
	// getting info from dirs. Note checksum is used locally only,
	// so byte order isn't considered.
	MD5Digest currentStateChecksum;
	
	EMU_STRUCT_STAT dest_st;
	// Stat the directory, to get attribute info
	// If it's a symbolic link, we want the link target here
	// (as we're about to back up the contents of the directory)
	{
		if(EMU_STAT(rLocalPath.c_str(), &dest_st) != 0)
		{
			// The directory has probably been deleted, so
			// just ignore this error. In a future scan, this
			// deletion will be noticed, deleted from server,
			// and this object deleted.
			rNotifier.NotifyDirStatFailed(this, rLocalPath,
				strerror(errno));
			return;
		}
		// Store inode number in map so directories are tracked
		// in case they're renamed
		{
			BackupClientInodeToIDMap &idMap(
				rParams.mrContext.GetNewIDMap());
			idMap.AddToMap(dest_st.st_ino, mObjectID,
				ContainingDirectoryID);
		}
		// Add attributes to checksum
		currentStateChecksum.Add(&dest_st.st_mode,
			sizeof(dest_st.st_mode));
		currentStateChecksum.Add(&dest_st.st_uid,
			sizeof(dest_st.st_uid));
		currentStateChecksum.Add(&dest_st.st_gid,
			sizeof(dest_st.st_gid));
		// Inode to be paranoid about things moving around
		currentStateChecksum.Add(&dest_st.st_ino,
			sizeof(dest_st.st_ino));
#ifdef HAVE_STRUCT_STAT_ST_FLAGS
		currentStateChecksum.Add(&dest_st.st_flags,
			sizeof(dest_st.st_flags));
#endif

		StreamableMemBlock xattr;
		BackupClientFileAttributes::FillExtendedAttr(xattr,
			rLocalPath.c_str());
		currentStateChecksum.Add(xattr.GetBuffer(), xattr.GetSize());
	}
	
	// Read directory entries, building arrays of names
	// First, need to read the contents of the directory.
	std::vector<std::string> dirs;
	std::vector<std::string> files;
	bool downloadDirectoryRecordBecauseOfFutureFiles = false;

	EMU_STRUCT_STAT link_st;
	if(EMU_LSTAT(rLocalPath.c_str(), &link_st) != 0)
	{
		// Report the error (logs and 
		// eventual email to administrator)
		rNotifier.NotifyFileStatFailed(this, rLocalPath,
			strerror(errno));
		
		// FIXME move to NotifyFileStatFailed()
		SetErrorWhenReadingFilesystemObject(rParams, 
			rLocalPath.c_str());
		
		// This shouldn't happen, so we'd better not continue
		THROW_EXCEPTION(CommonException, OSFileError)
	}

	// BLOCK
	{		
		// read the contents...
		DIR *dirHandle = 0;
		try
		{
			rNotifier.NotifyScanDirectory(this, rLocalPath);

			dirHandle = ::opendir(rLocalPath.c_str());
			if(dirHandle == 0)
			{
				// Report the error (logs and 
				// eventual email to administrator)
				if (errno == EACCES)
				{
					rNotifier.NotifyDirListFailed(this,
						rLocalPath, "Access denied");
				}
				else
				{
					rNotifier.NotifyDirListFailed(this, 
						rLocalPath, strerror(errno));
				}
				
				// Report the error (logs and eventual email
				// to administrator)
				SetErrorWhenReadingFilesystemObject(rParams,
					rLocalPath.c_str());
				// Ignore this directory for now.
				return;
			}
			
			// Basic structure for checksum info
			struct {
				box_time_t mModificationTime;
				box_time_t mAttributeModificationTime;
				int64_t mSize;
				// And then the name follows
			} checksum_info;
			// Be paranoid about structure packing
			::memset(&checksum_info, 0, sizeof(checksum_info));
	
			struct dirent *en = 0;
			EMU_STRUCT_STAT file_st;
			std::string filename;
			while((en = ::readdir(dirHandle)) != 0)
			{
				rParams.mrContext.DoKeepAlive();
				
				// Don't need to use
				// LinuxWorkaround_FinishDirentStruct(en,
				// rLocalPath.c_str());
				// on Linux, as a stat is performed to
				// get all this info

				if(en->d_name[0] == '.' && 
					(en->d_name[1] == '\0' || (en->d_name[1] == '.' && en->d_name[2] == '\0')))
				{
					// ignore, it's . or ..
					continue;
				}

				// Stat file to get info
				filename = MakeFullPath(rLocalPath, en->d_name);

				#ifdef WIN32
				// Don't stat the file just yet, to ensure
				// that users can exclude unreadable files
				// to suppress warnings that they are
				// not accessible.
				//
				// Our emulated readdir() abuses en->d_type, 
				// which would normally contain DT_REG, 
				// DT_DIR, etc, but we only use it here and 
				// prefer S_IFREG, S_IFDIR...
				int type = 0;
				type = (DT_DIR == en->d_type) ? S_IFDIR : type;
				type = (DT_REG == en->d_type) ? S_IFREG : type;
				#else
				if(EMU_LSTAT(filename.c_str(), &file_st) != 0)
				{
					if(!(rParams.mrContext.ExcludeDir(
						filename)))
					{
						// Report the error (logs and 
						// eventual email to
						// administrator)
 						rNotifier.NotifyFileStatFailed(
							this, filename,
							strerror(errno));
					
						// FIXME move to
						// NotifyFileStatFailed()
						SetErrorWhenReadingFilesystemObject(
							rParams, filename.c_str());
					}

					// Ignore this entry for now.
					continue;
				}

				if(file_st.st_dev != dest_st.st_dev)
				{
					if(!(rParams.mrContext.ExcludeDir(
						filename)))
					{
						rNotifier.NotifyMountPointSkipped(
							this, filename);
					}
					continue;
				}

				int type = file_st.st_mode & S_IFMT;
				#endif

				if(type == S_IFREG || type == S_IFLNK)
				{
					// File or symbolic link

					// Exclude it?
					if(rParams.mrContext.ExcludeFile(filename))
					{
 						rNotifier.NotifyFileExcluded(
								this, 
								filename);

						// Next item!
						continue;
					}

					// Store on list
					files.push_back(std::string(en->d_name));
				}
				else if(type == S_IFDIR)
				{
					// Directory

					// Exclude it?
					if(rParams.mrContext.ExcludeDir(filename))
					{
 						rNotifier.NotifyDirExcluded(
								this, 
								filename);

						// Next item!
						continue;
					}

					// Store on list
					dirs.push_back(std::string(en->d_name));
				}
				else if (type == S_IFSOCK || type == S_IFIFO)
				{
					// removed notification for these types
					// see Debian bug 479145, no objections
				}
				else
				{
					if(rParams.mrContext.ExcludeFile(filename))
					{
 						rNotifier.NotifyFileExcluded(
								this, 
								filename);
					}
					else
					{
 						rNotifier.NotifyUnsupportedFileType(
								this, filename);
						SetErrorWhenReadingFilesystemObject(
							rParams, filename.c_str());
					}

					continue;
				}
				
				// Here if the object is something to back up (file, symlink or dir, not excluded)
				// So make the information for adding to the checksum
				
				#ifdef WIN32
				// We didn't stat the file before,
				// but now we need the information.
				if(emu_stat(filename.c_str(), &file_st) != 0)
				{
 					rNotifier.NotifyFileStatFailed(this, 
 							filename, 
							strerror(errno));
					
					// Report the error (logs and 
					// eventual email to administrator)
					SetErrorWhenReadingFilesystemObject(
						rParams, filename.c_str());

					// Ignore this entry for now.
					continue;
				}

				if(file_st.st_dev != link_st.st_dev)
				{
 					rNotifier.NotifyMountPointSkipped(this, 
 							filename);
					continue;
				}
				#endif

				checksum_info.mModificationTime = FileModificationTime(file_st);
				checksum_info.mAttributeModificationTime = FileAttrModificationTime(file_st);
				checksum_info.mSize = file_st.st_size;
				currentStateChecksum.Add(&checksum_info, sizeof(checksum_info));
				currentStateChecksum.Add(en->d_name, strlen(en->d_name));
				
				// If the file has been modified madly into the future, download the 
				// directory record anyway to ensure that it doesn't get uploaded
				// every single time the disc is scanned.
				if(checksum_info.mModificationTime > rParams.mUploadAfterThisTimeInTheFuture)
				{
					downloadDirectoryRecordBecauseOfFutureFiles = true;
					// Log that this has happened
					if(!rParams.mHaveLoggedWarningAboutFutureFileTimes)
					{
						rNotifier.NotifyFileModifiedInFuture(
							this, filename);
						rParams.mHaveLoggedWarningAboutFutureFileTimes = true;
					}
				}
			}
	
			if(::closedir(dirHandle) != 0)
			{
				THROW_EXCEPTION(CommonException, OSFileError)
			}
			dirHandle = 0;
		}
		catch(...)
		{
			if(dirHandle != 0)
			{
				::closedir(dirHandle);
			}
			throw;
		}
	}

	// Finish off the checksum, and compare with the one currently stored
	bool checksumDifferent = true;
	currentStateChecksum.Finish();
	if(mInitialSyncDone && currentStateChecksum.DigestMatches(mStateChecksum))
	{
		// The checksum is the same, and there was one to compare with
		checksumDifferent = false;
	}

	// Pointer to potentially downloaded store directory info
	BackupStoreDirectory *pdirOnStore = 0;
	
	try
	{
		// Want to get the directory listing?
		if(ThisDirHasJustBeenCreated)
		{
			// Avoid sending another command to the server when we know it's empty
			pdirOnStore = new BackupStoreDirectory(mObjectID, ContainingDirectoryID);
		}
		else
		{
			// Consider asking the store for it
			if(!mInitialSyncDone || checksumDifferent || downloadDirectoryRecordBecauseOfFutureFiles)
			{
				pdirOnStore = FetchDirectoryListing(rParams);
			}
		}
				
		// Make sure the attributes are up to date -- if there's space on the server
		// and this directory has not just been created (because it's attributes will be correct in this case)
		// and the checksum is different, implying they *MIGHT* be different.
		if((!ThisDirHasJustBeenCreated) && checksumDifferent && (!rParams.mrContext.StorageLimitExceeded()))
		{
			UpdateAttributes(rParams, pdirOnStore, rLocalPath);
		}
		
		// Create the list of pointers to directory entries
		std::vector<BackupStoreDirectory::Entry *> entriesLeftOver;
		if(pdirOnStore)
		{
			entriesLeftOver.resize(pdirOnStore->GetNumberOfEntries(), 0);
			BackupStoreDirectory::Iterator i(*pdirOnStore);
			// Copy in pointers to all the entries
			for(unsigned int l = 0; l < pdirOnStore->GetNumberOfEntries(); ++l)
			{
				entriesLeftOver[l] = i.Next();
			}
		}
		
		// Do the directory reading
		bool updateCompleteSuccess = UpdateItems(rParams, rLocalPath,
			rRemotePath, pdirOnStore, entriesLeftOver, files, dirs);
		
		// LAST THING! (think exception safety)
		// Store the new checksum -- don't fetch things unnecessarily in the future
		// But... only if 1) the storage limit isn't exceeded -- make sure things are done again if
		// the directory is modified later
		// and 2) All the objects within the directory were stored successfully.
		if(!rParams.mrContext.StorageLimitExceeded() && updateCompleteSuccess)
		{
			currentStateChecksum.CopyDigestTo(mStateChecksum);
		}
	}
	catch(...)
	{
		// Bad things have happened -- clean up
		if(pdirOnStore != 0)
		{
			delete pdirOnStore;
			pdirOnStore = 0;
		}
		
		// Set things so that we get a full go at stuff later
		::memset(mStateChecksum, 0, sizeof(mStateChecksum));
		
		throw;
	}
	
	// Clean up directory on store
	if(pdirOnStore != 0)
	{
		delete pdirOnStore;
		pdirOnStore = 0;
	}
	
	// Flag things as having happened.
	mInitialSyncDone = true;
	mSyncDone = true;
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientDirectoryRecord::FetchDirectoryListing(BackupClientDirectoryRecord::SyncParams &)
//		Purpose: Fetch the directory listing of this directory from the store.
//		Created: 2003/10/09
//
// --------------------------------------------------------------------------
BackupStoreDirectory *BackupClientDirectoryRecord::FetchDirectoryListing(BackupClientDirectoryRecord::SyncParams &rParams)
{
	BackupStoreDirectory *pdir = 0;
	
	try
	{
		// Get connection to store
		BackupProtocolClient &connection(rParams.mrContext.GetConnection());

		// Query the directory
		std::auto_ptr<BackupProtocolClientSuccess> dirreply(connection.QueryListDirectory(
				mObjectID,
				BackupProtocolClientListDirectory::Flags_INCLUDE_EVERYTHING,	// both files and directories
				BackupProtocolClientListDirectory::Flags_Deleted | BackupProtocolClientListDirectory::Flags_OldVersion, // exclude old/deleted stuff
				true /* want attributes */));

		// Retrieve the directory from the stream following
		pdir = new BackupStoreDirectory;
		ASSERT(pdir != 0);
		std::auto_ptr<IOStream> dirstream(connection.ReceiveStream());
		pdir->ReadFromStream(*dirstream, connection.GetTimeout());
	}
	catch(...)
	{
		delete pdir;
		pdir = 0;
		throw;
	}
	
	return pdir;
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientDirectoryRecord::UpdateAttributes(BackupClientDirectoryRecord::SyncParams &, const std::string &)
//		Purpose: Sets the attributes of the directory on the store, if necessary
//		Created: 2003/10/09
//
// --------------------------------------------------------------------------
void BackupClientDirectoryRecord::UpdateAttributes(BackupClientDirectoryRecord::SyncParams &rParams, BackupStoreDirectory *pDirOnStore, const std::string &rLocalPath)
{
	// Get attributes for the directory
	BackupClientFileAttributes attr;
	box_time_t attrModTime = 0;
	attr.ReadAttributes(rLocalPath.c_str(), true /* directories have zero mod times */,
		0 /* no modification time */, &attrModTime);

	// Assume attributes need updating, unless proved otherwise
	bool updateAttr = true;

	// Got a listing to compare with?
	ASSERT(pDirOnStore == 0 || (pDirOnStore != 0 && pDirOnStore->HasAttributes()));
	if(pDirOnStore != 0 && pDirOnStore->HasAttributes())
	{
		const StreamableMemBlock &storeAttrEnc(pDirOnStore->GetAttributes());
		// Explict decryption
		BackupClientFileAttributes storeAttr(storeAttrEnc);
		
		// Compare the attributes
		if(attr.Compare(storeAttr, true,
			true /* ignore both modification times */))
		{
			// No update necessary
			updateAttr = false;
		}
	}

	// Update them?
	if(updateAttr)
	{
		// Get connection to store
		BackupProtocolClient &connection(rParams.mrContext.GetConnection());

		// Exception thrown if this doesn't work
		MemBlockStream attrStream(attr);
		connection.QueryChangeDirAttributes(mObjectID, attrModTime, attrStream);
	}
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientDirectoryRecord::UpdateItems(BackupClientDirectoryRecord::SyncParams &, const std::string &, BackupStoreDirectory *, std::vector<BackupStoreDirectory::Entry *> &)
//		Purpose: Update the items stored on the server. The rFiles vector will be erased after it's used to save space.
//				 Returns true if all items were updated successfully. (If not, the failures will have been logged).
//		Created: 2003/10/09
//
// --------------------------------------------------------------------------
bool BackupClientDirectoryRecord::UpdateItems(
	BackupClientDirectoryRecord::SyncParams &rParams,
	const std::string &rLocalPath,
	const std::string &rRemotePath,
	BackupStoreDirectory *pDirOnStore,
	std::vector<BackupStoreDirectory::Entry *> &rEntriesLeftOver,
	std::vector<std::string> &rFiles,
	const std::vector<std::string> &rDirs)
{
	BackupClientContext& rContext(rParams.mrContext);
	ProgressNotifier& rNotifier(rContext.GetProgressNotifier());

	bool allUpdatedSuccessfully = true;

	// Decrypt all the directory entries.
	// It would be nice to be able to just compare the encrypted versions, however this doesn't work
	// in practise because there can be multiple encodings of the same filename using different 
	// methods (although each method will result in the same string for the same filename.) This
	// happens when the server fixes a broken store, and gives plain text generated filenames.
	// So if we didn't do things like this, then you wouldn't be able to recover from bad things
	// happening with the server.
	DecryptedEntriesMap_t decryptedEntries;
	if(pDirOnStore != 0)
	{
		BackupStoreDirectory::Iterator i(*pDirOnStore);
		BackupStoreDirectory::Entry *en = 0;
		while((en = i.Next()) != 0)
		{
			decryptedEntries[BackupStoreFilenameClear(en->GetName()).GetClearFilename()] = en;
		}
	}

	// Do files
	for(std::vector<std::string>::const_iterator f = rFiles.begin();
		f != rFiles.end(); ++f)
	{
		// Send keep-alive message if needed
		rContext.DoKeepAlive();
		
		// Filename of this file
		std::string filename(MakeFullPath(rLocalPath, *f));

		// Get relevant info about file
		box_time_t modTime = 0;
		uint64_t attributesHash = 0;
		int64_t fileSize = 0;
		InodeRefType inodeNum = 0;
		bool hasMultipleHardLinks = true;
		// BLOCK
		{
			// Stat the file
			EMU_STRUCT_STAT st;
			if(EMU_LSTAT(filename.c_str(), &st) != 0)
			{
				rNotifier.NotifyFileStatFailed(this, 
					filename, strerror(errno));

				// Report the error (logs and 
				// eventual email to administrator)
				SetErrorWhenReadingFilesystemObject(rParams, 
					filename.c_str());

				// Ignore this entry for now.
				continue;
			}
			
			// Extract required data
			modTime = FileModificationTime(st);
			fileSize = st.st_size;
			inodeNum = st.st_ino;
			hasMultipleHardLinks = (st.st_nlink > 1);
			attributesHash = BackupClientFileAttributes::GenerateAttributeHash(st, filename, *f);
		}

		// See if it's in the listing (if we have one)
		BackupStoreFilenameClear storeFilename(*f);
		BackupStoreDirectory::Entry *en = 0;
		int64_t latestObjectID = 0;
		if(pDirOnStore != 0)
		{
			DecryptedEntriesMap_t::iterator i(decryptedEntries.find(*f));
			if(i != decryptedEntries.end())
			{
				en = i->second;
				latestObjectID = en->GetObjectID();
			}
		}

		// Check that the entry which might have been found is in fact a file
		if((en != 0) && ((en->GetFlags() & BackupStoreDirectory::Entry::Flags_File) == 0))
		{
			// Directory exists in the place of this file -- sort it out
			RemoveDirectoryInPlaceOfFile(rParams, pDirOnStore,
				en, *f);
			en = 0;
		}
		
		// Check for renaming?
		if(pDirOnStore != 0 && en == 0)
		{
			// We now know...
			// 1) File has just been added
			// 2) It's not in the store
			
			// Do we know about the inode number?
			const BackupClientInodeToIDMap &idMap(rContext.GetCurrentIDMap());
			int64_t renameObjectID = 0, renameInDirectory = 0;
			if(idMap.Lookup(inodeNum, renameObjectID, renameInDirectory))
			{
				// Look up on the server to get the name, to build the local filename
				std::string localPotentialOldName;
				bool isDir = false;
				bool isCurrentVersion = false;
				box_time_t srvModTime = 0, srvAttributesHash = 0;
				BackupStoreFilenameClear oldLeafname;
				if(rContext.FindFilename(renameObjectID, renameInDirectory, localPotentialOldName, isDir, isCurrentVersion, &srvModTime, &srvAttributesHash, &oldLeafname))
				{	
					// Only interested if it's a file and the latest version
					if(!isDir && isCurrentVersion)
					{
						// Check that the object we found in the ID map doesn't exist on disc
						EMU_STRUCT_STAT st;
						if(EMU_STAT(localPotentialOldName.c_str(), &st) != 0 && errno == ENOENT)
						{
							// Doesn't exist locally, but does exist on the server.
							// Therefore we can safely rename it to this new file.

							// Get the connection to the server 
							BackupProtocolClient &connection(rContext.GetConnection());

							// Only do this step if there is room on the server.
							// This step will be repeated later when there is space available
							if(!rContext.StorageLimitExceeded())
							{
								// Rename the existing files (ie include old versions) on the server
								connection.QueryMoveObject(renameObjectID, renameInDirectory, mObjectID /* move to this directory */,
									BackupProtocolClientMoveObject::Flags_MoveAllWithSameName | BackupProtocolClientMoveObject::Flags_AllowMoveOverDeletedObject,
									storeFilename);
									
								// Stop the attempt to delete the file in the original location
								BackupClientDeleteList &rdelList(rContext.GetDeleteList());
								rdelList.StopFileDeletion(renameInDirectory, oldLeafname);
								
								// Create new entry in the directory for it
								// -- will be near enough what's actually on the server for the rest to work.
								en = pDirOnStore->AddEntry(storeFilename, srvModTime, renameObjectID, 0 /* size in blocks unknown, but not needed */,
									BackupStoreDirectory::Entry::Flags_File, srvAttributesHash);
							
								// Store the object ID for the inode lookup map later
								latestObjectID = renameObjectID;
							}
						}
					}
				}
			}
		}
		
		// Is it in the mPendingEntries list?
		box_time_t pendingFirstSeenTime = 0;		// ie not seen
		if(mpPendingEntries != 0)
		{
			std::map<std::string, box_time_t>::const_iterator i(mpPendingEntries->find(*f));
			if(i != mpPendingEntries->end())
			{
				// found it -- set flag
				pendingFirstSeenTime = i->second;
			}
		}
		
		// If pDirOnStore == 0, then this must have been after an initial sync:
		ASSERT(pDirOnStore != 0 || mInitialSyncDone);
		// So, if pDirOnStore == 0, then we know that everything before syncPeriodStart
		// is either on the server, or in the toupload list. If the directory had changed,
		// we'd have got a directory listing.
		//
		// At this point, if (pDirOnStore == 0 && en == 0), we can assume it's on the server with a
		// mod time < syncPeriodStart, or didn't exist before that time.
		//
		// But if en != 0, then we need to compare modification times to avoid uploading it again.

		// Need to update?
		//
		// Condition for upload:
		//    modification time within sync period
		//    if it's been seen before but not uploaded, is the time from this first sight longer than the MaxUploadWait
		//	  and if we know about it from a directory listing, that it hasn't got the same upload time as on the store

		bool doUpload = false;

		// Only upload a file if the mod time locally is 
		// different to that on the server.

		if (en == 0 || en->GetModificationTime() != modTime)
		{
			// Check the file modified within the acceptable time period we're checking
			// If the file isn't on the server, the acceptable time starts at zero.
			// Check pDirOnStore and en, because if we didn't download a directory listing,
			// pDirOnStore will be zero, but we know it's on the server.
			if (modTime < rParams.mSyncPeriodEnd)
			{
				if (pDirOnStore != 0 && en == 0)
				{
					doUpload = true;
					BOX_TRACE("Upload decision: " <<
						filename << ": will upload "
						"(not on server)");
				}
				else if (modTime >= rParams.mSyncPeriodStart)
				{
					doUpload = true;
					BOX_TRACE("Upload decision: " <<
						filename << ": will upload "
						"(modified since last sync)");
				}
			}

			// However, just in case things are continually 
			// modified, we check the first seen time.
			// The two compares of syncPeriodEnd and 
			// pendingFirstSeenTime are because the values 
			// are unsigned.

			if (!doUpload && 
				pendingFirstSeenTime != 0 &&
				rParams.mSyncPeriodEnd > pendingFirstSeenTime &&
				(rParams.mSyncPeriodEnd - pendingFirstSeenTime) 
				> rParams.mMaxUploadWait)
			{
				doUpload = true;
				BOX_TRACE("Upload decision: " <<
					filename << ": will upload "
					"(continually modified)");
			}

			// Then make sure that if files are added with a 
			// time less than the sync period start
			// (which can easily happen on file server), it 
			// gets uploaded. The directory contents checksum
			// will pick up the fact it has been added, so the 
			// store listing will be available when this happens.

			if (!doUpload &&
				modTime <= rParams.mSyncPeriodStart && 
				en != 0 && 
				en->GetModificationTime() != modTime)
			{
				doUpload = true;
				BOX_TRACE("Upload decision: " <<
					filename << ": will upload "
					"(mod time changed)");
			}

			// And just to catch really badly off clocks in 
			// the future for file server clients,
			// just upload the file if it's madly in the future.

			if (!doUpload && modTime > 
				rParams.mUploadAfterThisTimeInTheFuture)
			{
				doUpload = true;
				BOX_TRACE("Upload decision: " <<
					filename << ": will upload "
					"(mod time in the future)");
			}
		}
	
		if (en != 0 && en->GetModificationTime() == modTime)
		{
			BOX_TRACE("Upload decision: " <<
				filename << ": will not upload "
				"(not modified since last upload)");
		}
		else if (!doUpload)
		{
			if (modTime > rParams.mSyncPeriodEnd)
			{
				box_time_t now = GetCurrentBoxTime();
				int age = BoxTimeToSeconds(now -
					modTime);
				BOX_TRACE("Upload decision: " <<
					filename << ": will not upload "
					"(modified too recently: "
					"only " << age << " seconds ago)");
			}
			else
			{
				BOX_TRACE("Upload decision: " <<
					filename << ": will not upload "
					"(mod time is " << modTime << 
					" which is outside sync window, "
					<< rParams.mSyncPeriodStart << " to "
					<< rParams.mSyncPeriodEnd << ")");
			}
		}

		bool fileSynced = true;

		if (doUpload)
		{
			// Upload needed, don't mark sync success until
			// we've actually done it
			fileSynced = false;

			// Make sure we're connected -- must connect here so we know whether
			// the storage limit has been exceeded, and hence whether or not
			// to actually upload the file.
			rContext.GetConnection();

			// Only do this step if there is room on the server.
			// This step will be repeated later when there is space available
			if(!rContext.StorageLimitExceeded())
			{
				// Upload the file to the server, recording the
				// object ID it returns
				bool noPreviousVersionOnServer = 
					((pDirOnStore != 0) && (en == 0));
				
				// Surround this in a try/catch block, to
				// catch errors, but still continue
				bool uploadSuccess = false;
				try
				{
					latestObjectID = UploadFile(rParams,
						filename, storeFilename,
						fileSize, modTime,
						attributesHash,
						noPreviousVersionOnServer);

					if (latestObjectID == 0)
					{
						// storage limit exceeded
						rParams.mrContext.SetStorageLimitExceeded();
						uploadSuccess = false;
						allUpdatedSuccessfully = false;
					}
					else
					{
						uploadSuccess = true;
					}
				}
				catch(ConnectionException &e)
				{
					// Connection errors should just be
					// passed on to the main handler,
					// retries would probably just cause
					// more problems.
					rNotifier.NotifyFileUploadException(
						this, filename, e);
					throw;
				}
				catch(BoxException &e)
				{
					if (e.GetType() == BackupStoreException::ExceptionType &&
						e.GetSubType() == BackupStoreException::SignalReceived)
					{
						// abort requested, pass the 
						// exception on up.
						throw;
					}
					
					// an error occured -- make return
					// code false, to show error in directory
					allUpdatedSuccessfully = false;
					// Log it.
					SetErrorWhenReadingFilesystemObject(rParams, filename.c_str());
					rNotifier.NotifyFileUploadException(
						this, filename, e);
				}

				// Update structures if the file was uploaded
				// successfully.
				if(uploadSuccess)
				{
					fileSynced = true;

					// delete from pending entries
					if(pendingFirstSeenTime != 0 && mpPendingEntries != 0)
					{
						mpPendingEntries->erase(*f);
					}
				}
			}
			else
			{
				rNotifier.NotifyFileSkippedServerFull(this,
					filename);
			}
		}
		else if(en != 0 && en->GetAttributesHash() != attributesHash)
		{
			// Attributes have probably changed, upload them again.
			// If the attributes have changed enough, the directory
			// hash will have changed too, and so the dir will have
			// been downloaded, and the entry will be available.

			// Get connection
			BackupProtocolClient &connection(rContext.GetConnection());

			// Only do this step if there is room on the server.
			// This step will be repeated later when there is
			// space available
			if(!rContext.StorageLimitExceeded())
			{
				try
				{
					rNotifier.NotifyFileUploadingAttributes(
						this, filename);
					
					// Update store
					BackupClientFileAttributes attr;
					attr.ReadAttributes(filename.c_str(), false /* put mod times in the attributes, please */);
					MemBlockStream attrStream(attr);
					connection.QuerySetReplacementFileAttributes(mObjectID, attributesHash, storeFilename, attrStream);
					fileSynced = true;
				}
				catch (BoxException &e)
				{
					BOX_ERROR("Failed to read or store "
						"file attributes for '" <<
						filename << "', will try "
						"again later");
				}
			}
		}

		if(modTime >= rParams.mSyncPeriodEnd)
		{
			// Allocate?
			if(mpPendingEntries == 0)
			{
				mpPendingEntries = new std::map<std::string, box_time_t>;
			}
			// Adding to mPendingEntries list 
			if(pendingFirstSeenTime == 0)
			{
				// Haven't seen this before -- add to list!
				(*mpPendingEntries)[*f] = modTime;
			}
		}
		
		// Zero pointer in rEntriesLeftOver, if we have a pointer to zero
		if(en != 0)
		{
			for(unsigned int l = 0; l < rEntriesLeftOver.size(); ++l)
			{
				if(rEntriesLeftOver[l] == en)
				{
					rEntriesLeftOver[l] = 0;
					break;
				}
			}
		}
		
		// Does this file need an entry in the ID map?
		if(fileSize >= rParams.mFileTrackingSizeThreshold)
		{
			// Get the map
			BackupClientInodeToIDMap &idMap(rContext.GetNewIDMap());
		
			// Need to get an ID from somewhere...
			if(latestObjectID != 0)
			{
				// Use this one
				BOX_TRACE("Storing uploaded file ID " <<
					inodeNum << " (" << filename << ") "
					"in ID map as object " <<
					latestObjectID << " with parent " <<
					mObjectID);
				idMap.AddToMap(inodeNum, latestObjectID, mObjectID /* containing directory */);
			}
			else
			{
				// Don't know it -- haven't sent anything to the store, and didn't get a listing.
				// Look it up in the current map, and if it's there, use that.
				const BackupClientInodeToIDMap &currentIDMap(rContext.GetCurrentIDMap());
				int64_t objid = 0, dirid = 0;
				if(currentIDMap.Lookup(inodeNum, objid, dirid))
				{
					// Found
					if (dirid != mObjectID)
					{
						BOX_WARNING("Found conflicting parent ID for file ID " << inodeNum << " (" << filename << "): expected " << mObjectID << " but found " << dirid << " (same directory used in two different locations?)");
					}

					ASSERT(dirid == mObjectID);

					// NOTE: If the above assert fails, an inode number has been reused by the OS,
					// or there is a problem somewhere. If this happened on a short test run, look
					// into it. However, in a long running process this may happen occasionally and
					// not indicate anything wrong.
					// Run the release version for real life use, where this check is not made.
					BOX_TRACE("Storing found file ID " <<
						inodeNum << " (" << filename <<
						") in ID map as object " <<
						objid << " with parent " <<
						mObjectID);
					idMap.AddToMap(inodeNum, objid,
						mObjectID /* containing directory */);
				}
			}
		}
		
		if (fileSynced)
		{
			rNotifier.NotifyFileSynchronised(this, filename,
				fileSize);
		}
	}

	// Erase contents of files to save space when recursing
	rFiles.clear();

	// Delete the pending entries, if the map is entry
	if(mpPendingEntries != 0 && mpPendingEntries->size() == 0)
	{
		BOX_TRACE("Deleting mpPendingEntries from dir ID " <<
			BOX_FORMAT_OBJECTID(mObjectID));
		delete mpPendingEntries;
		mpPendingEntries = 0;
	}
	
	// Do directories
	for(std::vector<std::string>::const_iterator d = rDirs.begin();
		d != rDirs.end(); ++d)
	{
		// Send keep-alive message if needed
		rContext.DoKeepAlive();
		
		// Get the local filename
		std::string dirname(MakeFullPath(rLocalPath, *d));
	
		// See if it's in the listing (if we have one)
		BackupStoreFilenameClear storeFilename(*d);
		BackupStoreDirectory::Entry *en = 0;
		if(pDirOnStore != 0)
		{
			DecryptedEntriesMap_t::iterator i(decryptedEntries.find(*d));
			if(i != decryptedEntries.end())
			{
				en = i->second;
			}
		}
		
		// Check that the entry which might have been found is in fact a directory
		if((en != 0) && ((en->GetFlags() & BackupStoreDirectory::Entry::Flags_Dir) == 0))
		{
			// Entry exists, but is not a directory. Bad.
			// Get rid of it.
			BackupProtocolClient &connection(rContext.GetConnection());
			connection.QueryDeleteFile(mObjectID /* in directory */, storeFilename);
			rNotifier.NotifyFileDeleted(en->GetObjectID(),
				storeFilename.GetClearFilename());
			
			// Nothing found
			en = 0;
		}

		// Flag for having created directory, so can optimise the
		// recursive call not to read it again, because we know
		// it's empty.
		bool haveJustCreatedDirOnServer = false;

		// Next, see if it's in the list of sub directories
		BackupClientDirectoryRecord *psubDirRecord = 0;
		std::map<std::string, BackupClientDirectoryRecord *>::iterator
			e(mSubDirectories.find(*d));

		if(e != mSubDirectories.end())
		{
			// In the list, just use this pointer
			psubDirRecord = e->second;
		}
		else 
		{
			// Note: if we have exceeded our storage limit, then
			// we should not upload any more data, nor create any
			// DirectoryRecord representing data that would have
			// been uploaded. This step will be repeated when 
			// there is some space available.
			bool doCreateDirectoryRecord = true;
			
			// Need to create the record. But do we need to create the directory on the server?
			int64_t subDirObjectID = 0;
			if(en != 0)
			{
				// No. Exists on the server, and we know about it from the listing.
				subDirObjectID = en->GetObjectID();
			}
			else if(rContext.StorageLimitExceeded())	
			// know we've got a connection if we get this far,
			// as dir will have been modified.
			{
				doCreateDirectoryRecord = false;
			}				
			else
			{
				// Yes, creation required!
				// It is known that the it doesn't exist:
				//   if pDirOnStore == 0, then the directory has had an initial sync, and hasn't been modified.
				//	 so it has definately been created already.
				//   if en == 0 but pDirOnStore != 0, well... obviously it doesn't exist.

				// Get attributes
				box_time_t attrModTime = 0;
				InodeRefType inodeNum = 0;
				BackupClientFileAttributes attr;
				bool failedToReadAttributes = false;

				try
				{
					attr.ReadAttributes(dirname.c_str(),
						true /* directories have zero mod times */,
						0 /* not interested in mod time */,
						&attrModTime, 0 /* not file size */,
						&inodeNum);
				}
				catch (BoxException &e)
				{
					BOX_WARNING("Failed to read attributes "
						"of directory, cannot check "
						"for rename, assuming new: '"
						<< dirname << "'");
					failedToReadAttributes = true;
				}

				// Check to see if the directory been renamed
				// First, do we have a record in the ID map?
				int64_t renameObjectID = 0, renameInDirectory = 0;
				bool renameDir = false;
				const BackupClientInodeToIDMap &idMap(
					rContext.GetCurrentIDMap());

				if(!failedToReadAttributes && idMap.Lookup(inodeNum,
					renameObjectID, renameInDirectory))
				{
					// Look up on the server to get the name, to build the local filename
					std::string localPotentialOldName;
					bool isDir = false;
					bool isCurrentVersion = false;
					if(rContext.FindFilename(renameObjectID, renameInDirectory, localPotentialOldName, isDir, isCurrentVersion))
					{	
						// Only interested if it's a directory
						if(isDir && isCurrentVersion)
						{
							// Check that the object doesn't exist already
							EMU_STRUCT_STAT st;
							if(EMU_STAT(localPotentialOldName.c_str(), &st) != 0 && errno == ENOENT)
							{
								// Doesn't exist locally, but does exist on the server.
								// Therefore we can safely rename it.
								renameDir = true;
							}
						}
					}
				}

				// Get connection
				BackupProtocolClient &connection(rContext.GetConnection());
				
				// Don't do a check for storage limit exceeded here, because if we get to this
				// stage, a connection will have been opened, and the status known, so the check 
				// in the else if(...) above will be correct.

				// Build attribute stream for sending
				MemBlockStream attrStream(attr);

				if(renameDir)
				{
					// Rename the existing directory on the server
					connection.QueryMoveObject(renameObjectID, renameInDirectory, mObjectID /* move to this directory */,
						BackupProtocolClientMoveObject::Flags_MoveAllWithSameName | BackupProtocolClientMoveObject::Flags_AllowMoveOverDeletedObject,
						storeFilename);
						
					// Put the latest attributes on it
					connection.QueryChangeDirAttributes(renameObjectID, attrModTime, attrStream);

					// Stop it being deleted later
					BackupClientDeleteList &rdelList(
						rContext.GetDeleteList());
					rdelList.StopDirectoryDeletion(renameObjectID);

					// This is the ID for the renamed directory
					subDirObjectID = renameObjectID;
				}
				else
				{
					// Create a new directory
					std::auto_ptr<BackupProtocolClientSuccess> dirCreate(connection.QueryCreateDirectory(
						mObjectID, attrModTime, storeFilename, attrStream));
					subDirObjectID = dirCreate->GetObjectID(); 
					
					// Flag as having done this for optimisation later
					haveJustCreatedDirOnServer = true;
				}
			}

			if (doCreateDirectoryRecord)
			{				
				// New an object for this
				psubDirRecord = new BackupClientDirectoryRecord(subDirObjectID, *d);
				
				// Store in list
				try
				{
					mSubDirectories[*d] = psubDirRecord;
				}
				catch(...)
				{
					delete psubDirRecord;
					psubDirRecord = 0;
					throw;
				}
			}
		}
		
		ASSERT(psubDirRecord != 0 || rContext.StorageLimitExceeded());
		
		if(psubDirRecord)
		{
			// Sync this sub directory too
			psubDirRecord->SyncDirectory(rParams, mObjectID,
				dirname, rRemotePath + "/" + *d,
				haveJustCreatedDirOnServer);
		}

		// Zero pointer in rEntriesLeftOver, if we have a pointer to zero
		if(en != 0)
		{
			for(unsigned int l = 0; l < rEntriesLeftOver.size(); ++l)
			{
				if(rEntriesLeftOver[l] == en)
				{
					rEntriesLeftOver[l] = 0;
					break;
				}
			}
		}
	}
	
	// Delete everything which is on the store, but not on disc
	for(unsigned int l = 0; l < rEntriesLeftOver.size(); ++l)
	{
		if(rEntriesLeftOver[l] != 0)
		{
			BackupStoreDirectory::Entry *en = rEntriesLeftOver[l];
		
			// These entries can't be deleted immediately, as it would prevent
			// renaming and moving of objects working properly. So we add them
			// to a list, which is actually deleted at the very end of the session.
			// If there's an error during the process, it doesn't matter if things
			// aren't actually deleted, as the whole state will be reset anyway.
			BackupClientDeleteList &rdel(rContext.GetDeleteList());

			BackupStoreFilenameClear clear(en->GetName());
			std::string localName = MakeFullPath(rLocalPath,
				clear.GetClearFilename());
			
			// Delete this entry -- file or directory?
			if((en->GetFlags() & BackupStoreDirectory::Entry::Flags_File) != 0)
			{
				// Set a pending deletion for the file
				rdel.AddFileDelete(mObjectID, en->GetName(),
					localName);
			}
			else if((en->GetFlags() & BackupStoreDirectory::Entry::Flags_Dir) != 0)
			{
				// Set as a pending deletion for the directory
				rdel.AddDirectoryDelete(en->GetObjectID(),
					localName);
				
				// If there's a directory record for it in 
				// the sub directory map, delete it now
				BackupStoreFilenameClear dirname(en->GetName());
				std::map<std::string, BackupClientDirectoryRecord *>::iterator e(mSubDirectories.find(dirname.GetClearFilename()));
				if(e != mSubDirectories.end())
				{
					// Carefully delete the entry from the map
					BackupClientDirectoryRecord *rec = e->second;
					mSubDirectories.erase(e);
					delete rec;

					std::string name = MakeFullPath(
						rLocalPath, 
						dirname.GetClearFilename());

					BOX_TRACE("Deleted directory record "
						"for " << name);
				}				
			}
		}
	}

	// Return success flag (will be false if some files failed)
	return allUpdatedSuccessfully;
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientDirectoryRecord::RemoveDirectoryInPlaceOfFile(SyncParams &, BackupStoreDirectory *, int64_t, const std::string &)
//		Purpose: Called to resolve difficulties when a directory is found on the
//				 store where a file is to be uploaded.
//		Created: 9/7/04
//
// --------------------------------------------------------------------------
void BackupClientDirectoryRecord::RemoveDirectoryInPlaceOfFile(
	SyncParams &rParams,
	BackupStoreDirectory* pDirOnStore,
	BackupStoreDirectory::Entry* pEntry,
	const std::string &rFilename)
{
	// First, delete the directory
	BackupProtocolClient &connection(rParams.mrContext.GetConnection());
	connection.QueryDeleteDirectory(pEntry->GetObjectID());

	BackupStoreFilenameClear clear(pEntry->GetName());
	rParams.mrContext.GetProgressNotifier().NotifyDirectoryDeleted(
		pEntry->GetObjectID(), clear.GetClearFilename());

	// Then, delete any directory record
	std::map<std::string, BackupClientDirectoryRecord *>::iterator
		e(mSubDirectories.find(rFilename));

	if(e != mSubDirectories.end())
	{
		// A record exists for this, remove it
		BackupClientDirectoryRecord *psubDirRecord = e->second;
		mSubDirectories.erase(e);

		// And delete the object
		delete psubDirRecord;
	}
}



// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientDirectoryRecord::UploadFile(
//			 BackupClientDirectoryRecord::SyncParams &,
//			 const std::string &,
//			 const BackupStoreFilename &,
//			 int64_t, box_time_t, box_time_t, bool)
//		Purpose: Private. Upload a file to the server. May send
//			 a patch instead of the whole thing
//		Created: 20/1/04
//
// --------------------------------------------------------------------------
int64_t BackupClientDirectoryRecord::UploadFile(
	BackupClientDirectoryRecord::SyncParams &rParams,
	const std::string &rFilename,
	const BackupStoreFilename &rStoreFilename,
	int64_t FileSize,
	box_time_t ModificationTime,
	box_time_t AttributesHash,
	bool NoPreviousVersionOnServer)
{
	BackupClientContext& rContext(rParams.mrContext);
	ProgressNotifier& rNotifier(rContext.GetProgressNotifier());

	// Get the connection
	BackupProtocolClient &connection(rContext.GetConnection());

	// Info
	int64_t objID = 0;
	bool doNormalUpload = true;
	
	// Use a try block to catch store full errors
	try
	{
		// Might an old version be on the server, and is the file
		// size over the diffing threshold?
		if(!NoPreviousVersionOnServer &&
			FileSize >= rParams.mDiffingUploadSizeThreshold)
		{
			// YES -- try to do diff, if possible
			// First, query the server to see if there's an old version available
			std::auto_ptr<BackupProtocolClientSuccess> getBlockIndex(connection.QueryGetBlockIndexByName(mObjectID, rStoreFilename));
			int64_t diffFromID = getBlockIndex->GetObjectID();
			
			if(diffFromID != 0)
			{
				// Found an old version
				rNotifier.NotifyFileUploadingPatch(this, 
					rFilename);

				// Get the index
				std::auto_ptr<IOStream> blockIndexStream(connection.ReceiveStream());
			
				//
				// Diff the file
				//

				rContext.ManageDiffProcess();

				bool isCompletelyDifferent = false;
				std::auto_ptr<IOStream> patchStream(
					BackupStoreFile::EncodeFileDiff(
						rFilename.c_str(),
						mObjectID,	/* containing directory */
						rStoreFilename, diffFromID, *blockIndexStream,
						connection.GetTimeout(), 
						&rContext, // DiffTimer implementation
						0 /* not interested in the modification time */, 
						&isCompletelyDifferent));
	
				rContext.UnManageDiffProcess();

				//
				// Upload the patch to the store
				//
				std::auto_ptr<BackupProtocolClientSuccess> stored(connection.QueryStoreFile(mObjectID, ModificationTime,
						AttributesHash, isCompletelyDifferent?(0):(diffFromID), rStoreFilename, *patchStream));
				
				// Get object ID from the result		
				objID = stored->GetObjectID();

				// Don't attempt to upload it again!
				doNormalUpload = false;
			} 
		}
	
		if(doNormalUpload)
		{
			// below threshold or nothing to diff from, so upload whole
			rNotifier.NotifyFileUploading(this, rFilename);
			
			// Prepare to upload, getting a stream which will encode the file as we go along
			std::auto_ptr<IOStream> upload(
				BackupStoreFile::EncodeFile(rFilename.c_str(),
					mObjectID, rStoreFilename, NULL,
					&rParams,
					&(rParams.mrRunStatusProvider)));
		
			// Send to store
			std::auto_ptr<BackupProtocolClientSuccess> stored(
				connection.QueryStoreFile(
					mObjectID, ModificationTime,
					AttributesHash, 
					0 /* no diff from file ID */, 
					rStoreFilename, *upload));
	
			// Get object ID from the result		
			objID = stored->GetObjectID();
		}
	}
	catch(BoxException &e)
	{
		rContext.UnManageDiffProcess();

		if(e.GetType() == ConnectionException::ExceptionType &&
			e.GetSubType() == ConnectionException::Protocol_UnexpectedReply)
		{
			// Check and see what error the protocol has,
			// this is more useful to users than the exception.
			int type, subtype;
			if(connection.GetLastError(type, subtype))
			{
				if(type == BackupProtocolClientError::ErrorType
				&& subtype == BackupProtocolClientError::Err_StorageLimitExceeded)
				{
					// The hard limit was exceeded on the server, notify!
					rParams.mrSysadminNotifier.NotifySysadmin(
						SysadminNotifier::StoreFull);
					// return an error code instead of
					// throwing an exception that we
					// can't debug.
					return 0;
				}
				rNotifier.NotifyFileUploadServerError(this,
					rFilename, type, subtype);
			}
		}
		
		// Send the error on it's way
		throw;
	}

	rNotifier.NotifyFileUploaded(this, rFilename, FileSize);

	// Return the new object ID of this file
	return objID;
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientDirectoryRecord::SetErrorWhenReadingFilesystemObject(SyncParams &, const char *)
//		Purpose: Sets the error state when there were problems reading an object
//				 from the filesystem.
//		Created: 29/3/04
//
// --------------------------------------------------------------------------
void BackupClientDirectoryRecord::SetErrorWhenReadingFilesystemObject(BackupClientDirectoryRecord::SyncParams &rParams, const char *Filename)
{
	// Zero hash, so it gets synced properly next time round.
	::memset(mStateChecksum, 0, sizeof(mStateChecksum));

	// Log the error - already done by caller
	/*
	rParams.GetProgressNotifier().NotifyFileReadFailed(this, 
		Filename, strerror(errno));
	*/

	// Mark that an error occured in the parameters object
	rParams.mReadErrorsOnFilesystemObjects = true;
}



// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientDirectoryRecord::SyncParams::SyncParams(BackupClientContext &)
//		Purpose: Constructor
//		Created: 8/3/04
//
// --------------------------------------------------------------------------
BackupClientDirectoryRecord::SyncParams::SyncParams(
	RunStatusProvider &rRunStatusProvider,
	SysadminNotifier &rSysadminNotifier,
	ProgressNotifier &rProgressNotifier,
	BackupClientContext &rContext)
	: mSyncPeriodStart(0),
	  mSyncPeriodEnd(0),
	  mMaxUploadWait(0),
	  mMaxFileTimeInFuture(99999999999999999LL),
	  mFileTrackingSizeThreshold(16*1024),
	  mDiffingUploadSizeThreshold(16*1024),
	  mrRunStatusProvider(rRunStatusProvider),
	  mrSysadminNotifier(rSysadminNotifier),
	  mrProgressNotifier(rProgressNotifier),
	  mrContext(rContext),
	  mReadErrorsOnFilesystemObjects(false),
	  mUploadAfterThisTimeInTheFuture(99999999999999999LL),
	  mHaveLoggedWarningAboutFutureFileTimes(false)
{
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientDirectoryRecord::SyncParams::~SyncParams()
//		Purpose: Destructor
//		Created: 8/3/04
//
// --------------------------------------------------------------------------
BackupClientDirectoryRecord::SyncParams::~SyncParams()
{
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientDirectoryRecord::Deserialize(Archive & rArchive)
//		Purpose: Deserializes this object instance from a stream of bytes, using an Archive abstraction.
//
//		Created: 2005/04/11
//
// --------------------------------------------------------------------------
void BackupClientDirectoryRecord::Deserialize(Archive & rArchive)
{
	// Make deletion recursive
	DeleteSubDirectories();

	// Delete maps
	if(mpPendingEntries != 0)
	{
		delete mpPendingEntries;
		mpPendingEntries = 0;
	}

	//
	//
	//
	rArchive.Read(mObjectID);
	rArchive.Read(mSubDirName);
	rArchive.Read(mInitialSyncDone);
	rArchive.Read(mSyncDone);

	//
	//
	//
	int64_t iCount = 0;
	rArchive.Read(iCount);

	if (iCount != sizeof(mStateChecksum)/sizeof(mStateChecksum[0]))
	{
		// we have some kind of internal system representation change: throw for now
		THROW_EXCEPTION(CommonException, Internal)
	}

	for (int v = 0; v < iCount; v++)
	{
		// Load each checksum entry
		rArchive.Read(mStateChecksum[v]);
	}

	//
	//
	//
	iCount = 0;
	rArchive.Read(iCount);

	if (iCount > 0)
	{
		// load each pending entry
		mpPendingEntries = new std::map<std::string, box_time_t>;
		if (!mpPendingEntries)
		{
			throw std::bad_alloc();
		}

		for (int v = 0; v < iCount; v++)
		{
			std::string strItem;
			box_time_t btItem;

			rArchive.Read(strItem);
			rArchive.Read(btItem);
			(*mpPendingEntries)[strItem] = btItem;
		}
	}

	//
	//
	//
	iCount = 0;
	rArchive.Read(iCount);

	if (iCount > 0)
	{
		for (int v = 0; v < iCount; v++)
		{
			std::string strItem;
			rArchive.Read(strItem);

			BackupClientDirectoryRecord* pSubDirRecord = 
				new BackupClientDirectoryRecord(0, ""); 
			// will be deserialized anyway, give it id 0 for now

			if (!pSubDirRecord)
			{
				throw std::bad_alloc();
			}

			/***** RECURSE *****/
			pSubDirRecord->Deserialize(rArchive);
			mSubDirectories[strItem] = pSubDirRecord;
		}
	}
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientDirectoryRecord::Serialize(Archive & rArchive)
//		Purpose: Serializes this object instance into a stream of bytes, using an Archive abstraction.
//
//		Created: 2005/04/11
//
// --------------------------------------------------------------------------
void BackupClientDirectoryRecord::Serialize(Archive & rArchive) const
{
	//
	//
	//
	rArchive.Write(mObjectID);
	rArchive.Write(mSubDirName);
	rArchive.Write(mInitialSyncDone);
	rArchive.Write(mSyncDone);

	//
	//
	//
	int64_t iCount = 0;

	// when reading back the archive, we will 
	// need to know how many items there are.
	iCount = sizeof(mStateChecksum) / sizeof(mStateChecksum[0]);
	rArchive.Write(iCount); 

	for (int v = 0; v < iCount; v++)
	{
		rArchive.Write(mStateChecksum[v]);
	}

	//
	//
	//
	if (!mpPendingEntries)
	{
		iCount = 0;
		rArchive.Write(iCount);
	}
	else
	{
		iCount = mpPendingEntries->size();
		rArchive.Write(iCount);

		for (std::map<std::string, box_time_t>::const_iterator
			i =  mpPendingEntries->begin(); 
			i != mpPendingEntries->end(); i++)
		{
			rArchive.Write(i->first);
			rArchive.Write(i->second);
		}
	}
	//
	//
	//
	iCount = mSubDirectories.size();
	rArchive.Write(iCount);

	for (std::map<std::string, BackupClientDirectoryRecord*>::const_iterator
		i =  mSubDirectories.begin(); 
		i != mSubDirectories.end(); i++)
	{
		const BackupClientDirectoryRecord* pSubItem = i->second;
		ASSERT(pSubItem);

		rArchive.Write(i->first);
		pSubItem->Serialize(rArchive);
	}
}
