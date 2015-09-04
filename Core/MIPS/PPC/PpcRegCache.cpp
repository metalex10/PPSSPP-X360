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

#include <PpcEmitter.h>
#include "PpcRegCache.h"
#include "PpcJit.h"

using namespace PpcGen;

PpcRegCache::PpcRegCache(MIPSState *mips, MIPSComp::PpcJitOptions *options) : mips_(mips), options_(options) {
}

void PpcRegCache::Init(PPCXEmitter *emitter) {
	emit_ = emitter;
}

void PpcRegCache::Start(MIPSAnalyst::AnalysisResults &stats) {
	for (int i = 0; i < NUM_PPCREG; i++) {
		ar[i].mipsReg = -1;
		ar[i].isDirty = false;
	}
	for (int i = 0; i < NUM_MIPSREG; i++) {
		mr[i].loc = ML_MEM;
		mr[i].reg = INVALID_REG;
		mr[i].imm = -1;
		mr[i].spillLock = false;
	}
}

const PPCReg *PpcRegCache::GetMIPSAllocationOrder(int &count) {
	// Note that R0 is reserved as scratch for now.
	// R1 could be used as it's only used for scratch outside "regalloc space" now.
	// R12 is also potentially usable.
	// R4-R7 are registers we could use for static allocation or downcount.
	// R8 is used to preserve flags in nasty branches.
	// R9 and upwards are reserved for jit basics.
	if (options_->downcountInRegister) {
		static const PPCReg allocationOrder[] = {
			/*R14, R15, R16, R17, R18, R19,*/
			R20, R21, R22, R23, R24, R25, 
			R26, R27, R28, R29,	R30, R31,
		};
		count = sizeof(allocationOrder) / sizeof(const int);
		return allocationOrder;
	} else {
		static const PPCReg allocationOrder2[] = {
			/*R14, R15, R16, R17, R18, R19,*/
			R20, R21, R22, R23, R24, R25, 
			R26, R27, R28, R29,	R30, R31,
		};
		count = sizeof(allocationOrder2) / sizeof(const int);
		return allocationOrder2;
	}
}

void PpcRegCache::FlushBeforeCall() {
	// R4-R11 are preserved. Others need flushing.
	/*
	FlushPpcReg(R2);
	FlushPpcReg(R3);
	FlushPpcReg(R12);
	*/
}

// TODO: Somewhat smarter spilling - currently simply spills the first available, should do
// round robin or FIFO or something.
PPCReg PpcRegCache::MapReg(MIPSReg mipsReg, int mapFlags) {
	// Let's see if it's already mapped. If so we just need to update the dirty flag.
	// We don't need to check for ML_NOINIT because we assume that anyone who maps
	// with that flag immediately writes a "known" value to the register.
	if (mr[mipsReg].loc == ML_PPCREG) {
		if (ar[mr[mipsReg].reg].mipsReg != mipsReg) {
			ERROR_LOG(JIT, "Register mapping out of sync! %i", mipsReg);
		}
		if (mapFlags & MAP_DIRTY) {
			ar[mr[mipsReg].reg].isDirty = true;
		}
		return (PPCReg)mr[mipsReg].reg;
	}

	// Okay, not mapped, so we need to allocate an ARM register.

	int allocCount;
	const PPCReg *allocOrder = GetMIPSAllocationOrder(allocCount);

allocate:
	for (int i = 0; i < allocCount; i++) {
		int reg = allocOrder[i];

		if (ar[reg].mipsReg == -1) {
			// That means it's free. Grab it, and load the value into it (if requested).
			ar[reg].isDirty = (mapFlags & MAP_DIRTY) ? true : false;
			if (!(mapFlags & MAP_NOINIT)) {
				if (mr[mipsReg].loc == ML_MEM) {
					if (mipsReg != 0) {
						emit_->LWZ((PPCReg)reg, CTXREG, GetMipsRegOffset(mipsReg));
					} else {
						// If we get a request to load the zero register, at least we won't spend
						// time on a memory access...
						emit_->MOVI2R((PPCReg)reg, 0);
					}
				} else if (mr[mipsReg].loc == ML_IMM) {
					emit_->MOVI2R((PPCReg)reg, mr[mipsReg].imm);
					ar[reg].isDirty = true;  // IMM is always dirty.
				}
			}
			ar[reg].mipsReg = mipsReg;
			mr[mipsReg].loc = ML_PPCREG;
			mr[mipsReg].reg = (PPCReg)reg;
			return (PPCReg)reg;
		}
	}

	// Still nothing. Let's spill a reg and goto 10.
	// TODO: Use age or something to choose which register to spill?
	// TODO: Spill dirty regs first? or opposite?
	int bestToSpill = -1;
	for (int i = 0; i < allocCount; i++) {
		int reg = allocOrder[i];
		if (ar[reg].mipsReg != -1 && mr[ar[reg].mipsReg].spillLock)
			continue;
		bestToSpill = reg;
		break;
	}

	if (bestToSpill != -1) {
		// ERROR_LOG(JIT, "Out of registers at PC %08x - spills register %i.", mips_->pc, bestToSpill);
		FlushPpcReg((PPCReg)bestToSpill);
		goto allocate;
	}

	// Uh oh, we have all them spilllocked....
	ERROR_LOG(JIT, "Out of spillable registers at PC %08x!!!", mips_->pc);
	return INVALID_REG;
}

void PpcRegCache::MapInIn(MIPSReg rd, MIPSReg rs) {
	SpillLock(rd, rs);
	MapReg(rd);
	MapReg(rs);
	ReleaseSpillLocks();
}

void PpcRegCache::MapDirtyIn(MIPSReg rd, MIPSReg rs, bool avoidLoad) {
	SpillLock(rd, rs);
	bool load = !avoidLoad || rd == rs;
	MapReg(rd, MAP_DIRTY | (load ? 0 : MAP_NOINIT));
	MapReg(rs);
	ReleaseSpillLocks();
}

void PpcRegCache::MapDirtyInIn(MIPSReg rd, MIPSReg rs, MIPSReg rt, bool avoidLoad) {
	SpillLock(rd, rs, rt);
	bool load = !avoidLoad || (rd == rs || rd == rt);
	MapReg(rd, MAP_DIRTY | (load ? 0 : MAP_NOINIT));
	MapReg(rt);
	MapReg(rs);
	ReleaseSpillLocks();
}

void PpcRegCache::MapDirtyDirtyInIn(MIPSReg rd1, MIPSReg rd2, MIPSReg rs, MIPSReg rt, bool avoidLoad) {
	SpillLock(rd1, rd2, rs, rt);
	bool load1 = !avoidLoad || (rd1 == rs || rd1 == rt);
	bool load2 = !avoidLoad || (rd2 == rs || rd2 == rt);
	MapReg(rd1, MAP_DIRTY | (load1 ? 0 : MAP_NOINIT));
	MapReg(rd2, MAP_DIRTY | (load2 ? 0 : MAP_NOINIT));
	MapReg(rt);
	MapReg(rs);
	ReleaseSpillLocks();
}

void PpcRegCache::FlushPpcReg(PPCReg r) {
	if (ar[r].mipsReg == -1) {
		// Nothing to do, reg not mapped.
		return;
	}
	if (ar[r].mipsReg != -1) {
		if (ar[r].isDirty && mr[ar[r].mipsReg].loc == ML_PPCREG)
			emit_->STW(r, CTXREG, GetMipsRegOffset(ar[r].mipsReg));
		// IMMs won't be in an ARM reg.
		mr[ar[r].mipsReg].loc = ML_MEM;
		mr[ar[r].mipsReg].reg = INVALID_REG;
		mr[ar[r].mipsReg].imm = 0;
	} else {
		ERROR_LOG(JIT, "Dirty but no mipsreg?");
	}
	ar[r].isDirty = false;
	ar[r].mipsReg = -1;
}

void PpcRegCache::FlushR(MIPSReg r) {
	switch (mr[r].loc) {
	case ML_IMM:
		// IMM is always "dirty".
		emit_->MOVI2R(SREG, mr[r].imm);
		emit_->STW(SREG, CTXREG, GetMipsRegOffset(r));
		break;

	case ML_PPCREG:
		if (mr[r].reg == INVALID_REG) {
			ERROR_LOG(JIT, "FlushMipsReg: MipsReg had bad PpcReg");
		}
		if (ar[mr[r].reg].isDirty) {
			emit_->STW((PPCReg)mr[r].reg, CTXREG, GetMipsRegOffset(r));
			ar[mr[r].reg].isDirty = false;
		}
		ar[mr[r].reg].mipsReg = -1;
		break;

	case ML_MEM:
		// Already there, nothing to do.
		break;

	default:
		//BAD
		break;
	}
	mr[r].loc = ML_MEM;
	mr[r].reg = INVALID_REG;
	mr[r].imm = 0;
}

void PpcRegCache::FlushAll() {
	for (int i = 0; i < NUM_MIPSREG; i++) {
		FlushR(i);
	}
	// Sanity check
	for (int i = 0; i < NUM_PPCREG; i++) {
		if (ar[i].mipsReg != -1) {
			ERROR_LOG(JIT, "Flush fail: ar[%i].mipsReg=%i", i, ar[i].mipsReg);
		}
	}
}

void PpcRegCache::SetImm(MIPSReg r, u32 immVal) {
	if (r == 0)
		ERROR_LOG(JIT, "Trying to set immediate %08x to r0", immVal);

	// Zap existing value if cached in a reg
	if (mr[r].loc == ML_PPCREG) {
		ar[mr[r].reg].mipsReg = -1;
		ar[mr[r].reg].isDirty = false;
	}
	mr[r].loc = ML_IMM;
	mr[r].imm = immVal;
	mr[r].reg = INVALID_REG;
}

bool PpcRegCache::IsImm(MIPSReg r) const {
	if (r == 0) return true;
	return mr[r].loc == ML_IMM;
}

u32 PpcRegCache::GetImm(MIPSReg r) const {
	if (r == 0) return 0;
	if (mr[r].loc != ML_IMM) {
		ERROR_LOG(JIT, "Trying to get imm from non-imm register %i", r);
	}
	return mr[r].imm;
}

int PpcRegCache::GetMipsRegOffset(MIPSReg r) {
	if (r < 32)
		return r * 4;
	switch (r) {
	case MIPSREG_HI:
		return offsetof(MIPSState, hi);
	case MIPSREG_LO:
		return offsetof(MIPSState, lo);
	}
	ERROR_LOG(JIT, "bad mips register %i", r);
	return 0;  // or what?
}

void PpcRegCache::SpillLock(MIPSReg r1, MIPSReg r2, MIPSReg r3, MIPSReg r4) {
	mr[r1].spillLock = true;
	if (r2 != -1) mr[r2].spillLock = true;
	if (r3 != -1) mr[r3].spillLock = true;
	if (r4 != -1) mr[r4].spillLock = true;
}

void PpcRegCache::ReleaseSpillLocks() {
	for (int i = 0; i < NUM_MIPSREG; i++) {
		mr[i].spillLock = false;
	}
}

void PpcRegCache::ReleaseSpillLock(MIPSReg reg) {
	mr[reg].spillLock = false;
}

PPCReg PpcRegCache::R(int mipsReg) {
	if (mr[mipsReg].loc == ML_PPCREG) {
		return (PPCReg)mr[mipsReg].reg;
	} else {
		ERROR_LOG(JIT, "Reg %i not in ppc reg. compilerPC = %08x", mipsReg, compilerPC_);
		return INVALID_REG;  // BAAAD
	}
}
