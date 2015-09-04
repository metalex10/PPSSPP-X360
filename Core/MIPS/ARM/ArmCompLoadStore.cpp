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


// Optimization ideas:
//
// It's common to see sequences of stores writing or reading to a contiguous set of
// addresses in function prologues/epilogues:
//  sw s5, 104(sp)
//  sw s4, 100(sp)
//  sw s3, 96(sp)
//  sw s2, 92(sp)
//  sw s1, 88(sp)
//  sw s0, 84(sp)
//  sw ra, 108(sp)
//  mov s4, a0
//  mov s3, a1
//  ...
// Such sequences could easily be detected and turned into nice contiguous
// sequences of ARM stores instead of the current 3 instructions per sw/lw.
//
// Also, if we kept track of the likely register content of a cached register,
// (pointer or data), we could avoid many BIC instructions.


#include "Core/MemMap.h"
#include "Core/Config.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSAnalyst.h"
#include "Core/MIPS/MIPSCodeUtils.h"
#include "Core/MIPS/ARM/ArmJit.h"
#include "Core/MIPS/ARM/ArmRegCache.h"

#define _RS MIPS_GET_RS(op)
#define _RT MIPS_GET_RT(op)
#define _RD MIPS_GET_RD(op)
#define _FS MIPS_GET_FS(op)
#define _FT MIPS_GET_FT(op)
#define _FD MIPS_GET_FD(op)
#define _SA MIPS_GET_SA(op)
#define _POS  ((op>> 6) & 0x1F)
#define _SIZE ((op>>11) & 0x1F)
#define _IMM16 (signed short)(op & 0xFFFF)
#define _IMM26 (op & 0x03FFFFFF)

// All functions should have CONDITIONAL_DISABLE, so we can narrow things down to a file quickly.
// Currently known non working ones should have DISABLE.

// #define CONDITIONAL_DISABLE { Comp_Generic(op); return; }
#define CONDITIONAL_DISABLE ;
#define DISABLE { Comp_Generic(op); return; }

namespace MIPSComp
{
	void Jit::SetR0ToEffectiveAddress(MIPSGPReg rs, s16 offset) {
		Operand2 op2;
		if (offset) {
			bool negated;
			if (TryMakeOperand2_AllowNegation(offset, op2, &negated)) {
				if (!negated)
					ADD(R0, gpr.R(rs), op2);
				else
					SUB(R0, gpr.R(rs), op2);
			} else {
				// Try to avoid using MOVT
				if (offset < 0) {
					gpr.SetRegImm(R0, (u32)(-offset));
					SUB(R0, gpr.R(rs), R0);
				} else {
					gpr.SetRegImm(R0, (u32)offset);
					ADD(R0, gpr.R(rs), R0);
				}
			}
			BIC(R0, R0, Operand2(0xC0, 4));   // &= 0x3FFFFFFF
		} else {
			BIC(R0, gpr.R(rs), Operand2(0xC0, 4));   // &= 0x3FFFFFFF
		}
	}

	void Jit::SetCCAndR0ForSafeAddress(MIPSGPReg rs, s16 offset, ARMReg tempReg, bool reverse) {
		SetR0ToEffectiveAddress(rs, offset);

		// There are three valid ranges.  Each one gets a bit.
		const u32 BIT_SCRATCH = 1, BIT_RAM = 2, BIT_VRAM = 4;
		MOVI2R(tempReg, BIT_SCRATCH | BIT_RAM | BIT_VRAM);

		CMP(R0, AssumeMakeOperand2(PSP_GetScratchpadMemoryBase()));
		SetCC(CC_LO);
		BIC(tempReg, tempReg, BIT_SCRATCH);
		SetCC(CC_HS);
		CMP(R0, AssumeMakeOperand2(PSP_GetScratchpadMemoryEnd()));
		BIC(tempReg, tempReg, BIT_SCRATCH);

		// If it was in that range, later compares don't matter.
		CMP(R0, AssumeMakeOperand2(PSP_GetVidMemBase()));
		SetCC(CC_LO);
		BIC(tempReg, tempReg, BIT_VRAM);
		SetCC(CC_HS);
		CMP(R0, AssumeMakeOperand2(PSP_GetVidMemEnd()));
		BIC(tempReg, tempReg, BIT_VRAM);

		CMP(R0, AssumeMakeOperand2(PSP_GetKernelMemoryBase()));
		SetCC(CC_LO);
		BIC(tempReg, tempReg, BIT_RAM);
		SetCC(CC_HS);
		CMP(R0, AssumeMakeOperand2(PSP_GetUserMemoryEnd()));
		BIC(tempReg, tempReg, BIT_RAM);

		// If we left any bit set, the address is OK.
		SetCC(CC_AL);
		CMP(tempReg, 0);
		SetCC(reverse ? CC_EQ : CC_GT);
	}

	void Jit::Comp_ITypeMemLR(MIPSOpcode op, bool load) {
		CONDITIONAL_DISABLE;
		int offset = (signed short)(op & 0xFFFF);
		MIPSGPReg rt = _RT;
		MIPSGPReg rs = _RS;
		int o = op >> 26;

		if (!js.inDelaySlot) {
			// Optimisation: Combine to single unaligned load/store
			bool isLeft = (o == 34 || o == 42);
			MIPSOpcode nextOp = Memory::Read_Instruction(js.compilerPC + 4);
			// Find a matching shift in opposite direction with opposite offset.
			if (nextOp == (isLeft ? (op.encoding + (4<<26) - 3)
				                  : (op.encoding - (4<<26) + 3)))
			{
				EatInstruction(nextOp);
				nextOp = MIPSOpcode(((load ? 35 : 43) << 26) | ((isLeft ? nextOp : op) & 0x03FFFFFF)); //lw, sw
				Comp_ITypeMem(nextOp);
				return;
			}
		}

		u32 iaddr = gpr.IsImm(rs) ? offset + gpr.GetImm(rs) : 0xFFFFFFFF;
		bool doCheck = false;
		FixupBranch skip;

		if (gpr.IsImm(rs) && Memory::IsValidAddress(iaddr)) {
			u32 addr = iaddr & 0x3FFFFFFF;
			// Must be OK even if rs == rt since we have the value from imm already.
			gpr.MapReg(rt, load ? MAP_NOINIT | MAP_DIRTY : 0);
			gpr.SetRegImm(R0, addr & ~3);

			u8 shift = (addr & 3) * 8;

			switch (o) {
			case 34: // lwl
				LDR(R0, MEMBASEREG, R0);
				ANDI2R(gpr.R(rt), gpr.R(rt), 0x00ffffff >> shift, R1);
				ORR(gpr.R(rt), gpr.R(rt), Operand2(R0, ST_LSL, 24 - shift));
				break;

			case 38: // lwr
				LDR(R0, MEMBASEREG, R0);
				ANDI2R(gpr.R(rt), gpr.R(rt), 0xffffff00 << (24 - shift), R1);
				ORR(gpr.R(rt), gpr.R(rt), Operand2(R0, ST_LSR, shift));
				break;

			case 42: // swl
				LDR(R1, MEMBASEREG, R0);
				// Don't worry, can't use temporary.
				ANDI2R(R1, R1, 0xffffff00 << shift, R0);
				ORR(R1, R1, Operand2(gpr.R(rt), ST_LSR, 24 - shift));
				STR(R1, MEMBASEREG, R0);
				break;

			case 46: // swr
				LDR(R1, MEMBASEREG, R0);
				// Don't worry, can't use temporary.
				ANDI2R(R1, R1, 0x00ffffff >> (24 - shift), R0);
				ORR(R1, R1, Operand2(gpr.R(rt), ST_LSL, shift));
				STR(R1, MEMBASEREG, R0);
				break;
			}
			return;
		}

		_dbg_assert_msg_(JIT, !gpr.IsImm(rs), "Invalid immediate address?  CPU bug?");
		load ? gpr.MapDirtyIn(rt, rs, false) : gpr.MapInIn(rt, rs);

		if (!g_Config.bFastMemory && rs != MIPS_REG_SP) {
			SetCCAndR0ForSafeAddress(rs, offset, R1, true);
			doCheck = true;
		} else {
			SetR0ToEffectiveAddress(rs, offset);
		}
		if (doCheck) {
			skip = B();
		}
		SetCC(CC_AL);

		// Need temp regs.  TODO: Get from the regcache?
		if (load) {
			PUSH(1, R10);
		} else {
			PUSH(2, R9, R10);
		}

		// Here's our shift amount.
		AND(R1, R0, 3);
		LSL(R1, R1, 3);

		// Now align the address for the actual read.
		BIC(R0, R0, 3);

		switch (o) {
		case 34: // lwl
			MOVI2R(R10, 0x00ffffff);
			LDR(R0, MEMBASEREG, R0);
			AND(gpr.R(rt), gpr.R(rt), Operand2(R10, ST_LSR, R1));
			RSB(R1, R1, 24);
			ORR(gpr.R(rt), gpr.R(rt), Operand2(R0, ST_LSL, R1));
			break;

		case 38: // lwr
			MOVI2R(R10, 0xffffff00);
			LDR(R0, MEMBASEREG, R0);
			LSR(R0, R0, R1);
			RSB(R1, R1, 24);
			AND(gpr.R(rt), gpr.R(rt), Operand2(R10, ST_LSL, R1));
			ORR(gpr.R(rt), gpr.R(rt), R0);
			break;

		case 42: // swl
			MOVI2R(R10, 0xffffff00);
			LDR(R9, MEMBASEREG, R0);
			AND(R9, R9, Operand2(R10, ST_LSL, R1));
			RSB(R1, R1, 24);
			ORR(R9, R9, Operand2(gpr.R(rt), ST_LSR, R1));
			STR(R9, MEMBASEREG, R0);
			break;

		case 46: // swr
			MOVI2R(R10, 0x00ffffff);
			LDR(R9, MEMBASEREG, R0);
			RSB(R1, R1, 24);
			AND(R9, R9, Operand2(R10, ST_LSR, R1));
			RSB(R1, R1, 24);
			ORR(R9, R9, Operand2(gpr.R(rt), ST_LSL, R1));
			STR(R9, MEMBASEREG, R0);
			break;
		}

		if (load) {
			POP(1, R10);
		} else {
			POP(2, R9, R10);
		}

		if (doCheck) {
			SetJumpTarget(skip);
		}
	}

	void Jit::Comp_ITypeMem(MIPSOpcode op)
	{
		CONDITIONAL_DISABLE;
		int offset = (signed short)(op&0xFFFF);
		bool load = false;
		MIPSGPReg rt = _RT;
		MIPSGPReg rs = _RS;
		int o = op>>26;
		if (((op >> 29) & 1) == 0 && rt == MIPS_REG_ZERO) {
			// Don't load anything into $zr
			return;
		}

		u32 iaddr = gpr.IsImm(rs) ? offset + gpr.GetImm(rs) : 0xFFFFFFFF;
		bool doCheck = false;
		ARMReg addrReg = R0;

		switch (o) {
		case 32: //lb
		case 33: //lh
		case 35: //lw
		case 36: //lbu
		case 37: //lhu
			load = true;
		case 40: //sb
		case 41: //sh
		case 43: //sw
			// Map base register as pointer and go from there - if the displacement isn't too big.
			// This is faster if there are multiple loads from the same pointer. Need to hook up the MIPS analyzer..
			if (jo.cachePointers && g_Config.bFastMemory) {
				// ARM has smaller load/store immediate displacements than MIPS, 12 bits - and some memory ops only have 8 bits.
				int offsetRange = 0x3ff;
				if (o == 41 || o == 33 || o == 37 || o == 32)
					offsetRange = 0xff;  // 8 bit offset only
				if (!gpr.IsImm(rs) && rs != rt && (offset <= offsetRange) && offset >= -offsetRange) {
					gpr.SpillLock(rs, rt);
					gpr.MapRegAsPointer(rs);
					gpr.MapReg(rt, load ? (MAP_NOINIT | MAP_DIRTY) : 0);
					switch (o) {
					case 35: LDR  (gpr.R(rt), gpr.RPtr(rs), Operand2(offset, TYPE_IMM)); break;
					case 37: LDRH (gpr.R(rt), gpr.RPtr(rs), Operand2(offset, TYPE_IMM)); break;
					case 33: LDRSH(gpr.R(rt), gpr.RPtr(rs), Operand2(offset, TYPE_IMM)); break;
					case 36: LDRB (gpr.R(rt), gpr.RPtr(rs), Operand2(offset, TYPE_IMM)); break;
					case 32: LDRSB(gpr.R(rt), gpr.RPtr(rs), Operand2(offset, TYPE_IMM)); break;
						// Store
					case 43: STR  (gpr.R(rt), gpr.RPtr(rs), Operand2(offset, TYPE_IMM)); break;
					case 41: STRH (gpr.R(rt), gpr.RPtr(rs), Operand2(offset, TYPE_IMM)); break;
					case 40: STRB (gpr.R(rt), gpr.RPtr(rs), Operand2(offset, TYPE_IMM)); break;
					}
					gpr.ReleaseSpillLocks();
					break;
				}
			}

			if (gpr.IsImm(rs) && Memory::IsValidAddress(iaddr)) {
				// TODO: Avoid mapping a register for the "zero" register, use R0 instead.

				// We can compute the full address at compile time. Kickass.
				u32 addr = iaddr & 0x3FFFFFFF;
				// Still flush it, since often these will be in a row.
				load ? gpr.MapDirtyIn(rt, rs) : gpr.MapInIn(rt, rs);
				if (addr == iaddr && offset == 0) {
					// It was already safe.  Let's shove it into a reg and use it directly.
					addrReg = gpr.R(rs);
				} else {
					gpr.SetRegImm(R0, addr);
					addrReg = R0;
				}
			} else {
				_dbg_assert_msg_(JIT, !gpr.IsImm(rs), "Invalid immediate address?  CPU bug?");
				load ? gpr.MapDirtyIn(rt, rs) : gpr.MapInIn(rt, rs);

				if (!g_Config.bFastMemory && rs != MIPS_REG_SP) {
					SetCCAndR0ForSafeAddress(rs, offset, R1);
					doCheck = true;
				} else {
					SetR0ToEffectiveAddress(rs, offset);
				}
				addrReg = R0;
			}

			switch (o)
			{
			// Load
			case 35: LDR  (gpr.R(rt), MEMBASEREG, addrReg); break;
			case 37: LDRH (gpr.R(rt), MEMBASEREG, addrReg); break;
			case 33: LDRSH(gpr.R(rt), MEMBASEREG, addrReg); break;
			case 36: LDRB (gpr.R(rt), MEMBASEREG, addrReg); break;
			case 32: LDRSB(gpr.R(rt), MEMBASEREG, addrReg); break;
			// Store
			case 43: STR  (gpr.R(rt), MEMBASEREG, addrReg); break;
			case 41: STRH (gpr.R(rt), MEMBASEREG, addrReg); break;
			case 40: STRB (gpr.R(rt), MEMBASEREG, addrReg); break;
			}
			if (doCheck) {
				if (load) {
					SetCC(CC_EQ);
					MOVI2R(gpr.R(rt), 0);
				}
				SetCC(CC_AL);
			}
			break;
		case 34: //lwl
		case 38: //lwr
			load = true;
		case 42: //swl
		case 46: //swr
			Comp_ITypeMemLR(op, load);
			break;
		default:
			Comp_Generic(op);
			return;
		}
	}

	void Jit::Comp_Cache(MIPSOpcode op) {
		CONDITIONAL_DISABLE;
		// TODO: Could use this as a hint, and technically required to handle icache, etc.
		// But right now Int_Cache does nothing, so let's not even call it.
	}
}
