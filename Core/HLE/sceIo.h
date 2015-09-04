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

#include <string>

#include "Core/System.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/sceKernel.h"

void __IoInit();
void __IoDoState(PointerWrap &p);
void __IoShutdown();

struct ScePspDateTime;

u32 __IoGetFileHandleFromId(u32 id, u32 &outError);
void __IoCopyDate(ScePspDateTime& date_out, const tm& date_in);

KernelObject *__KernelFileNodeObject();
KernelObject *__KernelDirListingObject();

void Register_IoFileMgrForUser();
void Register_StdioForUser();
