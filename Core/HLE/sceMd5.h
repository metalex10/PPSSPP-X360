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

#include "HLE.h"

void Register_sceMd5();

u32 sceKernelUtilsMt19937Init(u32 ctx, u32 seed);
u32 sceKernelUtilsMt19937UInt(u32 ctx);

int sceKernelUtilsMd5Digest(u32 dataAddr, int len, u32 digestAddr);
int sceKernelUtilsMd5BlockInit(u32 ctxAddr);
int sceKernelUtilsMd5BlockUpdate(u32 ctxAddr, u32 dataAddr, int len);
int sceKernelUtilsMd5BlockResult(u32 ctxAddr, u32 digestAddr);

int sceKernelUtilsSha1Digest(u32 dataAddr, int len, u32 digestAddr);
int sceKernelUtilsSha1BlockInit(u32 ctxAddr);
int sceKernelUtilsSha1BlockUpdate(u32 ctxAddr, u32 dataAddr, int len);
int sceKernelUtilsSha1BlockResult(u32 ctxAddr, u32 digestAddr);