// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#ifndef _MEMORYUTIL_H
#define _MEMORYUTIL_H

#ifndef _WIN32
#include <sys/mman.h>
#endif
#include <string>

void* AllocateExecutableMemory(size_t size, bool low = true);
void* AllocateMemoryPages(size_t size);
void FreeMemoryPages(void* ptr, size_t size);
void* AllocateAlignedMemory(size_t size,size_t alignment);
void FreeAlignedMemory(void* ptr);
void WriteProtectMemory(void* ptr, size_t size, bool executable = false);
void UnWriteProtectMemory(void* ptr, size_t size, bool allowExecute = false);
#ifdef __SYMBIAN32__
void ResetExecutableMemory(void* ptr);
#endif

inline int GetPageSize() { return 4096; }

template <typename T>
class SimpleBuf {
public:
	SimpleBuf() : buf_(NULL), size_(0) {
	}

	SimpleBuf(size_t size) : buf_(NULL) {
		resize(size);
	}

	~SimpleBuf() {
		if (buf_ != NULL) {
			FreeMemoryPages(buf_, size_ * sizeof(T));
		}
	}

	inline T &operator[](size_t index) {
		return buf_[index];
	}

	// Doesn't preserve contents.
	void resize(size_t size) {
		if (size_ < size) {
			if (buf_ != NULL) {
				FreeMemoryPages(buf_, size_ * sizeof(T));
			}
			buf_ = (T *)AllocateMemoryPages(size * sizeof(T));
			size_ = size;
		}
	}

	T *data() {
		return buf_;
	}

	size_t size() {
		return size_;
	}

private:
	T *buf_;
	size_t size_;
};

#endif
