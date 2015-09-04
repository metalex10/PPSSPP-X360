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

#ifdef _WIN32
#include "Common/CommonWindows.h"
#else
#include <sys/time.h>
#endif

#include <time.h>

#include "Common/ChunkFile.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/sceKernelTime.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/sceRtc.h"

// The time when the game started.
static time_t start_time;

void __KernelTimeInit()
{
	time(&start_time);
}

void __KernelTimeDoState(PointerWrap &p)
{
	auto s = p.Section("sceKernelTime", 1);
	if (!s)
		return;

	p.Do(start_time);
}

int sceKernelGetSystemTime(u32 sysclockPtr)
{
	u64 t = CoreTiming::GetGlobalTimeUs();
	if (Memory::IsValidAddress(sysclockPtr)) 
		Memory::Write_U64(t, sysclockPtr);
	DEBUG_LOG(SCEKERNEL, "sceKernelGetSystemTime(out:%16llx)", t);
	hleEatCycles(265);
	hleReSchedule("system time");
	return 0;
}

u32 sceKernelGetSystemTimeLow()
{
	// This clock should tick at 1 Mhz.
	u64 t = CoreTiming::GetGlobalTimeUs();
	VERBOSE_LOG(SCEKERNEL,"%08x=sceKernelGetSystemTimeLow()",(u32)t);
	hleEatCycles(165);
	hleReSchedule("system time");
	return (u32)t;
}

u64 sceKernelGetSystemTimeWide()
{
	u64 t = CoreTiming::GetGlobalTimeUsScaled();
	DEBUG_LOG(SCEKERNEL,"%i=sceKernelGetSystemTimeWide()",(u32)t);
	hleEatCycles(250);
	hleReSchedule("system time");
	return t;
}

int sceKernelUSec2SysClock(u32 usec, u32 clockPtr)
{
	DEBUG_LOG(SCEKERNEL,"sceKernelUSec2SysClock(%i, %08x)", usec, clockPtr);
	if (Memory::IsValidAddress(clockPtr))
		Memory::Write_U64((usec & 0xFFFFFFFFL), clockPtr);
	hleEatCycles(165);
	return 0;
}

u64 sceKernelUSec2SysClockWide(u32 usec)
{
	DEBUG_LOG(SCEKERNEL, "sceKernelUSec2SysClockWide(%i)", usec);
	hleEatCycles(150);
	return usec; 
}

int sceKernelSysClock2USec(u32 sysclockPtr, u32 highPtr, u32 lowPtr)
{
	DEBUG_LOG(SCEKERNEL, "sceKernelSysClock2USec(clock = %08x, lo = %08x, hi = %08x)", sysclockPtr, highPtr, lowPtr);
	u64 time = Memory::Read_U64(sysclockPtr);
	u32 highResult = (u32)(time / 1000000);
	u32 lowResult = (u32)(time % 1000000);
	if (Memory::IsValidAddress(highPtr))
		Memory::Write_U32(highResult, highPtr);
	if (Memory::IsValidAddress(lowPtr))
		Memory::Write_U32(lowResult, lowPtr);
	hleEatCycles(415);
	return 0;
}

int sceKernelSysClock2USecWide(u32 lowClock, u32 highClock, u32 lowPtr, u32 highPtr)
{
	u64 sysClock = lowClock | ((u64)highClock << 32);
	DEBUG_LOG(SCEKERNEL, "sceKernelSysClock2USecWide(clock = %llu, lo = %08x, hi = %08x)", sysClock, lowPtr, highPtr);
	if (Memory::IsValidAddress(lowPtr)) {
		Memory::Write_U32((u32)(sysClock / 1000000), lowPtr);
		if (Memory::IsValidAddress(highPtr)) 
			Memory::Write_U32((u32)(sysClock % 1000000), highPtr);
	} else 
		if (Memory::IsValidAddress(highPtr)) 
			Memory::Write_U32((int) sysClock, highPtr);
	hleEatCycles(385);
	return 0;
}

u32 sceKernelLibcClock()
{
	u32 retVal = (u32) CoreTiming::GetGlobalTimeUs();
	DEBUG_LOG(SCEKERNEL, "%i = sceKernelLibcClock", retVal);
	hleEatCycles(330);
	hleReSchedule("libc clock");
	return retVal;
}

u32 sceKernelLibcTime(u32 outPtr)
{
	u32 t = (u32) start_time + (u32) (CoreTiming::GetGlobalTimeUs() / 1000000ULL);

	DEBUG_LOG(SCEKERNEL, "%i = sceKernelLibcTime(%08X)", t, outPtr);
	// The PSP sure takes its sweet time on this function.
	hleEatCycles(3385);

	if (Memory::IsValidAddress(outPtr))
		Memory::Write_U32(t, outPtr);
	else if (outPtr != 0)
		return 0;

	hleReSchedule("libc time");
	return t;
}

u32 sceKernelLibcGettimeofday(u32 timeAddr, u32 tzAddr)
{
	// TODO: tzAddr?
	if (Memory::IsValidAddress(timeAddr))
	{
		PSPTimeval *tv = (PSPTimeval *)Memory::GetPointer(timeAddr);
		__RtcTimeOfDay(tv);
	}

	DEBUG_LOG(SCEKERNEL,"sceKernelLibcGettimeofday(%08x, %08x)", timeAddr, tzAddr);
	hleEatCycles(1885);

	hleReSchedule("libc timeofday");
	return 0;
}
