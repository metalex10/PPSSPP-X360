// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

// TODO: Remove the Windows-specific code, FILE is fine there too.

#include <map>

#include "../Core/FileSystems/FileSystem.h"

#ifdef _WIN32
typedef void * HANDLE;
#endif

#if defined(__APPLE__)

#if TARGET_OS_IPHONE
#define HOST_IS_CASE_SENSITIVE 1
#elif TARGET_IPHONE_SIMULATOR
#define HOST_IS_CASE_SENSITIVE 0
#else
// Mac OSX case sensitivity defaults off, but is user configurable (when
// creating a filesytem), so assume the worst:
#define HOST_IS_CASE_SENSITIVE 1
#endif

#elif defined(_WIN32) || defined(__SYMBIAN32__)
#define HOST_IS_CASE_SENSITIVE 0

#else  // Android, Linux, BSD (and the rest?)
#define HOST_IS_CASE_SENSITIVE 1

#endif

#if HOST_IS_CASE_SENSITIVE
enum FixPathCaseBehavior {
	FPC_FILE_MUST_EXIST,  // all path components must exist (rmdir, move from)
	FPC_PATH_MUST_EXIST,  // all except the last one must exist - still tries to fix last one (fopen, move to)
	FPC_PARTIAL_ALLOWED,  // don't care how many exist (mkdir recursive)
};

bool FixPathCase(std::string& basePath, std::string &path, FixPathCaseBehavior behavior);
#endif

struct DirectoryFileHandle
{
#ifdef _WIN32
	HANDLE hFile;
#else
	FILE* hFile;
#endif
	DirectoryFileHandle()
	{
#ifdef _WIN32
		hFile = (HANDLE)-1;
#else
		hFile = 0;
#endif
	}

	std::string GetLocalPath(std::string& basePath, std::string localpath);
	bool Open(std::string& basePath, std::string& fileName, FileAccess access);
	size_t Read(u8* pointer, s64 size);
	size_t Write(const u8* pointer, s64 size);
	size_t Seek(s32 position, FileMove type);
	void Close();
};

class DirectoryFileSystem : public IFileSystem {
public:
	DirectoryFileSystem(IHandleAllocator *_hAlloc, std::string _basePath);
	~DirectoryFileSystem();

	void DoState(PointerWrap &p);
	std::vector<PSPFileInfo> GetDirListing(std::string path);
	u32      OpenFile(std::string filename, FileAccess access, const char *devicename=NULL);
	void     CloseFile(u32 handle);
	size_t   ReadFile(u32 handle, u8 *pointer, s64 size);
	size_t   WriteFile(u32 handle, const u8 *pointer, s64 size);
	size_t   SeekFile(u32 handle, s32 position, FileMove type);
	PSPFileInfo GetFileInfo(std::string filename);
	bool     OwnsHandle(u32 handle);

	bool MkDir(const std::string &dirname);
	bool RmDir(const std::string &dirname);
	int  RenameFile(const std::string &from, const std::string &to);
	bool RemoveFile(const std::string &filename);
	bool GetHostPath(const std::string &inpath, std::string &outpath);

private:
	struct OpenFileEntry {
		DirectoryFileHandle hFile;
	};

	typedef std::map<u32, OpenFileEntry> EntryMap;
	EntryMap entries;
	std::string basePath;
	IHandleAllocator *hAlloc;

	// In case of Windows: Translate slashes, etc.
	std::string GetLocalPath(std::string localpath);
};

// VFSFileSystem: Ability to map in Android APK paths as well! Does not support all features, only meant for fonts.
// Very inefficient - always load the whole file on open.
class VFSFileSystem : public IFileSystem {
public:
	VFSFileSystem(IHandleAllocator *_hAlloc, std::string _basePath);
	~VFSFileSystem();

	void DoState(PointerWrap &p);
	std::vector<PSPFileInfo> GetDirListing(std::string path);
	u32      OpenFile(std::string filename, FileAccess access, const char *devicename=NULL);
	void     CloseFile(u32 handle);
	size_t   ReadFile(u32 handle, u8 *pointer, s64 size);
	size_t   WriteFile(u32 handle, const u8 *pointer, s64 size);
	size_t   SeekFile(u32 handle, s32 position, FileMove type);
	PSPFileInfo GetFileInfo(std::string filename);
	bool     OwnsHandle(u32 handle);

	bool MkDir(const std::string &dirname);
	bool RmDir(const std::string &dirname);
	int  RenameFile(const std::string &from, const std::string &to);
	bool RemoveFile(const std::string &filename);
	bool GetHostPath(const std::string &inpath, std::string &outpath);

private:
	struct OpenFileEntry {
		u8 *fileData;
		size_t size;
		size_t seekPos;
	};

	typedef std::map<u32, OpenFileEntry> EntryMap;
	EntryMap entries;
	std::string basePath;
	IHandleAllocator *hAlloc;

	std::string GetLocalPath(std::string localpath);
};
