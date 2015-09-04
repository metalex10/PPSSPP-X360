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

#ifndef _MEMARENA_H_
#define _MEMARENA_H_

#ifdef _WIN32
#include "CommonWindows.h"
#endif

#ifdef __SYMBIAN32__
#include <e32std.h>
#endif

#include "Common.h"

// This class lets you create a block of anonymous RAM, and then arbitrarily map views into it.
// Multiple views can mirror the same section of the block, which makes it very convient for emulating
// memory mirrors.

class MemArena
{
public:
	void GrabLowMemSpace(size_t size);
	void ReleaseSpace();
	void *CreateView(s64 offset, size_t size, void *base = 0);
	void ReleaseView(void *view, size_t size);

#ifdef __SYMBIAN32__
	RChunk* memmap;
#else
	// This only finds 1 GB in 32-bit
	static u8 *Find4GBBase();
#endif
private:

#ifdef _WIN32
	HANDLE hMemoryMapping;
#else
	int fd;
#endif
};

enum {
	MV_MIRROR_PREVIOUS = 1,
	// MV_FAKE_VMEM = 2,
	// MV_WII_ONLY = 4,
	MV_IS_PRIMARY_RAM = 0x100,
};

struct MemoryView
{
	u8 **out_ptr_low;
	u8 **out_ptr;
	u32 virtual_address;
	u32 size;
	u32 flags;
};

// Uses a memory arena to set up an emulator-friendly memory map according to
// a passed-in list of MemoryView structures.
u8 *MemoryMap_Setup(const MemoryView *views, int num_views, u32 flags, MemArena *arena);
void MemoryMap_Shutdown(const MemoryView *views, int num_views, u32 flags, MemArena *arena);

#endif // _MEMARENA_H_
