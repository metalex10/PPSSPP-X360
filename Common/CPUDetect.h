// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

// Detect the cpu, so we'll know which optimizations to use
#ifndef _CPUDETECT_H_
#define _CPUDETECT_H_

// Every architecture has its own define. This needs to be added to.
#if defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_7S__)
#define HAVE_ARMV7 1
#endif

#include <string>

enum CPUVendor {
	VENDOR_INTEL = 0,
	VENDOR_AMD = 1,
	VENDOR_ARM = 2,
	VENDOR_OTHER = 3,
};

struct CPUInfo {
	CPUVendor vendor;

	char cpu_string[0x21];
	char brand_string[0x41];
	bool OS64bit;
	bool CPU64bit;
	bool Mode64bit;

	bool HTT;
	int num_cores;
	int logical_cpu_count;

	bool bSSE;
	bool bSSE2;
	bool bSSE3;
	bool bSSSE3;
	bool bPOPCNT;
	bool bSSE4_1;
	bool bSSE4_2;
	bool bLZCNT;
	bool bSSE4A;
	bool bAVX;
	bool bFMA;
	bool bAES;
	bool bLAHFSAHF64;
	bool bLongMode;

	// ARM specific CPUInfo
	bool bSwp;
	bool bHalf;
	bool bThumb;
	bool bFastMult;
	bool bVFP;
	bool bEDSP;
	bool bThumbEE;
	bool bNEON;
	bool bVFPv3;
	bool bTLS;
	bool bVFPv4;
	bool bIDIVa;
	bool bIDIVt;

	// ARMv8 specific
	bool bFP;
	bool bASIMD;

	// Call Detect()
	explicit CPUInfo();

	// Turn the cpu info into a string we can show
	std::string Summarize();

private:
	// Detects the various cpu features
	void Detect();
};

extern CPUInfo cpu_info;

#endif // _CPUDETECT_H_
