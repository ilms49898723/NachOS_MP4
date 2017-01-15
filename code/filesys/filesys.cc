// filesys.cc
//  Routines to manage the overall operation of the file system.
//  Implements routines to map from textual file names to files.
//
//  Each file in the file system has:
//     A file header, stored in a sector on disk
//      (the size of the file header data structure is arranged
//      to be precisely the size of 1 disk sector)
//     A number of data blocks
//     An entry in the file system directory
//
//  The file system consists of several data structures:
//     A bitmap of free disk sectors (cf. bitmap.h)
//     A directory of file names and file headers
//
//      Both the bitmap and the directory are represented as normal
//  files.  Their file headers are located in specific sectors
//  (sector 0 and sector 1), so that the file system can find them
//  on bootup.
//
//  The file system assumes that the bitmap and directory files are
//  kept "open" continuously while Nachos is running.
//
//  For those operations (such as Create, Remove) that modify the
//  directory and/or bitmap, if the operation succeeds, the changes
//  are written immediately back to disk (the two files are kept
//  open during all this time).  If the operation fails, and we have
//  modified part of the directory and/or bitmap, we simply discard
//  the changed version, without writing it back to disk.
//
//  Our implementation at this point has the following restrictions:
//
//     there is no synchronization for concurrent accesses
//     files have a fixed size, set when the file is created
//     files cannot be bigger than about 3KB in size
//     there is no hierarchical directory structure, and only a limited
//       number of files can be added to the system
//     there is no attempt to make the system robust to failures
//      (if Nachos exits in the middle of an operation that modifies
//      the file system, it may corrupt the disk)
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation
// of liability and disclaimer of warranty provisions.
#ifndef FILESYS_STUB

#include "copyright.h"
#include "debug.h"
#include "disk.h"
#include "pbitmap.h"
#include "directory.h"
#include "filehdr.h"
#include "filesys.h"

// Sectors containing the file headers for the bitmap of free sectors,
// and the directory of files.  These file headers are placed in well-known
// sectors, so that they can be located on boot-up.
#define FreeMapSector       0
#define DirectorySector     1

// Initial file sizes for the bitmap and directory; until the file system
// supports extensible files, the directory size sets the maximum number
// of files that can be loaded onto the disk.
#define FreeMapFileSize     (NumSectors / BitsInByte)
#define NumDirEntries       64
#define DirectoryFileSize   (sizeof(DirectoryEntry) * NumDirEntries)

//----------------------------------------------------------------------
// FileSystem::FileSystem
//  Initialize the file system.  If format = TRUE, the disk has
//  nothing on it, and we need to initialize the disk to contain
//  an empty directory, and a bitmap of free sectors (with almost but
//  not all of the sectors marked as free).
//
//  If format = FALSE, we just have to open the files
//  representing the bitmap and the directory.
//
//  "format" -- should we initialize the disk?
//----------------------------------------------------------------------

FileSystem::FileSystem(bool format) {
    DEBUG(dbgFile, "Initializing the file system.");

    if (format) {
        PersistentBitmap* freeMap = new PersistentBitmap(NumSectors);
        Directory* directory = new Directory(NumDirEntries);
        FileHeader* mapHdr = new FileHeader;
        FileHeader* dirHdr = new FileHeader;
        mapHdr->level = 1;
        dirHdr->level = 1;

        DEBUG(dbgFile, "Formatting the file system.");

        // First, allocate space for FileHeaders for the directory and bitmap
        // (make sure no one else grabs these!)
        freeMap->Mark(FreeMapSector);
        freeMap->Mark(DirectorySector);

        // Second, allocate space for the data blocks containing the contents
        // of the directory and bitmap files.  There better be enough space!

        ASSERT(mapHdr->Allocate(freeMap, FreeMapFileSize));
        ASSERT(dirHdr->Allocate(freeMap, DirectoryFileSize));

        // Flush the bitmap and directory FileHeaders back to disk
        // We need to do this before we can "Open" the file, since open
        // reads the file header off of disk (and currently the disk has garbage
        // on it!).

        DEBUG(dbgFile, "Writing headers back to disk.");
        mapHdr->WriteBack(FreeMapSector);
        dirHdr->WriteBack(DirectorySector);

        // OK to open the bitmap and directory files now
        // The file system operations assume these two files are left open
        // while Nachos is running.

        freeMapFile = new OpenFile(FreeMapSector);
        directoryFile = new OpenFile(DirectorySector);

        // Once we have the files "open", we can write the initial version
        // of each file back to disk.  The directory at this point is completely
        // empty; but the bitmap has been changed to reflect the fact that
        // sectors on the disk have been allocated for the file headers and
        // to hold the file data for the directory and bitmap.

        DEBUG(dbgFile, "Writing bitmap and directory back to disk.");
        freeMap->WriteBack(freeMapFile);     // flush changes to disk
        directory->WriteBack(directoryFile);

        if (debug->IsEnabled('f')) {
            freeMap->Print();
            directory->Print();
        }

        delete freeMap;
        delete directory;
        delete mapHdr;
        delete dirHdr;
    } else {
        // if we are not formatting the disk, just open the files representing
        // the bitmap and directory; these are left open while Nachos is running
        freeMapFile = new OpenFile(FreeMapSector);
        directoryFile = new OpenFile(DirectorySector);
    }

    for (int i = 0; i < 20; ++i) {
        fileDescriptorTable[i] = NULL;
    }
}

//----------------------------------------------------------------------
// MP4 mod tag
// FileSystem::~FileSystem
//----------------------------------------------------------------------
FileSystem::~FileSystem() {
    delete freeMapFile;
    delete directoryFile;
}

//----------------------------------------------------------------------
// FileSystem::Create
//  Create a file in the Nachos file system (similar to UNIX create).
//  Since we can't increase the size of files dynamically, we have
//  to give Create the initial size of the file.
//
//  The steps to create a file are:
//    Make sure the file doesn't already exist
//        Allocate a sector for the file header
//    Allocate space on disk for the data blocks for the file
//    Add the name to the directory
//    Store the new file header on disk
//    Flush the changes to the bitmap and the directory back to disk
//
//  Return TRUE if everything goes ok, otherwise, return FALSE.
//
//  Create fails if:
//          file is already in directory
//      no free space for file header
//      no free entry for file in directory
//      no free space for data blocks for the file
//
//  Note that this implementation assumes there is no concurrent access
//  to the file system!
//
//  "name" -- name of file to be created
//  "initialSize" -- size of file to be created
//----------------------------------------------------------------------

bool
FileSystem::Create(char* name, int initialSize) {
    const int NumOfLevel1Hdr = divRoundUp(initialSize, SectorSize * NumDirect);

    Directory* directory;
    PersistentBitmap* freeMap;
    FileHeader* hdr;
    FileHeader level1Hdr[NumDirect];
    int sector;
    int level1Sector[NumDirect];
    bool success = TRUE;

    DEBUG(dbgFile, "Creating file " << name << " size " << initialSize);

    directory = new Directory(NumDirEntries);

    char parent[1024];
    char filename[1024];
    SplitPath(name, parent, filename);
    OpenFile* dirFile = OpenDir(parent);

    if (dirFile == NULL) {
        return FALSE;
    }

    directory->FetchFrom(dirFile);

    if (directory->Find(filename) != -1) {
        success = FALSE;    // file is already in directory
    } else {
        freeMap = new PersistentBitmap(freeMapFile, NumSectors);
        sector = freeMap->FindAndSet(); // find a sector to hold the file header

        bool level1Flag = TRUE;

        for (int i = 0; i < NumOfLevel1Hdr; ++i) {
            level1Sector[i] = freeMap->FindAndSet();

            if (level1Sector[i] == -1) {
                level1Flag = FALSE;
                break;
            }
        }

        if (sector == -1 || level1Flag == FALSE) {
            success = FALSE;    // no free block for file header
        } else if (!directory->Add(filename, sector)) {
            success = FALSE;    // no space in directory
        } else {
            hdr = new FileHeader;

            hdr->numBytes = initialSize;
            hdr->numSectors = NumOfLevel1Hdr;
            hdr->level = 0;

            for (int i = 0; i < NumOfLevel1Hdr; ++i) {
                hdr->dataSectors[i] = level1Sector[i];
            }

            int remainFileSize = initialSize;
            int level1HdrIdx = 0;

            while (remainFileSize > 0) {
                int toRequest = (remainFileSize >= NumDirect * SectorSize) ? NumDirect * SectorSize : remainFileSize;
                remainFileSize -= toRequest;
                level1Hdr[level1HdrIdx].level = 1;

                if (!level1Hdr[level1HdrIdx++].Allocate(freeMap, toRequest)) {
                    success = FALSE;
                }
            }

            if (success == TRUE) {
                // everthing worked, flush all changes back to disk
                hdr->WriteBack(sector);

                for (int i = 0; i < NumOfLevel1Hdr; ++i) {
                    level1Hdr[i].WriteBack(level1Sector[i]);
                }

                directory->WriteBack(dirFile);
                freeMap->WriteBack(freeMapFile);
            }

            delete hdr;
        }

        delete freeMap;
    }

    delete dirFile;
    delete directory;
    return success;
}

bool
FileSystem::CreateDirectory(char* name, char* parent) {
    Directory* directory;
    PersistentBitmap* freeMap;
    FileHeader* dirHdr;
    int sector;
    bool success = TRUE;

    DEBUG(dbgFile, "Creating directory " << name);

    directory = new Directory(NumDirEntries);
    OpenFile* dirFile = OpenDir(parent);

    if (dirFile == NULL) {
        return FALSE;
    }

    directory->FetchFrom(dirFile);

    if (directory->Find(name) != -1) {
        success = FALSE;
    } else {
        freeMap = new PersistentBitmap(freeMapFile, NumSectors);
        sector = freeMap->FindAndSet();

        if (sector == -1) {
            success = FALSE;
        } else if (!directory->AddDir(name, sector)) {
            success = FALSE;
        } else {
            dirHdr = new FileHeader;

            dirHdr->level = 1;

            if (!dirHdr->Allocate(freeMap, DirectoryFileSize)) {
                success = FALSE;
            } else {
                success = TRUE;
                dirHdr->WriteBack(sector);
                directory->WriteBack(dirFile);
                freeMap->WriteBack(freeMapFile);

                Directory* newDirectory = new Directory(NumDirEntries);
                OpenFile* newDirFile = new OpenFile(sector);
                newDirectory->WriteBack(newDirFile);
                delete newDirFile;
                delete newDirectory;
            }

            delete dirHdr;
        }

        delete freeMap;
    }

    delete dirFile;
    delete directory;
    return success;
}

OpenFile*
FileSystem::OpenDir(char* inpath) {
    char path[1024];
    strncpy(path, inpath, 1024);
    Directory* directory = new Directory(NumDirEntries);
    directory->FetchFrom(directoryFile);
    char* split;
    int sector = 1;
    split = strtok(path, "/");

    while (split != NULL) {
        sector = directory->Find(split);

        if (sector == -1) {
            return NULL;
        } else {
            OpenFile* dirFile = new OpenFile(sector);
            directory->FetchFrom(dirFile);
            delete dirFile;
        }

        split = strtok(split + strlen(split) + 1, "/");
    }

    delete directory;
    return new OpenFile(sector);
}

//----------------------------------------------------------------------
// FileSystem::Open
//  Open a file for reading and writing.
//  To open a file:
//    Find the location of the file's header, using the directory
//    Bring the header into memory
//
//  "name" -- the text name of the file to be opened
//----------------------------------------------------------------------

OpenFile*
FileSystem::Open(char* name) {
    Directory* directory = new Directory(NumDirEntries);
    OpenFile* openFile = NULL;
    int sector;

    DEBUG(dbgFile, "Opening file" << name);

    char parent[1024];
    char filename[1024];
    SplitPath(name, parent, filename);
    OpenFile* dirFile = OpenDir(parent);

    if (dirFile == NULL) {
        return NULL;
    }

    directory->FetchFrom(dirFile);
    delete dirFile;

    sector = directory->Find(filename);

    if (sector >= 0) {
        openFile = new OpenFile(sector);    // name was found in directory
    }

    delete directory;
    return openFile;                // return NULL if not found
}

//----------------------------------------------------------------------
// FileSystem::Remove
//  Delete a file from the file system.  This requires:
//      Remove it from the directory
//      Delete the space for its header
//      Delete the space for its data blocks
//      Write changes to directory, bitmap back to disk
//
//  Return TRUE if the file was deleted, FALSE if the file wasn't
//  in the file system.
//
//  "name" -- the text name of the file to be removed
//----------------------------------------------------------------------

bool
FileSystem::Remove(char* name, bool recur) {
    cout << "Remove " << name << endl;
    Directory* directory;
    PersistentBitmap* freeMap;
    FileHeader* fileHdr;
    int sector;
    int tableIdx;

    char filename[1024];
    char parent[1024];
    SplitPath(name, parent, filename);

    directory = new Directory(NumDirEntries);
    OpenFile* dirFile = OpenDir(parent);

    if (dirFile == NULL) {
        cout << "Directory " << parent << " not found!" << endl;
        delete directory;
        return FALSE;
    }

    directory->FetchFrom(dirFile);
    sector = directory->Find(filename);
    tableIdx = directory->FindIndex(filename);

    if (sector == -1) {
        cout << "File " << filename << " not found!" << endl;
        delete directory;
        delete dirFile;
        return FALSE;             // file not found
    }

    fileHdr = new FileHeader;
    fileHdr->FetchFrom(sector);

    freeMap = new PersistentBitmap(freeMapFile, NumSectors);

    if (directory->table[tableIdx].type) {
        // is a directory, delete all files under it
        OpenFile* nextDirFile = OpenDir(name);
        Directory* nextDir = new Directory(NumDirEntries);
        nextDir->FetchFrom(nextDirFile);
        int totalCount = 0;

        for (int i = 0; i < nextDir->tableSize; ++i) {
            if (nextDir->table[i].inUse) {
                ++totalCount;
            }
        }

        if (recur == FALSE && totalCount != 0) {
            cout << filename << ": directory not empty!" << endl;
            delete directory;
            delete dirFile;
            delete fileHdr;
            delete nextDir;
            return FALSE;
        }

        for (int i = 0; i < nextDir->tableSize; ++i) {
            if (nextDir->table[i].inUse) {
                char nextFilename[1024];
                JoinPath(nextFilename, name, nextDir->table[i].name);
                Remove(nextFilename, recur);
            }
        }

        delete nextDir;
    }

    if (fileHdr->level == 0) {
        for (int i = 0; i < fileHdr->numSectors; ++i) {
            FileHeader* level1Hdr = new FileHeader;
            level1Hdr->FetchFrom(fileHdr->dataSectors[i]);
            level1Hdr->Deallocate(freeMap);
            // don't clean header sector, let it to be cleaned by level 0
            // freeMap->Clear(fileHdr->dataSectors[i]);
            delete level1Hdr;
        }
    }

    fileHdr->Deallocate(freeMap);       // remove data blocks
    freeMap->Clear(sector);         // remove header block
    directory->Remove(filename);

    freeMap->WriteBack(freeMapFile);        // flush to disk
    directory->WriteBack(dirFile);        // flush to disk
    delete fileHdr;
    delete dirFile;
    delete directory;
    delete freeMap;
    return TRUE;
}

//----------------------------------------------------------------------
// FileSystem::List
//  List all the files in the file system directory.
//----------------------------------------------------------------------

void
FileSystem::List(char* listDirectoryName) {
    Directory* directory = new Directory(NumDirEntries);
    OpenFile* dirFile = OpenDir(listDirectoryName);

    if (dirFile == NULL) {
        delete directory;
        return;
    }

    directory->FetchFrom(dirFile);
    directory->List();
    delete dirFile;
    delete directory;
}

void
FileSystem::RecursiveList(char* listDirectoryName, int tab) {
    if (tab == 4) {
        memset(isLast, 0, sizeof(bool) * 1024);
    }

    Directory* directory = new Directory(NumDirEntries);
    OpenFile* dirFile = OpenDir(listDirectoryName);

    if (dirFile == NULL) {
        delete directory;
        return;
    }

    directory->FetchFrom(dirFile);
    int totalCount = 0;

    for (int i = 0; i < directory->tableSize; ++i) {
        if (directory->table[i].inUse) {
            ++totalCount;
        }
    }

    for (int i = 0; i < directory->tableSize; ++i) {
        if (directory->table[i].inUse) {
            --totalCount;

            for (int j = 0; j < tab / 4 - 1; ++j) {
                if (!isLast[j]) {
                    cout << "│   ";
                } else {
                    cout << "    ";
                }
            }

            if (totalCount) {
                cout << "├──";
            } else {
                cout << "└──";
            }

            cout << (directory->table[i].type ? "\x1B[1;34m" : "");
            cout << directory->table[i].name;
            cout << (directory->table[i].type ? "/" : "");
            cout << "\x1B[0m" << endl;

            if (directory->table[i].type) {
                char nextDir[1024];
                JoinPath(nextDir, listDirectoryName, directory->table[i].name);
                isLast[tab / 4 - 1] = totalCount == 0;
                RecursiveList(nextDir, tab + 4);
            }
        }
    }

    delete dirFile;
    delete directory;
}

//----------------------------------------------------------------------
// FileSystem::Print
//  Print everything about the file system:
//    the contents of the bitmap
//    the contents of the directory
//    for each file in the directory,
//        the contents of the file header
//        the data in the file
//----------------------------------------------------------------------

void
FileSystem::Print() {
    FileHeader* bitHdr = new FileHeader;
    FileHeader* dirHdr = new FileHeader;
    PersistentBitmap* freeMap = new PersistentBitmap(freeMapFile, NumSectors);
    Directory* directory = new Directory(NumDirEntries);

    printf("Bit map file header:\n");
    bitHdr->FetchFrom(FreeMapSector);
    bitHdr->Print();

    printf("Directory file header:\n");
    dirHdr->FetchFrom(DirectorySector);
    dirHdr->Print();

    freeMap->Print();

    directory->FetchFrom(directoryFile);
    directory->Print();

    delete bitHdr;
    delete dirHdr;
    delete freeMap;
    delete directory;
}

int FileSystem::Open(char* name, int unused) {
    OpenFile* fp = Open(name);

    for (int i = 1; i < 20; ++i) {
        if (fileDescriptorTable[i] == NULL) {
            fileDescriptorTable[i] = fp;
            return i;
        }
    }

    return -1;
}

int FileSystem::Write(char* buffer, int size, int fileid) {
    if (fileDescriptorTable[fileid] == NULL) {
        return -1;
    }

    return fileDescriptorTable[fileid]->Write(buffer, size);
}

int FileSystem::Read(char* buffer, int size, int fileid) {
    if (fileDescriptorTable[fileid] == NULL) {
        return -1;
    }

    return fileDescriptorTable[fileid]->Read(buffer, size);
}

int FileSystem::Close(int fileid) {
    if (fileDescriptorTable[fileid] == NULL) {
        return 0;
    }

    delete fileDescriptorTable[fileid];
    fileDescriptorTable[fileid] = NULL;
    return 1;
}

void FileSystem::SplitPath(char* fullpath, char* parent, char* name) {
    strncpy(parent, fullpath, 1024);

    int idx;
    idx = strlen(parent) - 1;

    while (idx >= 0 && parent[idx] != '/') {
        --idx;
    }

    parent[idx] = '\0';

    strncpy(name, parent + idx + 1, 1024);

    if (strlen(parent) == 0) {
        strncpy(parent, "/", 1024);
    }
}

void FileSystem::JoinPath(char* dest, char* parent, char* name) {
    strncpy(dest, parent, 1024);

    if (dest[strlen(dest) - 1] != '/') {
        strcat(dest, "/");
    }

    strcat(dest, name);
}

#endif // FILESYS_STUB
