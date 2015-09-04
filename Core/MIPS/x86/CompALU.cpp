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

#include "Core/MIPS/MIPSCodeUtils.h"
#include "Core/MIPS/x86/Jit.h"
#include "Core/MIPS/x86/RegCache.h"
#include <algorithm>

using namespace MIPSAnalyst;

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

//#define CONDITIONAL_DISABLE { Comp_Generic(op); return; }
#define CONDITIONAL_DISABLE ;
#define DISABLE { Comp_Generic(op); return; }

namespace MIPSComp
{
	void Jit::CompImmLogic(MIPSOpcode op, void (XEmitter::*arith)(int, const OpArg &, const OpArg &))
	{
		u32 uimm = (u16)(op & 0xFFFF);
		MIPSGPReg rt = _RT;
		MIPSGPReg rs = _RS;
		gpr.Lock(rt, rs);
		gpr.MapReg(rt, rt == rs, true);
		if (rt != rs)
			MOV(32, gpr.R(rt), gpr.R(rs));
		(this->*arith)(32, gpr.R(rt), Imm32(uimm));
		gpr.UnlockAll();
	}

	void Jit::Comp_IType(MIPSOpcode op)
	{
		CONDITIONAL_DISABLE;
		s32 simm = (s32)_IMM16;  // sign extension
		u32 uimm = op & 0xFFFF;
		u32 suimm = (u32)(s32)simm;

		MIPSGPReg rt = _RT;
		MIPSGPReg rs = _RS;

		// noop, won't write to ZERO.
		if (rt == MIPS_REG_ZERO)
			return;

		switch (op >> 26) 
		{
		case 8:	// same as addiu?
		case 9:	// R(rt) = R(rs) + simm; break; //addiu
			{
				if (gpr.IsImm(rs))
				{
					gpr.SetImm(rt, gpr.GetImm(rs) + simm);
					break;
				}

				gpr.Lock(rt, rs);
				gpr.MapReg(rt, rt == rs, true);
				if (rt == rs || gpr.R(rs).IsSimpleReg())
					LEA(32, gpr.RX(rt), MDisp(gpr.RX(rs), simm));
				else
				{
					MOV(32, gpr.R(rt), gpr.R(rs));
					if (simm != 0)
						ADD(32, gpr.R(rt), Imm32((u32)(s32)simm));
				}
				gpr.UnlockAll();
			}
			break;

		case 10: // R(rt) = (s32)R(rs) < simm; break; //slti
			// There's a mips compiler out there asking questions it already knows the answer to...
			if (gpr.IsImm(rs))
			{
				gpr.SetImm(rt, (s32)gpr.GetImm(rs) < simm);
				break;
			}

			gpr.Lock(rt, rs);
			gpr.MapReg(rs, true, false);
			gpr.MapReg(rt, rt == rs, true);
			XOR(32, R(EAX), R(EAX));
			CMP(32, gpr.R(rs), Imm32(simm));
			SETcc(CC_L, R(EAX));
			MOV(32, gpr.R(rt), R(EAX));
			gpr.UnlockAll();
			break;

		case 11: // R(rt) = R(rs) < uimm; break; //sltiu
			if (gpr.IsImm(rs))
			{
				gpr.SetImm(rt, gpr.GetImm(rs) < uimm);
				break;
			}

			gpr.Lock(rt, rs);
			gpr.MapReg(rs, true, false);
			gpr.MapReg(rt, rt == rs, true);
			XOR(32, R(EAX), R(EAX));
			CMP(32, gpr.R(rs), Imm32((u32)simm));
			SETcc(CC_B, R(EAX));
			MOV(32, gpr.R(rt), R(EAX));
			gpr.UnlockAll();
			break;

		case 12: // R(rt) = R(rs) & uimm; break; //andi
			if (uimm == 0)
				gpr.SetImm(rt, 0);
			else if (gpr.IsImm(rs))
				gpr.SetImm(rt, gpr.GetImm(rs) & uimm);
			else
				CompImmLogic(op, &XEmitter::AND);
			break;

		case 13: // R(rt) = R(rs) | uimm; break; //ori
			if (gpr.IsImm(rs))
				gpr.SetImm(rt, gpr.GetImm(rs) | uimm);
			else
				CompImmLogic(op, &XEmitter::OR);
			break;

		case 14: // R(rt) = R(rs) ^ uimm; break; //xori
			if (gpr.IsImm(rs))
				gpr.SetImm(rt, gpr.GetImm(rs) ^ uimm);
			else
				CompImmLogic(op, &XEmitter::XOR);
			break;

		case 15: //R(rt) = uimm << 16;	 break; //lui
			gpr.SetImm(rt, uimm << 16);
			break;

		default:
			Comp_Generic(op);
			break;
		}
	}

	void Jit::Comp_RType2(MIPSOpcode op)
	{
		CONDITIONAL_DISABLE;
		MIPSGPReg rs = _RS;
		MIPSGPReg rd = _RD;

		// Don't change $zr.
		if (rd == MIPS_REG_ZERO)
			return;

		switch (op & 63)
		{
		case 22: //clz
			if (gpr.IsImm(rs))
			{
				u32 value = gpr.GetImm(rs);
				int x = 31;
				int count = 0;
				while (!(value & (1 << x)) && x >= 0)
				{
					count++;
					x--;
				}
				gpr.SetImm(rd, count);
			}
			else
			{
				gpr.Lock(rd, rs);
				gpr.MapReg(rd, rd == rs, true);
				BSR(32, EAX, gpr.R(rs));
				FixupBranch notFound = J_CC(CC_Z);

				MOV(32, gpr.R(rd), Imm32(31));
				SUB(32, gpr.R(rd), R(EAX));
				FixupBranch skip = J();

				SetJumpTarget(notFound);
				MOV(32, gpr.R(rd), Imm32(32));

				SetJumpTarget(skip);
				gpr.UnlockAll();
			}
			break;
		case 23: //clo
			if (gpr.IsImm(rs))
			{
				u32 value = gpr.GetImm(rs);
				int x = 31;
				int count = 0;
				while ((value & (1 << x)) && x >= 0)
				{
					count++;
					x--;
				}
				gpr.SetImm(rd, count);
			}
			else
			{
				gpr.Lock(rd, rs);
				gpr.MapReg(rd, rd == rs, true);
				MOV(32, R(EAX), gpr.R(rs));
				NOT(32, R(EAX));
				BSR(32, EAX, R(EAX));
				FixupBranch notFound = J_CC(CC_Z);

				MOV(32, gpr.R(rd), Imm32(31));
				SUB(32, gpr.R(rd), R(EAX));
				FixupBranch skip = J();

				SetJumpTarget(notFound);
				MOV(32, gpr.R(rd), Imm32(32));

				SetJumpTarget(skip);
				gpr.UnlockAll();
			}
			break;
		default:
			DISABLE;
		}
	}

	static u32 RType3_ImmAdd(const u32 a, const u32 b)
	{
		return a + b;
	}

	static u32 RType3_ImmSub(const u32 a, const u32 b)
	{
		return a - b;
	}

	static u32 RType3_ImmAnd(const u32 a, const u32 b)
	{
		return a & b;
	}

	static u32 RType3_ImmOr(const u32 a, const u32 b)
	{
		return a | b;
	}

	static u32 RType3_ImmXor(const u32 a, const u32 b)
	{
		return a ^ b;
	}

	//rd = rs X rt
	void Jit::CompTriArith(MIPSOpcode op, void (XEmitter::*arith)(int, const OpArg &, const OpArg &), u32 (*doImm)(const u32, const u32))
	{
		MIPSGPReg rt = _RT;
		MIPSGPReg rs = _RS;
		MIPSGPReg rd = _RD;

		// Yes, this happens.  Let's make it fast.
		if (doImm && gpr.IsImm(rs) && gpr.IsImm(rt))
		{
			gpr.SetImm(rd, doImm(gpr.GetImm(rs), gpr.GetImm(rt)));
			return;
		}

		// Act like zero was used if the operand is equivalent.  This happens.
		if (gpr.IsImm(rs) && gpr.GetImm(rs) == 0)
			rs = MIPS_REG_ZERO;
		if (gpr.IsImm(rt) && gpr.GetImm(rt) == 0)
			rt = MIPS_REG_ZERO;

		gpr.Lock(rt, rs, rd);
		// Optimize out operations against 0... and is the only one that isn't a MOV.
		if (rt == MIPS_REG_ZERO || (rs == MIPS_REG_ZERO && doImm != &RType3_ImmSub))
		{
			if (doImm == &RType3_ImmAnd)
				gpr.SetImm(rd, 0);
			else
			{
				MIPSGPReg rsource = rt == MIPS_REG_ZERO ? rs : rt;
				if (rsource != rd)
				{
					gpr.MapReg(rd, false, true);
					MOV(32, gpr.R(rd), gpr.R(rsource));
				}
			}
		}
		else if (gpr.IsImm(rt))
		{
			// No temporary needed.
			u32 rtval = gpr.GetImm(rt);
			gpr.MapReg(rd, rs == rd, true);
			if (rs != rd)
				MOV(32, gpr.R(rd), gpr.R(rs));
			(this->*arith)(32, gpr.R(rd), Imm32(rtval));
		}
		else
		{
			// Use EAX as a temporary if we'd overwrite it.
			if (rd == rt)
				MOV(32, R(EAX), gpr.R(rt));
			gpr.MapReg(rd, rs == rd, true);
			if (rs != rd)
				MOV(32, gpr.R(rd), gpr.R(rs));
			(this->*arith)(32, gpr.R(rd), rd == rt ? R(EAX) : gpr.R(rt));
		}
		gpr.UnlockAll();
	}

	void Jit::Comp_RType3(MIPSOpcode op)
	{
		CONDITIONAL_DISABLE
		
		MIPSGPReg rt = _RT;
		MIPSGPReg rs = _RS;
		MIPSGPReg rd = _RD;

		// noop, won't write to ZERO.
		if (rd == MIPS_REG_ZERO)
			return;

		switch (op & 63)
		{
		case 10: //if (R(rt) == 0) R(rd) = R(rs); break; //movz
			if (rd == rs)
				break;
			gpr.Lock(rt, rs, rd);
			if (!gpr.IsImm(rt))
			{
				gpr.KillImmediate(rs, true, false);
				// Need to load rd in case the condition fails.
				gpr.MapReg(rd, true, true);
				CMP(32, gpr.R(rt), Imm32(0));
				CMOVcc(32, gpr.RX(rd), gpr.R(rs), CC_E);
			}
			else if (gpr.GetImm(rt) == 0)
			{
				// Yes, this actually happens.
				if (gpr.IsImm(rs))
					gpr.SetImm(rd, gpr.GetImm(rs));
				else if (rd != rs)
				{
					gpr.MapReg(rd, false, true);
					MOV(32, gpr.R(rd), gpr.R(rs));
				}
			}
			gpr.UnlockAll();
			break;

		case 11: //if (R(rt) != 0) R(rd) = R(rs); break; //movn
			if (rd == rs)
				break;
			gpr.Lock(rt, rs, rd);
			if (!gpr.IsImm(rt))
			{
				gpr.KillImmediate(rs, true, false);
				// Need to load rd in case the condition fails.
				gpr.MapReg(rd, true, true);
				CMP(32, gpr.R(rt), Imm32(0));
				CMOVcc(32, gpr.RX(rd), gpr.R(rs), CC_NE);
			}
			else if (gpr.GetImm(rt) != 0)
			{
				if (gpr.IsImm(rs))
					gpr.SetImm(rd, gpr.GetImm(rs));
				else if (rd != rs)
				{
					gpr.MapReg(rd, false, true);
					MOV(32, gpr.R(rd), gpr.R(rs));
				}
			}
			gpr.UnlockAll();
			break;

		case 32: //R(rd) = R(rs) + R(rt);		break; //add
		case 33: //R(rd) = R(rs) + R(rt);		break; //addu
			CompTriArith(op, &XEmitter::ADD, &RType3_ImmAdd);
			break;
		case 34: //R(rd) = R(rs) - R(rt);		break; //sub
		case 35: //R(rd) = R(rs) - R(rt);		break; //subu
			CompTriArith(op, &XEmitter::SUB, &RType3_ImmSub);
			break;
		case 36: //R(rd) = R(rs) & R(rt);		break; //and
			CompTriArith(op, &XEmitter::AND, &RType3_ImmAnd);
			break;
		case 37: //R(rd) = R(rs) | R(rt);		break; //or
			CompTriArith(op, &XEmitter::OR, &RType3_ImmOr);
			break;
		case 38: //R(rd) = R(rs) ^ R(rt);		break; //xor
			CompTriArith(op, &XEmitter::XOR, &RType3_ImmXor);
			break;

		case 39: // R(rd) = ~(R(rs) | R(rt)); //nor
			CompTriArith(op, &XEmitter::OR, &RType3_ImmOr);
			if (gpr.IsImm(rd))
				gpr.SetImm(rd, ~gpr.GetImm(rd));
			else
				NOT(32, gpr.R(rd));
			break;

		case 42: //R(rd) = (int)R(rs) < (int)R(rt); break; //slt
			if (gpr.IsImm(rs) && gpr.IsImm(rt))
				gpr.SetImm(rd, (s32)gpr.GetImm(rs) < (s32)gpr.GetImm(rt));
			else
			{
				gpr.Lock(rt, rs, rd);
				gpr.MapReg(rs, true, false);
				gpr.MapReg(rd, rd == rt, true);
				XOR(32, R(EAX), R(EAX));
				CMP(32, gpr.R(rs), gpr.R(rt));
				SETcc(CC_L, R(EAX));
				MOV(32, gpr.R(rd), R(EAX));
				gpr.UnlockAll();
			}
			break;

		case 43: //R(rd) = R(rs) < R(rt);		break; //sltu
			if (gpr.IsImm(rs) && gpr.IsImm(rt))
				gpr.SetImm(rd, gpr.GetImm(rs) < gpr.GetImm(rt));
			else
			{
				gpr.Lock(rd, rs, rt);
				gpr.MapReg(rs, true, false);
				gpr.MapReg(rd, rd == rt, true);
				XOR(32, R(EAX), R(EAX));
				CMP(32, gpr.R(rs), gpr.R(rt));
				SETcc(CC_B, R(EAX));
				MOV(32, gpr.R(rd), R(EAX));
				gpr.UnlockAll();
			}
			break;

		case 44: //R(rd) = (R(rs) > R(rt)) ? R(rs) : R(rt); break; //max
			if (gpr.IsImm(rs) && gpr.IsImm(rt))
				gpr.SetImm(rd, std::max((s32)gpr.GetImm(rs), (s32)gpr.GetImm(rt)));
			else
			{
				MIPSGPReg rsrc = rd == rt ? rs : rt;
				gpr.Lock(rd, rs, rt);
				gpr.KillImmediate(rsrc, true, false);
				gpr.MapReg(rd, rd == rs || rd == rt, true);
				if (rd != rt && rd != rs)
					MOV(32, gpr.R(rd), gpr.R(rs));
				CMP(32, gpr.R(rd), gpr.R(rsrc));
				CMOVcc(32, gpr.RX(rd), gpr.R(rsrc), CC_L);
				gpr.UnlockAll();
			}
			break;

		case 45: //R(rd) = (R(rs) < R(rt)) ? R(rs) : R(rt); break; //min
			if (gpr.IsImm(rs) && gpr.IsImm(rt))
				gpr.SetImm(rd, std::min((s32)gpr.GetImm(rs), (s32)gpr.GetImm(rt)));
			else
			{
				MIPSGPReg rsrc = rd == rt ? rs : rt;
				gpr.Lock(rd, rs, rt);
				gpr.KillImmediate(rsrc, true, false);
				gpr.MapReg(rd, rd == rs || rd == rt, true);
				if (rd != rt && rd != rs)
					MOV(32, gpr.R(rd), gpr.R(rs));
				CMP(32, gpr.R(rd), gpr.R(rsrc));
				CMOVcc(32, gpr.RX(rd), gpr.R(rsrc), CC_G);
				gpr.UnlockAll();
			}
			break;

		default:
			Comp_Generic(op);
			break;
		}
	}

	static u32 ShiftType_ImmLogicalLeft(const u32 a, const u32 b)
	{
		return a << (b & 0x1f);
	}

	static u32 ShiftType_ImmLogicalRight(const u32 a, const u32 b)
	{
		return a >> (b & 0x1f);
	}

	static u32 ShiftType_ImmArithRight(const u32 a, const u32 b)
	{
		return ((s32) a) >> (b & 0x1f);
	}

	static u32 ShiftType_ImmRotateRight(const u32 a, const u32 b)
	{
		const s8 sa = b & 0x1f;
		return (a >> sa) | (a << (32 - sa));
	}

	void Jit::CompShiftImm(MIPSOpcode op, void (XEmitter::*shift)(int, OpArg, OpArg), u32 (*doImm)(const u32, const u32))
	{
		MIPSGPReg rd = _RD;
		MIPSGPReg rt = _RT;
		int sa = _SA;

		if (doImm && gpr.IsImm(rt))
		{
			gpr.SetImm(rd, doImm(gpr.GetImm(rt), sa));
			return;
		}

		gpr.Lock(rd, rt);
		gpr.MapReg(rd, rd == rt, true);
		if (rd != rt)
			MOV(32, gpr.R(rd), gpr.R(rt));
		(this->*shift)(32, gpr.R(rd), Imm8(sa));
		gpr.UnlockAll();
	}

	// "over-shifts" work the same as on x86 - only bottom 5 bits are used to get the shift value
	void Jit::CompShiftVar(MIPSOpcode op, void (XEmitter::*shift)(int, OpArg, OpArg), u32 (*doImm)(const u32, const u32))
	{
		MIPSGPReg rd = _RD;
		MIPSGPReg rt = _RT;
		MIPSGPReg rs = _RS;

		if (doImm && gpr.IsImm(rs) && gpr.IsImm(rt))
		{
			gpr.SetImm(rd, doImm(gpr.GetImm(rt), gpr.GetImm(rs)));
			return;
		}

		gpr.Lock(rd, rt, rs);
		if (gpr.IsImm(rs))
		{
			int sa = gpr.GetImm(rs);
			gpr.MapReg(rd, rd == rt, true);
			if (rd != rt)
				MOV(32, gpr.R(rd), gpr.R(rt));
			(this->*shift)(32, gpr.R(rd), Imm8(sa));
		}
		else
		{
			gpr.FlushLockX(ECX);
			gpr.MapReg(rd, rd == rt || rd == rs, true);
			MOV(32, R(ECX), gpr.R(rs));	// Only ECX can be used for variable shifts.
			AND(32, R(ECX), Imm32(0x1f));
			if (rd != rt)
				MOV(32, gpr.R(rd), gpr.R(rt));
			(this->*shift)(32, gpr.R(rd), R(ECX));
			gpr.UnlockAllX();
		}
		gpr.UnlockAll();
	}

	void Jit::Comp_ShiftType(MIPSOpcode op)
	{
		CONDITIONAL_DISABLE;
		int rs = (op>>21) & 0x1F;
		MIPSGPReg rd = _RD;
		int fd = (op>>6) & 0x1F;

		// noop, won't write to ZERO.
		if (rd == MIPS_REG_ZERO)
			return;

		// WARNING : ROTR
		switch (op & 0x3f)
		{
		case 0: CompShiftImm(op, &XEmitter::SHL, &ShiftType_ImmLogicalLeft); break;
		case 2: CompShiftImm(op, rs == 1 ? &XEmitter::ROR : &XEmitter::SHR, rs == 1 ? &ShiftType_ImmRotateRight : &ShiftType_ImmLogicalRight); break;	// srl, rotr
		case 3: CompShiftImm(op, &XEmitter::SAR, &ShiftType_ImmArithRight); break;	// sra

		case 4: CompShiftVar(op, &XEmitter::SHL, &ShiftType_ImmLogicalLeft); break; //sllv
		case 6: CompShiftVar(op, fd == 1 ? &XEmitter::ROR : &XEmitter::SHR, fd == 1 ? &ShiftType_ImmRotateRight : &ShiftType_ImmLogicalRight); break;	//srlv
		case 7: CompShiftVar(op, &XEmitter::SAR, &ShiftType_ImmArithRight); break; //srav

		default:
			Comp_Generic(op);
			break;
		}
	}

	void Jit::Comp_Special3(MIPSOpcode op)
	{
		CONDITIONAL_DISABLE;
		MIPSGPReg rs = _RS;
		MIPSGPReg rt = _RT;

		int pos = _POS;
		int size = _SIZE + 1;
		u32 mask = 0xFFFFFFFFUL >> (32 - size);

		// Don't change $zr.
		if (rt == MIPS_REG_ZERO)
			return;

		switch (op & 0x3f)
		{
		case 0x0: //ext
			if (gpr.IsImm(rs))
			{
				gpr.SetImm(rt, (gpr.GetImm(rs) >> pos) & mask);
				return;
			}

			gpr.Lock(rs, rt);
			gpr.MapReg(rt, rs == rt, true);
			if (rs != rt)
				MOV(32, gpr.R(rt), gpr.R(rs));
			SHR(32, gpr.R(rt), Imm8(pos));
			AND(32, gpr.R(rt), Imm32(mask));
			gpr.UnlockAll();
			break;

		case 0x4: //ins
			{
				u32 sourcemask = mask >> pos;
				u32 destmask = ~(sourcemask << pos);
				if (gpr.IsImm(rs))
				{
					u32 inserted = (gpr.GetImm(rs) & sourcemask) << pos;
					if (gpr.IsImm(rt))
					{
						gpr.SetImm(rt, (gpr.GetImm(rt) & destmask) | inserted);
						return;
					}

					gpr.Lock(rs, rt);
					gpr.MapReg(rt, true, true);
					AND(32, gpr.R(rt), Imm32(destmask));
					if (inserted != 0)
						OR(32, gpr.R(rt), Imm32(inserted));
					gpr.UnlockAll();
				}
				else
				{
					gpr.Lock(rs, rt);
					gpr.MapReg(rt, true, true);
					MOV(32, R(EAX), gpr.R(rs));
					AND(32, R(EAX), Imm32(sourcemask));
					SHL(32, R(EAX), Imm8(pos));
					AND(32, gpr.R(rt), Imm32(destmask));
					OR(32, gpr.R(rt), R(EAX));
					gpr.UnlockAll();
				}
			}
			break;
		}
	}


	void Jit::Comp_Allegrex(MIPSOpcode op)
	{
		CONDITIONAL_DISABLE
		MIPSGPReg rt = _RT;
		MIPSGPReg rd = _RD;
		// Don't change $zr.
		if (rd == MIPS_REG_ZERO)
			return;

		switch ((op >> 6) & 31)
		{
		case 16: // seb  // R(rd) = (u32)(s32)(s8)(u8)R(rt);
			if (gpr.IsImm(rt))
			{
				gpr.SetImm(rd, (u32)(s32)(s8)(u8)gpr.GetImm(rt));
				break;
			}

			gpr.Lock(rd, rt);
			gpr.MapReg(rd, rd == rt, true);
#ifdef _M_IX86
			// work around the byte-register addressing problem
			if (!gpr.R(rt).IsSimpleReg(EDX) && !gpr.R(rt).IsSimpleReg(ECX))
			{
				MOV(32, R(EAX), gpr.R(rt));
				MOVSX(32, 8, gpr.RX(rd), R(EAX));
			}
			else
#endif
			{
				gpr.KillImmediate(rt, true, false);
				MOVSX(32, 8, gpr.RX(rd), gpr.R(rt));
			}
			gpr.UnlockAll();
			break;

		case 20: //bitrev
			if (gpr.IsImm(rt))
			{
				// http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel
				u32 v = gpr.GetImm(rt);
				// swap odd and even bits
				v = ((v >> 1) & 0x55555555) | ((v & 0x55555555) << 1);
				// swap consecutive pairs
				v = ((v >> 2) & 0x33333333) | ((v & 0x33333333) << 2);
				// swap nibbles ...
				v = ((v >> 4) & 0x0F0F0F0F) | ((v & 0x0F0F0F0F) << 4);
				// swap bytes
				v = ((v >> 8) & 0x00FF00FF) | ((v & 0x00FF00FF) << 8);
				// swap 2-byte long pairs
				v = ( v >> 16             ) | ( v               << 16);
				gpr.SetImm(rd, v);
				break;
			}

			gpr.Lock(rd, rt);
			gpr.MapReg(rd, rd == rt, true);
			if (rd != rt)
				MOV(32, gpr.R(rd), gpr.R(rt));

			LEA(32, EAX, MScaled(gpr.RX(rd), 2, 0));
			SHR(32, gpr.R(rd), Imm8(1));
			XOR(32, gpr.R(rd), R(EAX));
			AND(32, gpr.R(rd), Imm32(0x55555555));
			XOR(32, gpr.R(rd), R(EAX));

			LEA(32, EAX, MScaled(gpr.RX(rd), 4, 0));
			SHR(32, gpr.R(rd), Imm8(2));
			XOR(32, gpr.R(rd), R(EAX));
			AND(32, gpr.R(rd), Imm32(0x33333333));
			XOR(32, gpr.R(rd), R(EAX));

			MOV(32, R(EAX), gpr.R(rd));
			SHL(32, R(EAX), Imm8(4));
			SHR(32, gpr.R(rd), Imm8(4));
			XOR(32, gpr.R(rd), R(EAX));
			AND(32, gpr.R(rd), Imm32(0x0F0F0F0F));
			XOR(32, gpr.R(rd), R(EAX));

			MOV(32, R(EAX), gpr.R(rd));
			SHL(32, R(EAX), Imm8(8));
			SHR(32, gpr.R(rd), Imm8(8));
			XOR(32, gpr.R(rd), R(EAX));
			AND(32, gpr.R(rd), Imm32(0x00FF00FF));
			XOR(32, gpr.R(rd), R(EAX));

			ROL(32, gpr.R(rd), Imm8(16));

			gpr.UnlockAll();
			break;

		case 24: // seh  // R(rd) = (u32)(s32)(s16)(u16)R(rt);
			if (gpr.IsImm(rt))
			{
				gpr.SetImm(rd, (u32)(s32)(s16)(u16)gpr.GetImm(rt));
				break;
			}

			gpr.Lock(rd, rt);
			gpr.MapReg(rd, rd == rt, true);
			MOVSX(32, 16, gpr.RX(rd), gpr.R(rt));
			gpr.UnlockAll();
			break;

		default:
			Comp_Generic(op);
			return;
		}
	}

	void Jit::Comp_Allegrex2(MIPSOpcode op)
	{
		CONDITIONAL_DISABLE
		MIPSGPReg rt = _RT;
		MIPSGPReg rd = _RD;
		// Don't change $zr.
		if (rd == MIPS_REG_ZERO)
			return;

		switch (op & 0x3ff)
		{
		case 0xA0: //wsbh
			if (gpr.IsImm(rt)) {
				u32 rtImm = gpr.GetImm(rt);
				gpr.SetImm(rd, ((rtImm & 0xFF00FF00) >> 8) | ((rtImm & 0x00FF00FF) << 8));
				break;
			}
			gpr.Lock(rd, rt);
			gpr.MapReg(rd, rd == rt, true);
			if (rd != rt)
				MOV(32, gpr.R(rd), gpr.R(rt));
			// Swap both 16-bit halfwords by rotating afterward.
			BSWAP(32, gpr.RX(rd));
			ROR(32, gpr.R(rd), Imm8(16));
			gpr.UnlockAll();
			break;
		case 0xE0: //wsbw
			if (gpr.IsImm(rt)) {
				gpr.SetImm(rd, swap32(gpr.GetImm(rt)));
				break;
			}
			gpr.Lock(rd, rt);
			gpr.MapReg(rd, rd == rt, true);
			if (rd != rt)
				MOV(32, gpr.R(rd), gpr.R(rt));
			BSWAP(32, gpr.RX(rd));
			gpr.UnlockAll();
			break;
		default:
			Comp_Generic(op);
			break;
		}
	}

	void Jit::Comp_MulDivType(MIPSOpcode op)
	{
		CONDITIONAL_DISABLE;
		MIPSGPReg rt = _RT;
		MIPSGPReg rs = _RS;
		MIPSGPReg rd = _RD;

		switch (op & 63) 
		{
		case 16: // R(rd) = HI; //mfhi
			gpr.MapReg(rd, false, true);
			MOV(32, gpr.R(rd), M((void *)&mips_->hi));
			break; 

		case 17: // HI = R(rs); //mthi
			gpr.MapReg(rs, true, false);
			MOV(32, M((void *)&mips_->hi), gpr.R(rs));
			break; 

		case 18: // R(rd) = LO; break; //mflo
			gpr.MapReg(rd, false, true);
			MOV(32, gpr.R(rd), M((void *)&mips_->lo));
			break;

		case 19: // LO = R(rs); break; //mtlo
			gpr.MapReg(rs, true, false);
			MOV(32, M((void *)&mips_->lo), gpr.R(rs));
			break; 

		case 24: //mult (the most popular one). lo,hi  = signed mul (rs * rt)
			gpr.FlushLockX(EDX);
			gpr.KillImmediate(rt, true, false);
			MOV(32, R(EAX), gpr.R(rs));
			IMUL(32, gpr.R(rt));
			MOV(32, M((void *)&mips_->hi), R(EDX));
			MOV(32, M((void *)&mips_->lo), R(EAX));
			gpr.UnlockAllX();
			break;


		case 25: //multu (2nd) lo,hi  = unsigned mul (rs * rt)
			gpr.FlushLockX(EDX);
			gpr.KillImmediate(rt, true, false);
			MOV(32, R(EAX), gpr.R(rs));
			MUL(32, gpr.R(rt));
			MOV(32, M((void *)&mips_->hi), R(EDX));
			MOV(32, M((void *)&mips_->lo), R(EAX));
			gpr.UnlockAllX();
			break;

		case 26: //div
			{
				gpr.FlushLockX(EDX);
				// For CMP.
				gpr.KillImmediate(rs, true, false);
				gpr.KillImmediate(rt, true, false);
				CMP(32, gpr.R(rt), Imm32(0));
				FixupBranch divZero = J_CC(CC_E);

				// INT_MAX / -1 would overflow.
				CMP(32, gpr.R(rs), Imm32(0x80000000));
				FixupBranch notOverflow = J_CC(CC_NE);
				CMP(32, gpr.R(rt), Imm32((u32) -1));
				FixupBranch notOverflow2 = J_CC(CC_NE);
				// TODO: Should HI be set to anything?
				MOV(32, M((void *)&mips_->lo), Imm32(0x80000000));
				FixupBranch skip2 = J();

				SetJumpTarget(notOverflow);
				SetJumpTarget(notOverflow2);

				MOV(32, R(EAX), gpr.R(rs));
				CDQ();
				IDIV(32, gpr.R(rt));
				MOV(32, M((void *)&mips_->hi), R(EDX));
				MOV(32, M((void *)&mips_->lo), R(EAX));
				FixupBranch skip = J();

				SetJumpTarget(divZero);
				// TODO: Is this the right way to handle a divide by zero?
				MOV(32, M((void *)&mips_->hi), Imm32(0));
				MOV(32, M((void *)&mips_->lo), Imm32(0));

				SetJumpTarget(skip);
				SetJumpTarget(skip2);
				gpr.UnlockAllX();
			}
			break;

		case 27: //divu
			{
				gpr.FlushLockX(EDX);
				gpr.KillImmediate(rt, true, false);
				CMP(32, gpr.R(rt), Imm32(0));
				FixupBranch divZero = J_CC(CC_E);

				MOV(32, R(EAX), gpr.R(rs));
				MOV(32, R(EDX), Imm32(0));
				DIV(32, gpr.R(rt));
				MOV(32, M((void *)&mips_->hi), R(EDX));
				MOV(32, M((void *)&mips_->lo), R(EAX));
				FixupBranch skip = J();

				SetJumpTarget(divZero);
				// TODO: Is this the right way to handle a divide by zero?
				MOV(32, M((void *)&mips_->hi), Imm32(0));
				MOV(32, M((void *)&mips_->lo), Imm32(0));

				SetJumpTarget(skip);
				gpr.UnlockAllX();
			}
			break;

		case 28: // madd
			gpr.FlushLockX(EDX);
			gpr.KillImmediate(rt, true, false);
			MOV(32, R(EAX), gpr.R(rs));
			IMUL(32, gpr.R(rt));
			ADD(32, M((void *)&mips_->lo), R(EAX));
			ADC(32, M((void *)&mips_->hi), R(EDX));
			gpr.UnlockAllX();
			break;

		case 29: // maddu
			gpr.FlushLockX(EDX);
			gpr.KillImmediate(rt, true, false);
			MOV(32, R(EAX), gpr.R(rs));
			MUL(32, gpr.R(rt));
			ADD(32, M((void *)&mips_->lo), R(EAX));
			ADC(32, M((void *)&mips_->hi), R(EDX));
			gpr.UnlockAllX();
			break;

		case 46: // msub
			gpr.FlushLockX(EDX);
			gpr.KillImmediate(rt, true, false);
			MOV(32, R(EAX), gpr.R(rs));
			IMUL(32, gpr.R(rt));
			SUB(32, M((void *)&mips_->lo), R(EAX));
			SBB(32, M((void *)&mips_->hi), R(EDX));
			gpr.UnlockAllX();
			break;

		case 47: // msubu
			gpr.FlushLockX(EDX);
			gpr.KillImmediate(rt, true, false);
			MOV(32, R(EAX), gpr.R(rs));
			MUL(32, gpr.R(rt));
			SUB(32, M((void *)&mips_->lo), R(EAX));
			SBB(32, M((void *)&mips_->hi), R(EDX));
			gpr.UnlockAllX();
			break;

		default:
			DISABLE;
		}
	}
}
