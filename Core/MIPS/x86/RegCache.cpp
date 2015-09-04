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

#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSTables.h"
#include "Core/MIPS/MIPSAnalyst.h"
#include "Core/MIPS/x86/Jit.h"
#include "Core/MIPS/x86/Asm.h"
#include "Core/MIPS/x86/RegCache.h"

using namespace Gen;

static const int allocationOrder[] = 
{
	// R12, when used as base register, for example in a LEA, can generate bad code! Need to look into this.
	// On x64, RCX and RDX are the first args.  CallProtectedFunction() assumes they're not regcached.
#ifdef _M_X64
#ifdef _WIN32
	RSI, RDI, R13, R14, R8, R9, R10, R11, R12,
#else
	RBP, R13, R14, R8, R9, R10, R11, R12,
#endif
#elif _M_IX86
	ESI, EDI, EBP, EDX, ECX,  // Let's try to free up EBX as well.
#endif
};

void GPRRegCache::FlushBeforeCall() {
	// TODO: Only flush the non-preserved-by-callee registers.
	Flush();
}

GPRRegCache::GPRRegCache() : mips(0), emit(0) {
	memset(regs, 0, sizeof(regs));
	memset(xregs, 0, sizeof(xregs));
}

void GPRRegCache::Start(MIPSState *mips, MIPSAnalyst::AnalysisResults &stats) {
	this->mips = mips;
	for (int i = 0; i < NUM_X_REGS; i++) {
		xregs[i].free = true;
		xregs[i].dirty = false;
		xregs[i].allocLocked = false;
	}
	memset(regs, 0, sizeof(regs));
	OpArg base = GetDefaultLocation(MIPS_REG_ZERO);
	for (int i = 0; i < NUM_MIPS_GPRS; i++) {
		regs[i].location = base;
		base.IncreaseOffset(sizeof(u32));
	}

	// todo: sort to find the most popular regs
	/*
	int maxPreload = 2;
	for (int i = 0; i < NUM_MIPS_GPRS; i++)
	{
		if (stats.numReads[i] > 2 || stats.numWrites[i] >= 2)
		{
			LoadToX64(i, true, false); //stats.firstRead[i] <= stats.firstWrite[i], false);
			maxPreload--;
			if (!maxPreload)
				break;
		}
	}*/
	//Find top regs - preload them (load bursts ain't bad)
	//But only preload IF written OR reads >= 3
}


// these are MIPS reg indices
void GPRRegCache::Lock(MIPSGPReg p1, MIPSGPReg p2, MIPSGPReg p3, MIPSGPReg p4) {
	regs[p1].locked = true;
	if (p2 != MIPS_REG_INVALID) regs[p2].locked = true;
	if (p3 != MIPS_REG_INVALID) regs[p3].locked = true;
	if (p4 != MIPS_REG_INVALID) regs[p4].locked = true;
}

// these are x64 reg indices
void GPRRegCache::LockX(int x1, int x2, int x3, int x4) {
	if (xregs[x1].allocLocked) {
		PanicAlert("RegCache: x %i already locked!", x1);
	}
	xregs[x1].allocLocked = true;
	if (x2 != 0xFF) xregs[x2].allocLocked = true;
	if (x3 != 0xFF) xregs[x3].allocLocked = true;
	if (x4 != 0xFF) xregs[x4].allocLocked = true;
}

void GPRRegCache::UnlockAll() {
	for (int i = 0; i < NUM_MIPS_GPRS; i++)
		regs[i].locked = false;
}

void GPRRegCache::UnlockAllX() {
	for (int i = 0; i < NUM_X_REGS; i++)
		xregs[i].allocLocked = false;
}

X64Reg GPRRegCache::GetFreeXReg()
{
	int aCount;
	const int *aOrder = GetAllocationOrder(aCount);
	for (int i = 0; i < aCount; i++)
	{
		X64Reg xr = (X64Reg)aOrder[i];
		if (!xregs[xr].allocLocked && xregs[xr].free)
		{
			return (X64Reg)xr;
		}
	}
	//Okay, not found :( Force grab one

	//TODO - add a pass to grab xregs whose mipsreg is not used in the next 3 instructions
	for (int i = 0; i < aCount; i++)
	{
		X64Reg xr = (X64Reg)aOrder[i];
		if (xregs[xr].allocLocked) 
			continue;
		MIPSGPReg preg = xregs[xr].mipsReg;
		if (!regs[preg].locked)
		{
			StoreFromRegister(preg);
			return xr;
		}
	}
	//Still no dice? Die!
	_assert_msg_(JIT, 0, "Regcache ran out of regs");
	return (X64Reg) -1;
}

void GPRRegCache::FlushR(X64Reg reg)
{
	if (reg >= NUM_X_REGS)
		PanicAlert("Flushing non existent reg");
	else if (!xregs[reg].free)
		StoreFromRegister(xregs[reg].mipsReg);
}

int GPRRegCache::SanityCheck() const {
	for (int i = 0; i < NUM_MIPS_GPRS; i++) {
		const MIPSGPReg r = MIPSGPReg(i);
		if (regs[i].away) {
			if (regs[i].location.IsSimpleReg()) {
				Gen::X64Reg simple = regs[i].location.GetSimpleReg();
				if (xregs[simple].allocLocked)
					return 1;
				if (xregs[simple].mipsReg != r)
					return 2;
			}
			else if (regs[i].location.IsImm())
				return 3;
		}
	}
	return 0;
}

void GPRRegCache::DiscardRegContentsIfCached(MIPSGPReg preg) {
	if (regs[preg].away && regs[preg].location.IsSimpleReg()) {
		X64Reg xr = regs[preg].location.GetSimpleReg();
		xregs[xr].free = true;
		xregs[xr].dirty = false;
		xregs[xr].mipsReg = MIPS_REG_INVALID;
		regs[preg].away = false;
		regs[preg].location = GetDefaultLocation(preg);
	}
}


void GPRRegCache::SetImm(MIPSGPReg preg, u32 immValue) {
	// ZERO is always zero.  Let's just make sure.
	if (preg == MIPS_REG_ZERO)
		immValue = 0;

	DiscardRegContentsIfCached(preg);
	regs[preg].away = true;
	regs[preg].location = Imm32(immValue);
}

bool GPRRegCache::IsImm(MIPSGPReg preg) const {
	// Always say yes for ZERO, even if it's in a temp reg.
	if (preg == MIPS_REG_ZERO)
		return true;
	return regs[preg].location.IsImm();
}

u32 GPRRegCache::GetImm(MIPSGPReg preg) const {
	_dbg_assert_msg_(JIT, IsImm(preg), "Reg %d must be an immediate.", preg);
	// Always 0 for ZERO.
	if (preg == MIPS_REG_ZERO)
		return 0;
	return regs[preg].location.GetImmValue();
}

const int *GPRRegCache::GetAllocationOrder(int &count) {
	count = sizeof(allocationOrder) / sizeof(const int);
	return allocationOrder;
}


OpArg GPRRegCache::GetDefaultLocation(MIPSGPReg reg) const {
	return M(&mips->r[reg]);
}


void GPRRegCache::KillImmediate(MIPSGPReg preg, bool doLoad, bool makeDirty) {
	if (regs[preg].away) {
		if (regs[preg].location.IsImm())
			MapReg(preg, doLoad, makeDirty);
		else if (regs[preg].location.IsSimpleReg())
			xregs[RX(preg)].dirty |= makeDirty;
	}
}

void GPRRegCache::MapReg(MIPSGPReg i, bool doLoad, bool makeDirty) {
	if (!regs[i].away && regs[i].location.IsImm())
		PanicAlert("Bad immediate");

	if (!regs[i].away || (regs[i].away && regs[i].location.IsImm())) {
		X64Reg xr = GetFreeXReg();
		if (xregs[xr].dirty) PanicAlert("Xreg already dirty");
		if (xregs[xr].allocLocked) PanicAlert("GetFreeXReg returned locked register");
		xregs[xr].free = false;
		xregs[xr].mipsReg = i;
		xregs[xr].dirty = makeDirty || regs[i].location.IsImm();
		OpArg newloc = ::Gen::R(xr);
		if (doLoad) {
			// Force ZERO to be 0.
			if (i == MIPS_REG_ZERO)
				emit->MOV(32, newloc, Imm32(0));
			else
				emit->MOV(32, newloc, regs[i].location);
		}
		for (int j = 0; j < 32; j++) {
			if (i != MIPSGPReg(j) && regs[j].location.IsSimpleReg(xr)) {
				ERROR_LOG(JIT, "BindToRegister: Strange condition");
				Crash();
			}
		}
		regs[i].away = true;
		regs[i].location = newloc;
	} else {
		// reg location must be simplereg; memory locations
		// and immediates are taken care of above.
		xregs[RX(i)].dirty |= makeDirty;
	}
	if (xregs[RX(i)].allocLocked) {
		PanicAlert("Seriously WTF, this reg should have been flushed");
	}
}

void GPRRegCache::StoreFromRegister(MIPSGPReg i) {
	if (regs[i].away) {
		bool doStore;
		if (regs[i].location.IsSimpleReg()) {
			X64Reg xr = RX(i);
			xregs[xr].free = true;
			xregs[xr].mipsReg = MIPS_REG_INVALID;
			doStore = xregs[xr].dirty;
			xregs[xr].dirty = false;
		} else {
			//must be immediate - do nothing
			doStore = true;
		}
		OpArg newLoc = GetDefaultLocation(i);
		// But never store to ZERO.
		if (doStore && i != MIPS_REG_ZERO)
			emit->MOV(32, newLoc, regs[i].location);
		regs[i].location = newLoc;
		regs[i].away = false;
	}
}

void GPRRegCache::Flush() {
	for (int i = 0; i < NUM_X_REGS; i++) {
		if (xregs[i].allocLocked)
			PanicAlert("Someone forgot to unlock X64 reg %i.", i);
	}
	for (int i = 0; i < NUM_MIPS_GPRS; i++) {
		const MIPSGPReg r = MIPSGPReg(i);
		if (regs[i].locked) {
			PanicAlert("Somebody forgot to unlock MIPS reg %i.", i);
		}
		if (regs[i].away) {
			if (regs[i].location.IsSimpleReg()) {
				X64Reg xr = RX(r);
				StoreFromRegister(r);
				xregs[xr].dirty = false;
			}
			else if (regs[i].location.IsImm()) {
				StoreFromRegister(r);
			} else {
				_assert_msg_(JIT,0,"Jit64 - Flush unhandled case, reg %i PC: %08x", i, mips->pc);
			}
		}
	}
}

void GPRRegCache::GetState(GPRRegCacheState &state) const {
	memcpy(state.regs, regs, sizeof(regs));
	memcpy(state.xregs, xregs, sizeof(xregs));
}

void GPRRegCache::RestoreState(const GPRRegCacheState state) {
	memcpy(regs, state.regs, sizeof(regs));
	memcpy(xregs, state.xregs, sizeof(xregs));
}
