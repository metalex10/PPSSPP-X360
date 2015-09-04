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

#include <algorithm>
#include <map>
#include "Common/ChunkFile.h"
#include "Core/HLE/HLE.h"
#include "Core/MIPS/MIPS.h"
#include "Core/CoreTiming.h"
#include "Core/Reporting.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/sceKernelMutex.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/KernelWaitHelpers.h"

#define PSP_MUTEX_ATTR_FIFO 0
#define PSP_MUTEX_ATTR_PRIORITY 0x100
#define PSP_MUTEX_ATTR_ALLOW_RECURSIVE 0x200
#define PSP_MUTEX_ATTR_KNOWN (PSP_MUTEX_ATTR_PRIORITY | PSP_MUTEX_ATTR_ALLOW_RECURSIVE)

// Not sure about the names of these
#define PSP_MUTEX_ERROR_NO_SUCH_MUTEX 0x800201C3
#define PSP_MUTEX_ERROR_TRYLOCK_FAILED 0x800201C4
#define PSP_MUTEX_ERROR_NOT_LOCKED 0x800201C5
#define PSP_MUTEX_ERROR_LOCK_OVERFLOW 0x800201C6
#define PSP_MUTEX_ERROR_UNLOCK_UNDERFLOW 0x800201C7
#define PSP_MUTEX_ERROR_ALREADY_LOCKED 0x800201C8

#define PSP_LWMUTEX_ERROR_NO_SUCH_LWMUTEX 0x800201CA
// Note: used only for _600.
#define PSP_LWMUTEX_ERROR_TRYLOCK_FAILED 0x800201CB
#define PSP_LWMUTEX_ERROR_NOT_LOCKED 0x800201CC
#define PSP_LWMUTEX_ERROR_LOCK_OVERFLOW 0x800201CD
#define PSP_LWMUTEX_ERROR_UNLOCK_UNDERFLOW 0x800201CE
#define PSP_LWMUTEX_ERROR_ALREADY_LOCKED 0x800201CF

struct NativeMutex
{
	SceSize_le size;
	char name[KERNELOBJECT_MAX_NAME_LENGTH + 1];
	SceUInt_le attr;
	s32_le initialCount;
	s32_le lockLevel;
	SceUID_le lockThread;
	// Not kept up to date.
	s32_le numWaitThreads;
};

struct Mutex : public KernelObject
{
	const char *GetName() {return nm.name;}
	const char *GetTypeName() {return "Mutex";}
	static u32 GetMissingErrorCode() { return PSP_MUTEX_ERROR_NO_SUCH_MUTEX; }
	static int GetStaticIDType() { return SCE_KERNEL_TMID_Mutex; }
	int GetIDType() const { return SCE_KERNEL_TMID_Mutex; }

	virtual void DoState(PointerWrap &p)
	{
		auto s = p.Section("Mutex", 1);
		if (!s)
			return;

		p.Do(nm);
		SceUID dv = 0;
		p.Do(waitingThreads, dv);
		p.Do(pausedWaits);
	}

	NativeMutex nm;
	std::vector<SceUID> waitingThreads;
	// Key is the callback id it was for, or if no callback, the thread id.
	std::map<SceUID, u64> pausedWaits;
};


struct NativeLwMutexWorkarea
{
	s32_le lockLevel;
	SceUID_le lockThread;
	u32_le attr;
	s32_le numWaitThreads;
	SceUID_le uid;
	s32_le pad[3];

	void init()
	{
		memset(this, 0, sizeof(NativeLwMutexWorkarea));
	}

	void clear()
	{
		lockLevel = 0;
		lockThread = -1;
		uid = -1;
	}
};

struct NativeLwMutex
{
	SceSize_le size;
	char name[KERNELOBJECT_MAX_NAME_LENGTH + 1];
	SceUInt_le attr;
	SceUID_le uid;
	PSPPointer<NativeLwMutexWorkarea> workarea;
	s32_le initialCount;
	// Not kept up to date.
	s32_le currentCount;
	// Not kept up to date.
	SceUID_le lockThread;
	// Not kept up to date.
	s32_le numWaitThreads;
};

struct LwMutex : public KernelObject
{
	const char *GetName() {return nm.name;}
	const char *GetTypeName() {return "LwMutex";}
	static u32 GetMissingErrorCode() { return PSP_LWMUTEX_ERROR_NO_SUCH_LWMUTEX; }
	static int GetStaticIDType() { return SCE_KERNEL_TMID_LwMutex; }
	int GetIDType() const { return SCE_KERNEL_TMID_LwMutex; }

	virtual void DoState(PointerWrap &p)
	{
		auto s = p.Section("LwMutex", 1);
		if (!s)
			return;

		p.Do(nm);
		SceUID dv = 0;
		p.Do(waitingThreads, dv);
		p.Do(pausedWaits);
	}

	NativeLwMutex nm;
	std::vector<SceUID> waitingThreads;
	// Key is the callback id it was for, or if no callback, the thread id.
	std::map<SceUID, u64> pausedWaits;
};

static int mutexWaitTimer = -1;
static int lwMutexWaitTimer = -1;
// Thread -> Mutex locks for thread end.
typedef std::multimap<SceUID, SceUID> MutexMap;
static MutexMap mutexHeldLocks;

void __KernelMutexBeginCallback(SceUID threadID, SceUID prevCallbackId);
void __KernelMutexEndCallback(SceUID threadID, SceUID prevCallbackId);
void __KernelLwMutexBeginCallback(SceUID threadID, SceUID prevCallbackId);
void __KernelLwMutexEndCallback(SceUID threadID, SceUID prevCallbackId);

void __KernelMutexInit()
{
	mutexWaitTimer = CoreTiming::RegisterEvent("MutexTimeout", __KernelMutexTimeout);
	lwMutexWaitTimer = CoreTiming::RegisterEvent("LwMutexTimeout", __KernelLwMutexTimeout);

	__KernelListenThreadEnd(&__KernelMutexThreadEnd);
	__KernelRegisterWaitTypeFuncs(WAITTYPE_MUTEX, __KernelMutexBeginCallback, __KernelMutexEndCallback);
	__KernelRegisterWaitTypeFuncs(WAITTYPE_LWMUTEX, __KernelLwMutexBeginCallback, __KernelLwMutexEndCallback);
}

void __KernelMutexDoState(PointerWrap &p)
{
	auto s = p.Section("sceKernelMutex", 1);
	if (!s)
		return;

	p.Do(mutexWaitTimer);
	CoreTiming::RestoreRegisterEvent(mutexWaitTimer, "MutexTimeout", __KernelMutexTimeout);
	p.Do(lwMutexWaitTimer);
	CoreTiming::RestoreRegisterEvent(lwMutexWaitTimer, "LwMutexTimeout", __KernelLwMutexTimeout);
	p.Do(mutexHeldLocks);
}

KernelObject *__KernelMutexObject()
{
	return new Mutex;
}

KernelObject *__KernelLwMutexObject()
{
	return new LwMutex;
}

void __KernelMutexShutdown()
{
	mutexHeldLocks.clear();
}

void __KernelMutexAcquireLock(Mutex *mutex, int count, SceUID thread)
{
#if defined(_DEBUG)
	std::pair<MutexMap::iterator, MutexMap::iterator> locked = mutexHeldLocks.equal_range(thread);
	for (MutexMap::iterator iter = locked.first; iter != locked.second; ++iter)
		_dbg_assert_msg_(SCEKERNEL, (*iter).second != mutex->GetUID(), "Thread %d / mutex %d wasn't removed from mutexHeldLocks properly.", thread, mutex->GetUID());
#endif

	mutexHeldLocks.insert(std::make_pair(thread, mutex->GetUID()));

	mutex->nm.lockLevel = count;
	mutex->nm.lockThread = thread;
}

void __KernelMutexAcquireLock(Mutex *mutex, int count)
{
	__KernelMutexAcquireLock(mutex, count, __KernelGetCurThread());
}

void __KernelMutexEraseLock(Mutex *mutex)
{
	if (mutex->nm.lockThread != -1)
	{
		SceUID id = mutex->GetUID();
		std::pair<MutexMap::iterator, MutexMap::iterator> locked = mutexHeldLocks.equal_range(mutex->nm.lockThread);
		for (MutexMap::iterator iter = locked.first; iter != locked.second; ++iter)
		{
			if ((*iter).second == id)
			{
				mutexHeldLocks.erase(iter);
				break;
			}
		}
	}
	mutex->nm.lockThread = -1;
}

std::vector<SceUID>::iterator __KernelMutexFindPriority(std::vector<SceUID> &waiting)
{
	_dbg_assert_msg_(SCEKERNEL, !waiting.empty(), "__KernelMutexFindPriority: Trying to find best of no threads.");

	std::vector<SceUID>::iterator iter, end, best = waiting.end();
	u32 best_prio = 0xFFFFFFFF;
	for (iter = waiting.begin(), end = waiting.end(); iter != end; ++iter)
	{
		u32 iter_prio = __KernelGetThreadPrio(*iter);
		if (iter_prio < best_prio)
		{
			best = iter;
			best_prio = iter_prio;
		}
	}

	_dbg_assert_msg_(SCEKERNEL, best != waiting.end(), "__KernelMutexFindPriority: Returning invalid best thread.");
	return best;
}

bool __KernelUnlockMutexForThread(Mutex *mutex, SceUID threadID, u32 &error, int result)
{
	if (!HLEKernel::VerifyWait(threadID, WAITTYPE_MUTEX, mutex->GetUID()))
		return false;

	// If result is an error code, we're just letting it go.
	if (result == 0)
	{
		int wVal = (int)__KernelGetWaitValue(threadID, error);
		__KernelMutexAcquireLock(mutex, wVal, threadID);
	}

	u32 timeoutPtr = __KernelGetWaitTimeoutPtr(threadID, error);
	if (timeoutPtr != 0 && mutexWaitTimer != -1)
	{
		// Remove any event for this thread.
		s64 cyclesLeft = CoreTiming::UnscheduleEvent(mutexWaitTimer, threadID);
		Memory::Write_U32((u32) cyclesToUs(cyclesLeft), timeoutPtr);
	}

	__KernelResumeThreadFromWait(threadID, result);
	return true;
}

bool __KernelUnlockMutexForThreadCheck(Mutex *mutex, SceUID threadID, u32 &error, int result, bool &wokeThreads)
{
	if (mutex->nm.lockThread == -1 && __KernelUnlockMutexForThread(mutex, threadID, error, 0))
		return true;
	return false;
}

void __KernelMutexBeginCallback(SceUID threadID, SceUID prevCallbackId)
{
	auto result = HLEKernel::WaitBeginCallback<Mutex, WAITTYPE_MUTEX, SceUID>(threadID, prevCallbackId, mutexWaitTimer);
	if (result == HLEKernel::WAIT_CB_SUCCESS)
		DEBUG_LOG(SCEKERNEL, "sceKernelLockMutexCB: Suspending lock wait for callback")
	else
		WARN_LOG_REPORT(SCEKERNEL, "sceKernelLockMutexCB: beginning callback with bad wait id?");
}

void __KernelMutexEndCallback(SceUID threadID, SceUID prevCallbackId)
{
	auto result = HLEKernel::WaitEndCallback<Mutex, WAITTYPE_MUTEX, SceUID>(threadID, prevCallbackId, mutexWaitTimer, __KernelUnlockMutexForThreadCheck);
	if (result == HLEKernel::WAIT_CB_RESUMED_WAIT)
		DEBUG_LOG(SCEKERNEL, "sceKernelLockMutexCB: Resuming lock wait for callback");
}

int sceKernelCreateMutex(const char *name, u32 attr, int initialCount, u32 optionsPtr)
{
	if (!name)
	{
		WARN_LOG_REPORT(SCEKERNEL, "%08x=sceKernelCreateMutex(): invalid name", SCE_KERNEL_ERROR_ERROR);
		return SCE_KERNEL_ERROR_ERROR;
	}
	if (attr & ~0xBFF)
	{
		WARN_LOG_REPORT(SCEKERNEL, "%08x=sceKernelCreateMutex(): invalid attr parameter: %08x", SCE_KERNEL_ERROR_ILLEGAL_ATTR, attr);
		return SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	}

	if (initialCount < 0)
		return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if ((attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) == 0 && initialCount > 1)
		return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

	Mutex *mutex = new Mutex();
	SceUID id = kernelObjects.Create(mutex);

	mutex->nm.size = sizeof(mutex->nm);
	strncpy(mutex->nm.name, name, KERNELOBJECT_MAX_NAME_LENGTH);
	mutex->nm.name[KERNELOBJECT_MAX_NAME_LENGTH] = 0;
	mutex->nm.attr = attr;
	mutex->nm.initialCount = initialCount;
	if (initialCount == 0)
	{
		mutex->nm.lockLevel = 0;
		mutex->nm.lockThread = -1;
	}
	else
		__KernelMutexAcquireLock(mutex, initialCount);

	DEBUG_LOG(SCEKERNEL, "%i=sceKernelCreateMutex(%s, %08x, %d, %08x)", id, name, attr, initialCount, optionsPtr);

	if (optionsPtr != 0)
	{
		u32 size = Memory::Read_U32(optionsPtr);
		if (size > 4)
			WARN_LOG_REPORT(SCEKERNEL, "sceKernelCreateMutex(%s) unsupported options parameter, size = %d", name, size);
	}
	if ((attr & ~PSP_MUTEX_ATTR_KNOWN) != 0)
		WARN_LOG_REPORT(SCEKERNEL, "sceKernelCreateMutex(%s) unsupported attr parameter: %08x", name, attr);

	return id;
}

int sceKernelDeleteMutex(SceUID id)
{
	u32 error;
	Mutex *mutex = kernelObjects.Get<Mutex>(id, error);
	if (mutex)
	{
		DEBUG_LOG(SCEKERNEL, "sceKernelDeleteMutex(%i)", id);
		bool wokeThreads = false;
		std::vector<SceUID>::iterator iter, end;
		for (iter = mutex->waitingThreads.begin(), end = mutex->waitingThreads.end(); iter != end; ++iter)
			wokeThreads |= __KernelUnlockMutexForThread(mutex, *iter, error, SCE_KERNEL_ERROR_WAIT_DELETE);

		if (mutex->nm.lockThread != -1)
			__KernelMutexEraseLock(mutex);
		mutex->waitingThreads.clear();

		if (wokeThreads)
			hleReSchedule("mutex deleted");

		return kernelObjects.Destroy<Mutex>(id);
	}
	else
	{
		DEBUG_LOG(SCEKERNEL, "sceKernelDeleteMutex(%i): invalid mutex", id);
		return error;
	}
}

bool __KernelLockMutexCheck(Mutex *mutex, int count, u32 &error)
{
	if (error)
		return false;

	const bool mutexIsRecursive = (mutex->nm.attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) != 0;

	if (count <= 0)
		error = SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	else if (count > 1 && !mutexIsRecursive)
		error = SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	// Two positive ints will always overflow to negative.
	else if (count + mutex->nm.lockLevel < 0)
		error = PSP_MUTEX_ERROR_LOCK_OVERFLOW;
	// Only a recursive mutex can re-lock.
	else if (mutex->nm.lockThread == __KernelGetCurThread())
	{
		if (mutexIsRecursive)
			return true;

		error = PSP_MUTEX_ERROR_ALREADY_LOCKED;
	}
	// Otherwise it would lock or wait.
	else if (mutex->nm.lockLevel == 0)
		return true;

	return false;
}

bool __KernelLockMutex(Mutex *mutex, int count, u32 &error)
{
	if (!__KernelLockMutexCheck(mutex, count, error))
		return false;

	if (mutex->nm.lockLevel == 0)
	{
		__KernelMutexAcquireLock(mutex, count);
		// Nobody had it locked - no need to block
		return true;
	}

	if (mutex->nm.lockThread == __KernelGetCurThread())
	{
		// __KernelLockMutexCheck() would've returned an error, so this must be recursive.
		mutex->nm.lockLevel += count;
		return true;
	}

	return false;
}

bool __KernelUnlockMutex(Mutex *mutex, u32 &error)
{
	__KernelMutexEraseLock(mutex);

	bool wokeThreads = false;
	std::vector<SceUID>::iterator iter;
	while (!wokeThreads && !mutex->waitingThreads.empty())
	{
		if ((mutex->nm.attr & PSP_MUTEX_ATTR_PRIORITY) != 0)
			iter = __KernelMutexFindPriority(mutex->waitingThreads);
		else
			iter = mutex->waitingThreads.begin();

		wokeThreads |= __KernelUnlockMutexForThread(mutex, *iter, error, 0);
		mutex->waitingThreads.erase(iter);
	}

	if (!wokeThreads)
		mutex->nm.lockThread = -1;

	return wokeThreads;
}

void __KernelMutexTimeout(u64 userdata, int cyclesLate)
{
	SceUID threadID = (SceUID)userdata;
	HLEKernel::WaitExecTimeout<Mutex, WAITTYPE_MUTEX>(threadID);
}

void __KernelMutexThreadEnd(SceUID threadID)
{
	u32 error;

	// If it was waiting on the mutex, it should finish now.
	SceUID waitingMutexID = __KernelGetWaitID(threadID, WAITTYPE_MUTEX, error);
	if (waitingMutexID)
	{
		Mutex *mutex = kernelObjects.Get<Mutex>(waitingMutexID, error);
		if (mutex)
			HLEKernel::RemoveWaitingThread(mutex->waitingThreads, threadID);
	}

	// Unlock all mutexes the thread had locked.
	std::pair<MutexMap::iterator, MutexMap::iterator> locked = mutexHeldLocks.equal_range(threadID);
	for (MutexMap::iterator iter = locked.first; iter != locked.second; )
	{
		// Need to increment early so erase() doesn't invalidate.
		SceUID mutexID = (*iter++).second;
		Mutex *mutex = kernelObjects.Get<Mutex>(mutexID, error);

		if (mutex)
		{
			mutex->nm.lockLevel = 0;
			__KernelUnlockMutex(mutex, error);
		}
	}
}

void __KernelWaitMutex(Mutex *mutex, u32 timeoutPtr)
{
	if (timeoutPtr == 0 || mutexWaitTimer == -1)
		return;

	int micro = (int) Memory::Read_U32(timeoutPtr);

	// This happens to be how the hardware seems to time things.
	if (micro <= 3)
		micro = 15;
	else if (micro <= 249)
		micro = 250;

	// This should call __KernelMutexTimeout() later, unless we cancel it.
	CoreTiming::ScheduleEvent(usToCycles(micro), mutexWaitTimer, __KernelGetCurThread());
}

int sceKernelCancelMutex(SceUID uid, int count, u32 numWaitThreadsPtr)
{
	u32 error;
	Mutex *mutex = kernelObjects.Get<Mutex>(uid, error);
	if (mutex)
	{
		bool lockable = count <= 0 || __KernelLockMutexCheck(mutex, count, error);
		if (!lockable)
		{
			// May still be okay.  As long as the count/etc. are valid.
			if (error != 0 && error != PSP_MUTEX_ERROR_LOCK_OVERFLOW && error != PSP_MUTEX_ERROR_ALREADY_LOCKED)
			{
				DEBUG_LOG(SCEKERNEL, "sceKernelCancelMutex(%i, %d, %08x): invalid count", uid, count, numWaitThreadsPtr);
				return error;
			}
		}

		DEBUG_LOG(SCEKERNEL, "sceKernelCancelMutex(%i, %d, %08x)", uid, count, numWaitThreadsPtr);

		// Remove threads no longer waiting on this first (so the numWaitThreads value is correct.)
		HLEKernel::CleanupWaitingThreads(WAITTYPE_MUTEX, uid, mutex->waitingThreads);

		if (Memory::IsValidAddress(numWaitThreadsPtr))
			Memory::Write_U32((u32)mutex->waitingThreads.size(), numWaitThreadsPtr);

		bool wokeThreads = false;
		for (auto iter = mutex->waitingThreads.begin(), end = mutex->waitingThreads.end(); iter != end; ++iter)
			wokeThreads |= __KernelUnlockMutexForThread(mutex, *iter, error, SCE_KERNEL_ERROR_WAIT_CANCEL);

		if (mutex->nm.lockThread != -1)
			__KernelMutexEraseLock(mutex);
		mutex->waitingThreads.clear();

		if (count <= 0)
		{
			mutex->nm.lockLevel = 0;
			mutex->nm.lockThread = -1;
		}
		else
			__KernelMutexAcquireLock(mutex, count);

		if (wokeThreads)
			hleReSchedule("mutex canceled");

		return 0;
	}
	else
	{
		DEBUG_LOG(SCEKERNEL, "sceKernelCancelMutex(%i, %d, %08x)", uid, count, numWaitThreadsPtr);
		return error;
	}
}

// int sceKernelLockMutex(SceUID id, int count, int *timeout)
int sceKernelLockMutex(SceUID id, int count, u32 timeoutPtr)
{
	DEBUG_LOG(SCEKERNEL, "sceKernelLockMutex(%i, %i, %08x)", id, count, timeoutPtr);
	u32 error;
	Mutex *mutex = kernelObjects.Get<Mutex>(id, error);

	if (__KernelLockMutex(mutex, count, error))
		return 0;
	else if (error)
		return error;
	else
	{
		SceUID threadID = __KernelGetCurThread();
		// May be in a tight loop timing out (where we don't remove from waitingThreads yet), don't want to add duplicates.
		if (std::find(mutex->waitingThreads.begin(), mutex->waitingThreads.end(), threadID) == mutex->waitingThreads.end())
			mutex->waitingThreads.push_back(threadID);
		__KernelWaitMutex(mutex, timeoutPtr);
		__KernelWaitCurThread(WAITTYPE_MUTEX, id, count, timeoutPtr, false, "mutex waited");

		// Return value will be overwritten by wait.
		return 0;
	}
}

// int sceKernelLockMutexCB(SceUID id, int count, int *timeout)
int sceKernelLockMutexCB(SceUID id, int count, u32 timeoutPtr)
{
	DEBUG_LOG(SCEKERNEL, "sceKernelLockMutexCB(%i, %i, %08x)", id, count, timeoutPtr);
	u32 error;
	Mutex *mutex = kernelObjects.Get<Mutex>(id, error);

	if (!__KernelLockMutexCheck(mutex, count, error))
	{
		if (error)
			return error;

		SceUID threadID = __KernelGetCurThread();
		// May be in a tight loop timing out (where we don't remove from waitingThreads yet), don't want to add duplicates.
		if (std::find(mutex->waitingThreads.begin(), mutex->waitingThreads.end(), threadID) == mutex->waitingThreads.end())
			mutex->waitingThreads.push_back(threadID);
		__KernelWaitMutex(mutex, timeoutPtr);
		__KernelWaitCurThread(WAITTYPE_MUTEX, id, count, timeoutPtr, true, "mutex waited");

		// Return value will be overwritten by wait.
		return 0;
	}
	else
	{
		if (__KernelCurHasReadyCallbacks())
		{
			// Might actually end up having to wait, so set the timeout.
			__KernelWaitMutex(mutex, timeoutPtr);
			__KernelWaitCallbacksCurThread(WAITTYPE_MUTEX, id, count, timeoutPtr);

			// Return value will be written to callback's v0, but... that's probably fine?
		}
		else
			__KernelLockMutex(mutex, count, error);

		return 0;
	}
}

// int sceKernelTryLockMutex(SceUID id, int count)
int sceKernelTryLockMutex(SceUID id, int count)
{
	DEBUG_LOG(SCEKERNEL, "sceKernelTryLockMutex(%i, %i)", id, count);
	u32 error;
	Mutex *mutex = kernelObjects.Get<Mutex>(id, error);

	if (__KernelLockMutex(mutex, count, error))
		return 0;
	else if (error)
		return error;
	else
		return PSP_MUTEX_ERROR_TRYLOCK_FAILED;
}

// int sceKernelUnlockMutex(SceUID id, int count)
int sceKernelUnlockMutex(SceUID id, int count)
{
	DEBUG_LOG(SCEKERNEL, "sceKernelUnlockMutex(%i, %i)", id, count);
	u32 error;
	Mutex *mutex = kernelObjects.Get<Mutex>(id, error);

	if (error)
		return error;
	if (count <= 0)
		return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if ((mutex->nm.attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) == 0 && count > 1)
		return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if (mutex->nm.lockLevel == 0 || mutex->nm.lockThread != __KernelGetCurThread())
		return PSP_MUTEX_ERROR_NOT_LOCKED;
	if (mutex->nm.lockLevel < count)
		return PSP_MUTEX_ERROR_UNLOCK_UNDERFLOW;

	mutex->nm.lockLevel -= count;

	if (mutex->nm.lockLevel == 0)
	{
		if (__KernelUnlockMutex(mutex, error))
			hleReSchedule("mutex unlocked");
	}

	return 0;
}

int sceKernelReferMutexStatus(SceUID id, u32 infoAddr)
{
	u32 error;
	Mutex *m = kernelObjects.Get<Mutex>(id, error);
	if (!m)
	{
		ERROR_LOG(SCEKERNEL, "sceKernelReferMutexStatus(%i, %08x): invalid mutex id", id, infoAddr);
		return error;
	}

	DEBUG_LOG(SCEKERNEL, "sceKernelReferMutexStatus(%08x, %08x)", id, infoAddr);

	// Should we crash the thread somehow?
	if (!Memory::IsValidAddress(infoAddr))
		return -1;

	// Don't write if the size is 0.  Anything else is A-OK, though, apparently.
	if (Memory::Read_U32(infoAddr) != 0)
	{
		HLEKernel::CleanupWaitingThreads(WAITTYPE_MUTEX, id, m->waitingThreads);

		m->nm.numWaitThreads = (int) m->waitingThreads.size();
		Memory::WriteStruct(infoAddr, &m->nm);
	}
	return 0;
}

int sceKernelCreateLwMutex(u32 workareaPtr, const char *name, u32 attr, int initialCount, u32 optionsPtr)
{
	if (!name)
	{
		WARN_LOG_REPORT(SCEKERNEL, "%08x=sceKernelCreateLwMutex(): invalid name", SCE_KERNEL_ERROR_ERROR);
		return SCE_KERNEL_ERROR_ERROR;
	}
	if (attr >= 0x400)
	{
		WARN_LOG_REPORT(SCEKERNEL, "%08x=sceKernelCreateLwMutex(): invalid attr parameter: %08x", SCE_KERNEL_ERROR_ILLEGAL_ATTR, attr);
		return SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	}

	if (initialCount < 0)
		return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if ((attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) == 0 && initialCount > 1)
		return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

	LwMutex *mutex = new LwMutex();
	SceUID id = kernelObjects.Create(mutex);
	mutex->nm.size = sizeof(mutex->nm);
	strncpy(mutex->nm.name, name, KERNELOBJECT_MAX_NAME_LENGTH);
	mutex->nm.name[KERNELOBJECT_MAX_NAME_LENGTH] = 0;
	mutex->nm.attr = attr;
	mutex->nm.uid = id;
	mutex->nm.workarea = workareaPtr;
	mutex->nm.initialCount = initialCount;
	auto workarea = Memory::GetStruct<NativeLwMutexWorkarea>(workareaPtr);
	workarea->init();
	workarea->lockLevel = initialCount;
	if (initialCount == 0)
		workarea->lockThread = 0;
	else
		workarea->lockThread = __KernelGetCurThread();
	workarea->attr = attr;
	workarea->uid = id;

	DEBUG_LOG(SCEKERNEL, "sceKernelCreateLwMutex(%08x, %s, %08x, %d, %08x)", workareaPtr, name, attr, initialCount, optionsPtr);

	if (optionsPtr != 0)
	{
		u32 size = Memory::Read_U32(optionsPtr);
		if (size > 4)
			WARN_LOG_REPORT(SCEKERNEL, "sceKernelCreateLwMutex(%s) unsupported options parameter, size = %d", name, size);
	}
	if ((attr & ~PSP_MUTEX_ATTR_KNOWN) != 0)
		WARN_LOG_REPORT(SCEKERNEL, "sceKernelCreateLwMutex(%s) unsupported attr parameter: %08x", name, attr);

	return 0;
}

template <typename T>
bool __KernelUnlockLwMutexForThread(LwMutex *mutex, T workarea, SceUID threadID, u32 &error, int result)
{
	if (!HLEKernel::VerifyWait(threadID, WAITTYPE_LWMUTEX, mutex->GetUID()))
		return false;

	// If result is an error code, we're just letting it go.
	if (result == 0)
	{
		workarea->lockLevel = (int) __KernelGetWaitValue(threadID, error);
		workarea->lockThread = threadID;
	}

	u32 timeoutPtr = __KernelGetWaitTimeoutPtr(threadID, error);
	if (timeoutPtr != 0 && lwMutexWaitTimer != -1)
	{
		// Remove any event for this thread.
		s64 cyclesLeft = CoreTiming::UnscheduleEvent(lwMutexWaitTimer, threadID);
		Memory::Write_U32((u32) cyclesToUs(cyclesLeft), timeoutPtr);
	}

	__KernelResumeThreadFromWait(threadID, result);
	return true;
}

int sceKernelDeleteLwMutex(u32 workareaPtr)
{
	DEBUG_LOG(SCEKERNEL, "sceKernelDeleteLwMutex(%08x)", workareaPtr);

	if (!workareaPtr || !Memory::IsValidAddress(workareaPtr))
		return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

	auto workarea = Memory::GetStruct<NativeLwMutexWorkarea>(workareaPtr);

	u32 error;
	LwMutex *mutex = kernelObjects.Get<LwMutex>(workarea->uid, error);
	if (mutex)
	{
		bool wokeThreads = false;
		std::vector<SceUID>::iterator iter, end;
		for (iter = mutex->waitingThreads.begin(), end = mutex->waitingThreads.end(); iter != end; ++iter)
			wokeThreads |= __KernelUnlockLwMutexForThread(mutex, workarea, *iter, error, SCE_KERNEL_ERROR_WAIT_DELETE);
		mutex->waitingThreads.clear();

		workarea->clear();

		if (wokeThreads)
			hleReSchedule("lwmutex deleted");

		return kernelObjects.Destroy<LwMutex>(mutex->GetUID());
	}
	else
		return error;
}

template <typename T>
bool __KernelLockLwMutex(T workarea, int count, u32 &error)
{
	if (!error)
	{
		if (count <= 0)
			error = SCE_KERNEL_ERROR_ILLEGAL_COUNT;
		else if (count > 1 && !(workarea->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE))
			error = SCE_KERNEL_ERROR_ILLEGAL_COUNT;
		// Two positive ints will always overflow to negative.
		else if (count + workarea->lockLevel < 0)
			error = PSP_LWMUTEX_ERROR_LOCK_OVERFLOW;
		else if (workarea->uid == -1)
			error = PSP_LWMUTEX_ERROR_NO_SUCH_LWMUTEX;
	}

	if (error)
		return false;

	if (workarea->lockLevel == 0)
	{
		if (workarea->lockThread != 0)
		{
			// Validate that it actually exists so we can return an error if not.
			kernelObjects.Get<LwMutex>(workarea->uid, error);
			if (error)
				return false;
		}

		workarea->lockLevel = count;
		workarea->lockThread = __KernelGetCurThread();
		return true;
	}

	if (workarea->lockThread == __KernelGetCurThread())
	{
		// Recursive mutex, let's just increase the lock count and keep going
		if (workarea->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE)
		{
			workarea->lockLevel += count;
			return true;
		}
		else
		{
			error = PSP_LWMUTEX_ERROR_ALREADY_LOCKED;
			return false;
		}
	}

	return false;
}

template <typename T>
bool __KernelUnlockLwMutex(T workarea, u32 &error)
{
	LwMutex *mutex = kernelObjects.Get<LwMutex>(workarea->uid, error);
	if (error)
	{
		workarea->lockThread = 0;
		return false;
	}

	bool wokeThreads = false;
	std::vector<SceUID>::iterator iter;
	while (!wokeThreads && !mutex->waitingThreads.empty())
	{
		if ((mutex->nm.attr & PSP_MUTEX_ATTR_PRIORITY) != 0)
			iter = __KernelMutexFindPriority(mutex->waitingThreads);
		else
			iter = mutex->waitingThreads.begin();

		wokeThreads |= __KernelUnlockLwMutexForThread(mutex, workarea, *iter, error, 0);
		mutex->waitingThreads.erase(iter);
	}

	if (!wokeThreads)
		workarea->lockThread = 0;

	return wokeThreads;
}

void __KernelLwMutexTimeout(u64 userdata, int cyclesLate)
{
	SceUID threadID = (SceUID)userdata;
	HLEKernel::WaitExecTimeout<LwMutex, WAITTYPE_LWMUTEX>(threadID);
}

void __KernelWaitLwMutex(LwMutex *mutex, u32 timeoutPtr)
{
	if (timeoutPtr == 0 || lwMutexWaitTimer == -1)
		return;

	int micro = (int) Memory::Read_U32(timeoutPtr);

	// This happens to be how the hardware seems to time things.
	if (micro <= 3)
		micro = 15;
	else if (micro <= 249)
		micro = 250;

	// This should call __KernelLwMutexTimeout() later, unless we cancel it.
	CoreTiming::ScheduleEvent(usToCycles(micro), lwMutexWaitTimer, __KernelGetCurThread());
}

bool __KernelUnlockLwMutexForThreadCheck(LwMutex *mutex, SceUID threadID, u32 &error, int result, bool &wokeThreads)
{
	if (mutex->nm.lockThread == -1 && __KernelUnlockLwMutexForThread(mutex, mutex->nm.workarea, threadID, error, 0))
		return true;
	return false;
}

void __KernelLwMutexBeginCallback(SceUID threadID, SceUID prevCallbackId)
{
	auto result = HLEKernel::WaitBeginCallback<LwMutex, WAITTYPE_LWMUTEX, SceUID>(threadID, prevCallbackId, lwMutexWaitTimer);
	if (result == HLEKernel::WAIT_CB_SUCCESS)
		DEBUG_LOG(SCEKERNEL, "sceKernelLockLwMutexCB: Suspending lock wait for callback")
	else
		WARN_LOG_REPORT(SCEKERNEL, "sceKernelLockLwMutexCB: beginning callback with bad wait id?");
}

void __KernelLwMutexEndCallback(SceUID threadID, SceUID prevCallbackId)
{
	auto result = HLEKernel::WaitEndCallback<LwMutex, WAITTYPE_LWMUTEX, SceUID>(threadID, prevCallbackId, lwMutexWaitTimer, __KernelUnlockLwMutexForThreadCheck);
	if (result == HLEKernel::WAIT_CB_RESUMED_WAIT)
		DEBUG_LOG(SCEKERNEL, "sceKernelLockLwMutexCB: Resuming lock wait for callback");
}

int sceKernelTryLockLwMutex(u32 workareaPtr, int count)
{
	DEBUG_LOG(SCEKERNEL, "sceKernelTryLockLwMutex(%08x, %i)", workareaPtr, count);

	auto workarea = Memory::GetStruct<NativeLwMutexWorkarea>(workareaPtr);

	u32 error = 0;
	if (__KernelLockLwMutex(workarea, count, error))
		return 0;
	// Unlike sceKernelTryLockLwMutex_600, this always returns the same error.
	else if (error)
		return PSP_MUTEX_ERROR_TRYLOCK_FAILED;
	else
		return PSP_MUTEX_ERROR_TRYLOCK_FAILED;
}

int sceKernelTryLockLwMutex_600(u32 workareaPtr, int count)
{
	DEBUG_LOG(SCEKERNEL, "sceKernelTryLockLwMutex_600(%08x, %i)", workareaPtr, count);

	auto workarea = Memory::GetStruct<NativeLwMutexWorkarea>(workareaPtr);

	u32 error = 0;
	if (__KernelLockLwMutex(workarea, count, error))
		return 0;
	else if (error)
		return error;
	else
		return PSP_LWMUTEX_ERROR_TRYLOCK_FAILED;
}

int sceKernelLockLwMutex(u32 workareaPtr, int count, u32 timeoutPtr)
{
	VERBOSE_LOG(SCEKERNEL, "sceKernelLockLwMutex(%08x, %i, %08x)", workareaPtr, count, timeoutPtr);

	auto workarea = Memory::GetStruct<NativeLwMutexWorkarea>(workareaPtr);

	u32 error = 0;
	if (__KernelLockLwMutex(workarea, count, error))
		return 0;
	else if (error)
		return error;
	else
	{
		LwMutex *mutex = kernelObjects.Get<LwMutex>(workarea->uid, error);
		if (mutex)
		{
			SceUID threadID = __KernelGetCurThread();
			// May be in a tight loop timing out (where we don't remove from waitingThreads yet), don't want to add duplicates.
			if (std::find(mutex->waitingThreads.begin(), mutex->waitingThreads.end(), threadID) == mutex->waitingThreads.end())
				mutex->waitingThreads.push_back(threadID);
			__KernelWaitLwMutex(mutex, timeoutPtr);
			__KernelWaitCurThread(WAITTYPE_LWMUTEX, workarea->uid, count, timeoutPtr, false, "lwmutex waited");

			// Return value will be overwritten by wait.
			return 0;
		}
		else
			return error;
	}
}

int sceKernelLockLwMutexCB(u32 workareaPtr, int count, u32 timeoutPtr)
{
	VERBOSE_LOG(SCEKERNEL, "sceKernelLockLwMutexCB(%08x, %i, %08x)", workareaPtr, count, timeoutPtr);

	auto workarea = Memory::GetStruct<NativeLwMutexWorkarea>(workareaPtr);

	u32 error = 0;
	if (__KernelLockLwMutex(workarea, count, error))
		return 0;
	else if (error)
		return error;
	else
	{
		LwMutex *mutex = kernelObjects.Get<LwMutex>(workarea->uid, error);
		if (mutex)
		{
			SceUID threadID = __KernelGetCurThread();
			// May be in a tight loop timing out (where we don't remove from waitingThreads yet), don't want to add duplicates.
			if (std::find(mutex->waitingThreads.begin(), mutex->waitingThreads.end(), threadID) == mutex->waitingThreads.end())
				mutex->waitingThreads.push_back(threadID);
			__KernelWaitLwMutex(mutex, timeoutPtr);
			__KernelWaitCurThread(WAITTYPE_LWMUTEX, workarea->uid, count, timeoutPtr, true, "lwmutex cb waited");

			// Return value will be overwritten by wait.
			return 0;
		}
		else
			return error;
	}
}

int sceKernelUnlockLwMutex(u32 workareaPtr, int count)
{
	VERBOSE_LOG(SCEKERNEL, "sceKernelUnlockLwMutex(%08x, %i)", workareaPtr, count);

	auto workarea = Memory::GetStruct<NativeLwMutexWorkarea>(workareaPtr);

	if (workarea->uid == -1)
		return PSP_LWMUTEX_ERROR_NO_SUCH_LWMUTEX;
	else if (count <= 0)
		return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	else if ((workarea->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) == 0 && count > 1)
		return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	else if (workarea->lockLevel == 0 || workarea->lockThread != __KernelGetCurThread())
		return PSP_LWMUTEX_ERROR_NOT_LOCKED;
	else if (workarea->lockLevel < count)
		return PSP_LWMUTEX_ERROR_UNLOCK_UNDERFLOW;

	workarea->lockLevel -= count;

	if (workarea->lockLevel == 0)
	{
		u32 error;
		if (__KernelUnlockLwMutex(workarea, error))
			hleReSchedule("lwmutex unlocked");
	}

	return 0;
}

int __KernelReferLwMutexStatus(SceUID uid, u32 infoPtr)
{
	u32 error;
	LwMutex *m = kernelObjects.Get<LwMutex>(uid, error);
	if (!m)
		return error;

	// Should we crash the thread somehow?
	if (!Memory::IsValidAddress(infoPtr))
		return -1;

	if (Memory::Read_U32(infoPtr) != 0)
	{
		auto workarea = m->nm.workarea;

		HLEKernel::CleanupWaitingThreads(WAITTYPE_LWMUTEX, uid, m->waitingThreads);

		// Refresh and write
		m->nm.currentCount = workarea->lockLevel;
		m->nm.lockThread = workarea->lockThread == 0 ? -1 : workarea->lockThread;
		m->nm.numWaitThreads = (int) m->waitingThreads.size();
		Memory::WriteStruct(infoPtr, &m->nm);
	}
	return 0;
}

int sceKernelReferLwMutexStatusByID(SceUID uid, u32 infoPtr)
{
	int error = __KernelReferLwMutexStatus(uid, infoPtr);
	if (error >= 0)
	{
		DEBUG_LOG(SCEKERNEL, "sceKernelReferLwMutexStatusByID(%08x, %08x)", uid, infoPtr);
		return error;
	}
	else
	{
		ERROR_LOG(SCEKERNEL, "%08x=sceKernelReferLwMutexStatusByID(%08x, %08x)", error, uid, infoPtr);
		return error;
	}
}

int sceKernelReferLwMutexStatus(u32 workareaPtr, u32 infoPtr)
{
	if (!Memory::IsValidAddress(workareaPtr))
		return -1;

	auto workarea = Memory::GetStruct<NativeLwMutexWorkarea>(workareaPtr);

	int error = __KernelReferLwMutexStatus(workarea->uid, infoPtr);
	if (error >= 0)
	{
		DEBUG_LOG(SCEKERNEL, "sceKernelReferLwMutexStatus(%08x, %08x)", workareaPtr, infoPtr);
		return error;
	}
	else
	{
		ERROR_LOG(SCEKERNEL, "%08x=sceKernelReferLwMutexStatus(%08x, %08x)", error, workareaPtr, infoPtr);
		return error;
	}
}