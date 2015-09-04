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

class PointerWrap;

int sceKernelCreateMsgPipe(const char *name, int partition, u32 attr, u32 size, u32 optionsPtr);
int sceKernelDeleteMsgPipe(SceUID uid);
int sceKernelSendMsgPipe(SceUID uid, u32 sendBufAddr, u32 sendSize, u32 waitMode, u32 resultAddr, u32 timeoutPtr);
int sceKernelSendMsgPipeCB(SceUID uid, u32 sendBufAddr, u32 sendSize, u32 waitMode, u32 resultAddr, u32 timeoutPtr);
int sceKernelTrySendMsgPipe(SceUID uid, u32 sendBufAddr, u32 sendSize, u32 waitMode, u32 resultAddr);
int sceKernelReceiveMsgPipe(SceUID uid, u32 receiveBufAddr, u32 receiveSize, u32 waitMode, u32 resultAddr, u32 timeoutPtr);
int sceKernelReceiveMsgPipeCB(SceUID uid, u32 receiveBufAddr, u32 receiveSize, u32 waitMode, u32 resultAddr, u32 timeoutPtr);
int sceKernelTryReceiveMsgPipe(SceUID uid, u32 receiveBufAddr, u32 receiveSize, u32 waitMode, u32 resultAddr);
int sceKernelCancelMsgPipe(SceUID uid, u32 numSendThreadsAddr, u32 numReceiveThreadsAddr);
int sceKernelReferMsgPipeStatus(SceUID uid, u32 statusPtr);

void __KernelMsgPipeInit();
void __KernelMsgPipeDoState(PointerWrap &p);
KernelObject *__KernelMsgPipeObject();
