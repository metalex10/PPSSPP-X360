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

#ifndef _ATOMIC_GCC_H_
#define _ATOMIC_GCC_H_

#ifdef BLACKBERRY
#include <atomic.h>
#elif defined(__SYMBIAN32__)
#include <e32atomics.h>
#endif

#include "Common.h"

// Atomic operations are performed in a single step by the CPU. It is
// impossible for other threads to see the operation "half-done."
//
// Some atomic operations can be combined with different types of memory
// barriers called "Acquire semantics" and "Release semantics", defined below.
//
// Acquire semantics: Future memory accesses cannot be relocated to before the
//                    operation.
//
// Release semantics: Past memory accesses cannot be relocated to after the
//                    operation.
//
// These barriers affect not only the compiler, but also the CPU.

namespace Common
{

inline void AtomicAdd(volatile u32& target, u32 value) {
	__sync_add_and_fetch(&target, value);
}

inline void AtomicAnd(volatile u32& target, u32 value) {
	__sync_and_and_fetch(&target, value);
}

inline void AtomicDecrement(volatile u32& target) {
	__sync_add_and_fetch(&target, -1);
}

inline void AtomicIncrement(volatile u32& target) {
	__sync_add_and_fetch(&target, 1);
}

inline u32 AtomicLoad(volatile u32& src) {
	return src; // 32-bit reads are always atomic.
}
inline u32 AtomicLoadAcquire(volatile u32& src) {
#ifdef __SYMBIAN32__
	return __e32_atomic_load_acq32(&src);
#else
	//keep the compiler from caching any memory references
	u32 result = src; // 32-bit reads are always atomic.
	//__sync_synchronize(); // TODO: May not be necessary.
	// Compiler instruction only. x86 loads always have acquire semantics.
	__asm__ __volatile__ ( "":::"memory" );
	return result;
#endif
}

inline void AtomicOr(volatile u32& target, u32 value) {
	__sync_or_and_fetch(&target, value);
}

inline void AtomicStore(volatile u32& dest, u32 value) {
	dest = value; // 32-bit writes are always atomic.
}
inline void AtomicStoreRelease(volatile u32& dest, u32 value) {
#ifdef BLACKBERRY
	atomic_set(&dest, value);
#elif defined(__SYMBIAN32__)
    __e32_atomic_store_rel32(&dest, value);
#else
	__sync_lock_test_and_set(&dest, value); // TODO: Wrong! This function has acquire semantics.
#endif
}

}

#endif
