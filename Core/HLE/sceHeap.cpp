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

#include "Common/ChunkFile.h"
#include "Core/Reporting.h"
#include "Core/HLE/HLE.h"
#include "sceKernelMemory.h"
#include "Core/HLE/sceHeap.h"
#include "../Util/BlockAllocator.h"
#include <map>

struct Heap {
	Heap():alloc(4) {}

	u32 size;
	u32 address;
	bool fromtop;
	BlockAllocator alloc;

	void DoState (PointerWrap &p) {
		p.Do(size);
		p.Do(address);
		p.Do(fromtop);
		p.Do(alloc);
	}
};

std::map<u32,Heap*> heapList;

static Heap *getHeap(u32 addr) {
	auto found = heapList.find(addr);
	if (found == heapList.end()) {
		return NULL;
	}
	return found->second;
}

void __HeapDoState(PointerWrap &p) {
	auto s = p.Section("sceHeap", 1, 2);
	if (!s)
		return;

	if (s >= 2) {
		p.Do(heapList);
	}
}

enum SceHeapAttr {
	PSP_HEAP_ATTR_HIGHMEM = 0x4000,
	PSP_HEAP_ATTR_EXT     = 0x8000,
};

void __HeapInit() {
	heapList.clear();
}

int sceHeapReallocHeapMemory(u32 heapAddr, u32 memPtr, int memSize) {
	ERROR_LOG_REPORT(HLE,"UNIMPL sceHeapReallocHeapMemory(%08x, %08x, %08x)", heapAddr, memPtr, memSize);
	return 0;
}

int sceHeapReallocHeapMemoryWithOption(u32 heapPtr, u32 memPtr, int memSize, u32 paramsPtr) {
	ERROR_LOG_REPORT(HLE,"UNIMPL sceHeapReallocHeapMemoryWithOption(%08x, %08x, %08x, %08x)", heapPtr, memPtr, memSize, paramsPtr);
	return 0;
}

int sceHeapFreeHeapMemory(u32 heapAddr, u32 memAddr) {
	Heap *heap = getHeap(heapAddr);
	if (!heap) {
		ERROR_LOG(HLE, "sceHeapFreeHeapMemory(%08x, %08x): invalid heap", heapAddr, memAddr);
		return SCE_KERNEL_ERROR_INVALID_ID;
	}

	DEBUG_LOG(HLE, "sceHeapFreeHeapMemory(%08x, %08x)", heapAddr, memAddr);
	// An invalid address will crash the PSP, but 0 is always returns success.
	if (memAddr == 0) {
		return 0;
	}

	if (!heap->alloc.FreeExact(memAddr)) {
		return SCE_KERNEL_ERROR_INVALID_POINTER;
	}
	return 0;
}

int sceHeapGetMallinfo(u32 heapAddr, u32 infoPtr) {
	ERROR_LOG_REPORT(HLE,"UNIMPL sceHeapGetMallinfo(%08x, %08x)", heapAddr, infoPtr);
	return 0;
}

u32 sceHeapAllocHeapMemoryWithOption(u32 heapAddr, u32 memSize, u32 paramsPtr) {
	Heap *heap = getHeap(heapAddr);
	u32 grain = 4;
	if (!heap) {
		ERROR_LOG(HLE, "sceHeapAllocHeapMemoryWithOption(%08x, %08x, %08x): invalid heap", heapAddr, memSize, paramsPtr);
		return 0;
	}

	// 0 is ignored.
	if (paramsPtr != 0) {
		u32 size = Memory::Read_U32(paramsPtr);
		if (size < 8) {
			ERROR_LOG(HLE, "sceHeapAllocHeapMemoryWithOption(%08x, %08x, %08x): invalid param size", heapAddr, memSize, paramsPtr);
			return 0;
		}
		if (size > 8) {
			WARN_LOG_REPORT(HLE, "sceHeapAllocHeapMemoryWithOption(): unexpected param size %d", size);
		}
		grain = Memory::Read_U32(paramsPtr + 4);
	}

	DEBUG_LOG(HLE,"sceHeapAllocHeapMemoryWithOption(%08x, %08x, %08x)", heapAddr, memSize, paramsPtr);
	// There's 8 bytes at the end of every block, reserved.
	memSize += 8;
	u32 addr = heap->alloc.AllocAligned(memSize, grain, grain, true);
	return addr;
}

int sceHeapGetTotalFreeSize(u32 heapAddr) {
	Heap *heap = getHeap(heapAddr);
	if (!heap) {
		ERROR_LOG(HLE, "sceHeapGetTotalFreeSize(%08x): invalid heap", heapAddr);
		return SCE_KERNEL_ERROR_INVALID_ID;
	}

	DEBUG_LOG(HLE, "sceHeapGetTotalFreeSize(%08x)", heapAddr);
	u32 free = heap->alloc.GetTotalFreeBytes();
	if (free >= 8) {
		// Every allocation requires an extra 8 bytes.
		free -= 8;
	}
	return free;
}

int sceHeapIsAllocatedHeapMemory(u32 heapPtr, u32 memPtr) {
	if (!Memory::IsValidAddress(memPtr)) {
		ERROR_LOG(HLE, "sceHeapIsAllocatedHeapMemory(%08x, %08x): invalid address", heapPtr, memPtr);
		return SCE_KERNEL_ERROR_INVALID_POINTER;
	}

	DEBUG_LOG(HLE, "sceHeapIsAllocatedHeapMemory(%08x, %08x)", heapPtr, memPtr);
	Heap *heap = getHeap(heapPtr);
	// An invalid heap is fine, it's not a member of this heap one way or another.
	// Only an exact address matches.  Off by one crashes, and off by 4 says no.
	if (heap && heap->alloc.GetBlockStartFromAddress(memPtr) == memPtr) {
		return 1;
	}
	return 0;
}

int sceHeapDeleteHeap(u32 heapAddr) {
	Heap *heap = getHeap(heapAddr);
	if (!heap) {
		ERROR_LOG(HLE, "sceHeapDeleteHeap(%08x): invalid heap", heapAddr);
		return SCE_KERNEL_ERROR_INVALID_ID;
	}

	DEBUG_LOG(HLE, "sceHeapDeleteHeap(%08x)", heapAddr);
	heapList.erase(heapAddr);
	delete heap;
	return 0;
}

int sceHeapCreateHeap(const char* name, u32 heapSize, int attr, u32 paramsPtr) {
	if (paramsPtr != 0) {
		u32 size = Memory::Read_U32(paramsPtr);
		WARN_LOG_REPORT(HLE, "sceHeapCreateHeap(): unsupported options parameter, size = %d", size);
	}	
	if (name == NULL) {
		WARN_LOG_REPORT(HLE, "sceHeapCreateHeap(): name is NULL");
		return 0;
	}
	int allocSize = (heapSize + 3) & ~3;

	Heap *heap = new Heap;
	heap->size = allocSize;
	heap->fromtop = (attr & PSP_HEAP_ATTR_HIGHMEM) != 0;
	u32 addr = userMemory.Alloc(heap->size, heap->fromtop, "Heap");
	if (addr == (u32)-1) {
		ERROR_LOG(HLE, "sceHeapCreateHeap(): Failed to allocate %i bytes memory", allocSize);	
		delete heap;
		return 0;
	}
	heap->address = addr;

	// Some of the heap is reseved by the implementation (the first 128 bytes, and 8 after each block.)
	heap->alloc.Init(heap->address + 128, heap->size - 128);
	heapList[heap->address] = heap;
	DEBUG_LOG(HLE, "%08x=sceHeapCreateHeap(%s, %08x, %08x, %08x)", heap->address, name, heapSize, attr, paramsPtr);
	return heap->address;
}

u32 sceHeapAllocHeapMemory(u32 heapAddr, u32 memSize) {
	Heap *heap = getHeap(heapAddr);
	if (!heap) {
		ERROR_LOG(HLE, "sceHeapAllocHeapMemory(%08x, %08x): invalid heap", heapAddr, memSize);
		// Yes, not 0 (returns a pointer), but an error code.  Strange.
		return SCE_KERNEL_ERROR_INVALID_ID;
	}

	DEBUG_LOG(HLE, "sceHeapAllocHeapMemory(%08x, %08x)", heapAddr, memSize);
	// There's 8 bytes at the end of every block, reserved.
	memSize += 8;
	// Always goes down, regardless of whether the heap is high or low.
	u32 addr = heap->alloc.Alloc(memSize, true);
	return addr;
}


static const HLEFunction sceHeap[] = 
{
	{0x0E875980,WrapI_UUI<sceHeapReallocHeapMemory>,"sceHeapReallocHeapMemory"},
	{0x1C84B58D,WrapI_UUIU<sceHeapReallocHeapMemoryWithOption>,"sceHeapReallocHeapMemoryWithOption"},
	{0x2ABADC63,WrapI_UU<sceHeapFreeHeapMemory>,"sceHeapFreeHeapMemory"},
	{0x2A0C2009,WrapI_UU<sceHeapGetMallinfo>,"sceHeapGetMallinfo"},
	{0x2B7299D8,WrapU_UUU<sceHeapAllocHeapMemoryWithOption>,"sceHeapAllocHeapMemoryWithOption"},
	{0x4929B40D,WrapI_U<sceHeapGetTotalFreeSize>,"sceHeapGetTotalFreeSize"},
	{0x7012BBDD,WrapI_UU<sceHeapIsAllocatedHeapMemory>,"sceHeapIsAllocatedHeapMemory"},
	{0x70210B73,WrapI_U<sceHeapDeleteHeap>,"sceHeapDeleteHeap"},
	{0x7DE281C2,WrapI_CUIU<sceHeapCreateHeap>,"sceHeapCreateHeap"},
	{0xA8E102A0,WrapU_UU<sceHeapAllocHeapMemory>,"sceHeapAllocHeapMemory"},
};

void Register_sceHeap()
{
	RegisterModule("sceHeap", ARRAY_SIZE(sceHeap), sceHeap);
}
