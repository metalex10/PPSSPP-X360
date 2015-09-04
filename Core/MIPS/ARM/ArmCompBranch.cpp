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

#include "Core/Reporting.h"
#include "Core/Config.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/HLETables.h"

#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSCodeUtils.h"
#include "Core/MIPS/MIPSAnalyst.h"
#include "Core/MIPS/MIPSTables.h"

#include "Core/MIPS/ARM/ArmJit.h"
#include "Core/MIPS/ARM/ArmRegCache.h"
#include "Core/MIPS/JitCommon/JitBlockCache.h"

#include "Common/ArmEmitter.h"

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

#define LOOPOPTIMIZATION 0

// We can disable nice delay slots.
// #define CONDITIONAL_NICE_DELAYSLOT delaySlotIsNice = false;
#define CONDITIONAL_NICE_DELAYSLOT ;

using namespace MIPSAnalyst;

namespace MIPSComp
{

void Jit::BranchRSRTComp(MIPSOpcode op, ArmGen::CCFlags cc, bool likely)
{
	if (js.inDelaySlot) {
		ERROR_LOG_REPORT(JIT, "Branch in RSRTComp delay slot at %08x in block starting at %08x", js.compilerPC, js.blockStart);
		return;
	}
	int offset = _IMM16 << 2;
	MIPSGPReg rt = _RT;
	MIPSGPReg rs = _RS;
	u32 targetAddr = js.compilerPC + offset + 4;

	if (jo.immBranches && gpr.IsImm(rs) && gpr.IsImm(rt) && js.numInstructions < jo.continueMaxInstructions) {
		// The cc flags are opposites: when NOT to take the branch.
		bool skipBranch;
		s32 rsImm = (s32)gpr.GetImm(rs);
		s32 rtImm = (s32)gpr.GetImm(rt);

		switch (cc) {
		case CC_EQ: skipBranch = rsImm == rtImm; break;
		case CC_NEQ: skipBranch = rsImm != rtImm; break;
		default: skipBranch = false; _dbg_assert_msg_(JIT, false, "Bad cc flag in BranchRSRTComp().");
		}

		if (skipBranch) {
			// Skip the delay slot if likely, otherwise it'll be the next instruction.
			if (likely)
				js.compilerPC += 4;
			return;
		}

		// Branch taken.  Always compile the delay slot, and then go to dest.
		CompileDelaySlot(DELAYSLOT_NICE);
		// Account for the increment in the loop.
		js.compilerPC = targetAddr - 4;
		// In case the delay slot was a break or something.
		js.compiling = true;
		return;
	}

	MIPSOpcode delaySlotOp = Memory::Read_Instruction(js.compilerPC+4);
	bool delaySlotIsNice = IsDelaySlotNiceReg(op, delaySlotOp, rt, rs);
	CONDITIONAL_NICE_DELAYSLOT;
	if (!likely && delaySlotIsNice)
		CompileDelaySlot(DELAYSLOT_NICE);

	// We might be able to flip the condition (EQ/NEQ are easy.)
	const bool canFlip = cc == CC_EQ || cc == CC_NEQ;

	Operand2 op2;
	bool negated;
	if (gpr.IsImm(rt) && TryMakeOperand2_AllowNegation(gpr.GetImm(rt), op2, &negated)) {
		gpr.MapReg(rs);
		if (!negated)
			CMP(gpr.R(rs), op2);
		else
			CMN(gpr.R(rs), op2);
	} else {
		if (gpr.IsImm(rs) && TryMakeOperand2_AllowNegation(gpr.GetImm(rs), op2, &negated) && canFlip) {
			gpr.MapReg(rt);
			if (!negated)
				CMP(gpr.R(rt), op2);
			else
				CMN(gpr.R(rt), op2);
		} else {
			gpr.MapInIn(rs, rt);
			CMP(gpr.R(rs), gpr.R(rt));
		}
	}

	ArmGen::FixupBranch ptr;
	if (!likely) {
		if (!delaySlotIsNice)
			CompileDelaySlot(DELAYSLOT_SAFE_FLUSH);
		else
			FlushAll();
		ptr = B_CC(cc);
	} else {
		FlushAll();
		ptr = B_CC(cc);
		CompileDelaySlot(DELAYSLOT_FLUSH);
	}

	// Take the branch
	WriteExit(targetAddr, js.nextExit++);

	SetJumpTarget(ptr);
	// Not taken
	WriteExit(js.compilerPC+8, js.nextExit++);

	js.compiling = false;
}


void Jit::BranchRSZeroComp(MIPSOpcode op, ArmGen::CCFlags cc, bool andLink, bool likely)
{
	if (js.inDelaySlot) {
		ERROR_LOG_REPORT(JIT, "Branch in RSZeroComp delay slot at %08x in block starting at %08x", js.compilerPC, js.blockStart);
		return;
	}
	int offset = _IMM16 << 2;
	MIPSGPReg rs = _RS;
	u32 targetAddr = js.compilerPC + offset + 4;

	if (jo.immBranches && gpr.IsImm(rs) && js.numInstructions < jo.continueMaxInstructions) {
		// The cc flags are opposites: when NOT to take the branch.
		bool skipBranch;
		s32 imm = (s32)gpr.GetImm(rs);

		switch (cc) {
		case CC_GT: skipBranch = imm > 0; break;
		case CC_GE: skipBranch = imm >= 0; break;
		case CC_LT: skipBranch = imm < 0; break;
		case CC_LE: skipBranch = imm <= 0; break;
		default: skipBranch = false; _dbg_assert_msg_(JIT, false, "Bad cc flag in BranchRSZeroComp().");
		}

		if (skipBranch) {
			// Skip the delay slot if likely, otherwise it'll be the next instruction.
			if (likely)
				js.compilerPC += 4;
			return;
		}

		// Branch taken.  Always compile the delay slot, and then go to dest.
		CompileDelaySlot(DELAYSLOT_NICE);
		if (andLink)
			gpr.SetImm(MIPS_REG_RA, js.compilerPC + 8);

		// Account for the increment in the loop.
		js.compilerPC = targetAddr - 4;
		// In case the delay slot was a break or something.
		js.compiling = true;
		return;
	}

	MIPSOpcode delaySlotOp = Memory::Read_Instruction(js.compilerPC + 4);
	bool delaySlotIsNice = IsDelaySlotNiceReg(op, delaySlotOp, rs);
	CONDITIONAL_NICE_DELAYSLOT;
	if (!likely && delaySlotIsNice)
		CompileDelaySlot(DELAYSLOT_NICE);

	gpr.MapReg(rs);
	CMP(gpr.R(rs), Operand2(0, TYPE_IMM));

	ArmGen::FixupBranch ptr;
	if (!likely)
	{
		if (!delaySlotIsNice)
			CompileDelaySlot(DELAYSLOT_SAFE_FLUSH);
		else
			FlushAll();
		ptr = B_CC(cc);
	}
	else
	{
		FlushAll();
		ptr = B_CC(cc);
		CompileDelaySlot(DELAYSLOT_FLUSH);
	}

	// Take the branch
	if (andLink)
	{
		gpr.SetRegImm(R0, js.compilerPC + 8);
		STR(R0, CTXREG, MIPS_REG_RA * 4);
	}

	WriteExit(targetAddr, js.nextExit++);

	SetJumpTarget(ptr);
	// Not taken
	WriteExit(js.compilerPC + 8, js.nextExit++);
	js.compiling = false;
}


void Jit::Comp_RelBranch(MIPSOpcode op)
{
	// The CC flags here should be opposite of the actual branch becuase they skip the branching action.
	switch (op >> 26)
	{
	case 4: BranchRSRTComp(op, CC_NEQ, false); break;//beq
	case 5: BranchRSRTComp(op, CC_EQ,  false); break;//bne

	case 6: BranchRSZeroComp(op, CC_GT, false, false); break;//blez
	case 7: BranchRSZeroComp(op, CC_LE, false, false); break;//bgtz

	case 20: BranchRSRTComp(op, CC_NEQ, true); break;//beql
	case 21: BranchRSRTComp(op, CC_EQ,  true); break;//bnel

	case 22: BranchRSZeroComp(op, CC_GT, false, true); break;//blezl
	case 23: BranchRSZeroComp(op, CC_LE, false, true); break;//bgtzl

	default:
		_dbg_assert_msg_(CPU,0,"Trying to compile instruction that can't be compiled");
		break;
	}
}

void Jit::Comp_RelBranchRI(MIPSOpcode op)
{
	switch ((op >> 16) & 0x1F)
	{
	case 0: BranchRSZeroComp(op, CC_GE, false, false); break; //if ((s32)R(rs) <  0) DelayBranchTo(addr); else PC += 4; break;//bltz
	case 1: BranchRSZeroComp(op, CC_LT, false, false); break; //if ((s32)R(rs) >= 0) DelayBranchTo(addr); else PC += 4; break;//bgez
	case 2: BranchRSZeroComp(op, CC_GE, false, true);  break; //if ((s32)R(rs) <  0) DelayBranchTo(addr); else PC += 8; break;//bltzl
	case 3: BranchRSZeroComp(op, CC_LT, false, true);  break; //if ((s32)R(rs) >= 0) DelayBranchTo(addr); else PC += 8; break;//bgezl
	case 16: BranchRSZeroComp(op, CC_GE, true, false); break;  //R(MIPS_REG_RA) = PC + 8; if ((s32)R(rs) <  0) DelayBranchTo(addr); else PC += 4; break;//bltzal
	case 17: BranchRSZeroComp(op, CC_LT, true, false);  break; //R(MIPS_REG_RA) = PC + 8; if ((s32)R(rs) >= 0) DelayBranchTo(addr); else PC += 4; break;//bgezal
	case 18: BranchRSZeroComp(op, CC_GE, true, true);  break;  //R(MIPS_REG_RA) = PC + 8; if ((s32)R(rs) <  0) DelayBranchTo(addr); else SkipLikely(); break;//bltzall
	case 19: BranchRSZeroComp(op, CC_LT, true, true);   break; //R(MIPS_REG_RA) = PC + 8; if ((s32)R(rs) >= 0) DelayBranchTo(addr); else SkipLikely(); break;//bgezall
	default:
		_dbg_assert_msg_(CPU,0,"Trying to compile instruction that can't be compiled");
		break;
	}
}

// If likely is set, discard the branch slot if NOT taken.
void Jit::BranchFPFlag(MIPSOpcode op, ArmGen::CCFlags cc, bool likely)
{
	if (js.inDelaySlot) {
		ERROR_LOG_REPORT(JIT, "Branch in FPFlag delay slot at %08x in block starting at %08x", js.compilerPC, js.blockStart);
		return;
	}
	int offset = _IMM16 << 2;
	u32 targetAddr = js.compilerPC + offset + 4;

	MIPSOpcode delaySlotOp = Memory::Read_Instruction(js.compilerPC + 4);
	bool delaySlotIsNice = IsDelaySlotNiceFPU(op, delaySlotOp);
	CONDITIONAL_NICE_DELAYSLOT;
	if (!likely && delaySlotIsNice)
		CompileDelaySlot(DELAYSLOT_NICE);

	gpr.MapReg(MIPS_REG_FPCOND);
	TST(gpr.R(MIPS_REG_FPCOND), Operand2(1, TYPE_IMM));

	ArmGen::FixupBranch ptr;
	if (!likely)
	{
		if (!delaySlotIsNice)
			CompileDelaySlot(DELAYSLOT_SAFE_FLUSH);
		else
			FlushAll();
		ptr = B_CC(cc);
	}
	else
	{
		FlushAll();
		ptr = B_CC(cc);
		CompileDelaySlot(DELAYSLOT_FLUSH);
	}

	// Take the branch
	WriteExit(targetAddr, js.nextExit++);

	SetJumpTarget(ptr);
	// Not taken
	WriteExit(js.compilerPC + 8, js.nextExit++);
	js.compiling = false;
}

void Jit::Comp_FPUBranch(MIPSOpcode op)
{
	switch((op >> 16) & 0x1f)
	{
	case 0:	BranchFPFlag(op, CC_NEQ, false); break;  // bc1f
	case 1: BranchFPFlag(op, CC_EQ,  false); break;  // bc1t
	case 2: BranchFPFlag(op, CC_NEQ, true);  break;  // bc1fl
	case 3: BranchFPFlag(op, CC_EQ,  true);  break;  // bc1tl
	default:
		_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");
		break;
	}
}

// If likely is set, discard the branch slot if NOT taken.
void Jit::BranchVFPUFlag(MIPSOpcode op, ArmGen::CCFlags cc, bool likely)
{
	if (js.inDelaySlot) {
		ERROR_LOG_REPORT(JIT, "Branch in VFPU delay slot at %08x in block starting at %08x", js.compilerPC, js.blockStart);
		return;
	}
	int offset = _IMM16 << 2;
	u32 targetAddr = js.compilerPC + offset + 4;

	MIPSOpcode delaySlotOp = Memory::Read_Instruction(js.compilerPC + 4);

	// Sometimes there's a VFPU branch in a delay slot (Disgaea 2: Dark Hero Days, Zettai Hero Project, La Pucelle)
	// The behavior is undefined - the CPU may take the second branch even if the first one passes.
	// However, it does consistently try each branch, which these games seem to expect.
	bool delaySlotIsBranch = MIPSCodeUtils::IsVFPUBranch(delaySlotOp);
	bool delaySlotIsNice = !delaySlotIsBranch && IsDelaySlotNiceVFPU(op, delaySlotOp);
	CONDITIONAL_NICE_DELAYSLOT;
	if (!likely && delaySlotIsNice)
		CompileDelaySlot(DELAYSLOT_NICE);
	if (delaySlotIsBranch && (signed short)(delaySlotOp & 0xFFFF) != (signed short)(op & 0xFFFF) - 1)
		ERROR_LOG_REPORT(JIT, "VFPU branch in VFPU delay slot at %08x with different target", js.compilerPC);

	int imm3 = (op >> 18) & 7;

	gpr.MapReg(MIPS_REG_VFPUCC);
	TST(gpr.R(MIPS_REG_VFPUCC), Operand2(1 << imm3, TYPE_IMM));

	ArmGen::FixupBranch ptr;
	js.inDelaySlot = true;
	if (!likely)
	{
		if (!delaySlotIsNice && !delaySlotIsBranch)
			CompileDelaySlot(DELAYSLOT_SAFE_FLUSH);
		else
			FlushAll();
		ptr = B_CC(cc);
	}
	else
	{
		FlushAll();
		ptr = B_CC(cc);
		if (!delaySlotIsBranch)
			CompileDelaySlot(DELAYSLOT_FLUSH);
	}
	js.inDelaySlot = false;

	// Take the branch
	WriteExit(targetAddr, js.nextExit++);

	SetJumpTarget(ptr);
	// Not taken
	u32 notTakenTarget = js.compilerPC + (delaySlotIsBranch ? 4 : 8);
	WriteExit(notTakenTarget, js.nextExit++);
	js.compiling = false;
}

void Jit::Comp_VBranch(MIPSOpcode op)
{
	switch ((op >> 16) & 3)
	{
	case 0:	BranchVFPUFlag(op, CC_NEQ, false); break;  // bvf
	case 1: BranchVFPUFlag(op, CC_EQ,  false); break;  // bvt
	case 2: BranchVFPUFlag(op, CC_NEQ, true);  break;  // bvfl
	case 3: BranchVFPUFlag(op, CC_EQ,  true);  break;  // bvtl
	}
}

void Jit::Comp_Jump(MIPSOpcode op)
{
	if (js.inDelaySlot) {
		ERROR_LOG_REPORT(JIT, "Branch in Jump delay slot at %08x in block starting at %08x", js.compilerPC, js.blockStart);
		return;
	}
	u32 off = _IMM26 << 2;
	u32 targetAddr = (js.compilerPC & 0xF0000000) | off;

	// Might be a stubbed address or something?
	if (!Memory::IsValidAddress(targetAddr))
	{
		if (js.nextExit == 0)
			ERROR_LOG_REPORT(JIT, "Jump to invalid address: %08x", targetAddr)
		else
			js.compiling = false;
		// TODO: Mark this block dirty or something?  May be indication it will be changed by imports.
		return;
	}

	switch (op >> 26) 
	{
	case 2: //j
		CompileDelaySlot(DELAYSLOT_NICE);
		if (jo.continueJumps && js.numInstructions < jo.continueMaxInstructions) {
			// Account for the increment in the loop.
			js.compilerPC = targetAddr - 4;
			// In case the delay slot was a break or something.
			js.compiling = true;
			return;
		}
		FlushAll();
		WriteExit(targetAddr, js.nextExit++);
		break;

	case 3: //jal
		gpr.SetImm(MIPS_REG_RA, js.compilerPC + 8);
		CompileDelaySlot(DELAYSLOT_NICE);
		if (jo.continueJumps && js.numInstructions < jo.continueMaxInstructions) {
			// Account for the increment in the loop.
			js.compilerPC = targetAddr - 4;
			// In case the delay slot was a break or something.
			js.compiling = true;
			return;
		}
		FlushAll();
		WriteExit(targetAddr, js.nextExit++);
		break;

	default:
		_dbg_assert_msg_(CPU,0,"Trying to compile instruction that can't be compiled");
		break;
	}
	js.compiling = false;
}

void Jit::Comp_JumpReg(MIPSOpcode op)
{
	if (js.inDelaySlot) {
		ERROR_LOG_REPORT(JIT, "Branch in JumpReg delay slot at %08x in block starting at %08x", js.compilerPC, js.blockStart);
		return;
	}
	MIPSGPReg rs = _RS;
	MIPSGPReg rd = _RD;
	bool andLink = (op & 0x3f) == 9;

	MIPSOpcode delaySlotOp = Memory::Read_Instruction(js.compilerPC + 4);
	bool delaySlotIsNice = IsDelaySlotNiceReg(op, delaySlotOp, rs);
	if (andLink && rs == rd)
		delaySlotIsNice = false;
	CONDITIONAL_NICE_DELAYSLOT;

	ARMReg destReg = R8;
	if (IsSyscall(delaySlotOp)) {
		gpr.MapReg(rs);
		MovToPC(gpr.R(rs));  // For syscall to be able to return.
		if (andLink)
			gpr.SetImm(rd, js.compilerPC + 8);
		CompileDelaySlot(DELAYSLOT_FLUSH);
		return;  // Syscall wrote exit code.
	} else if (delaySlotIsNice) {
		if (andLink)
			gpr.SetImm(rd, js.compilerPC + 8);
		CompileDelaySlot(DELAYSLOT_NICE);

		if (!andLink && rs == MIPS_REG_RA && g_Config.bDiscardRegsOnJRRA) {
			// According to the MIPS ABI, there are some regs we don't need to preserve.
			// Let's discard them so we don't need to write them back.
			// NOTE: Not all games follow the MIPS ABI! Tekken 6, for example, will crash
			// with this enabled.
			gpr.DiscardR(MIPS_REG_COMPILER_SCRATCH);
			for (int i = MIPS_REG_A0; i <= MIPS_REG_T7; i++)
				gpr.DiscardR((MIPSGPReg)i);
			gpr.DiscardR(MIPS_REG_T8);
			gpr.DiscardR(MIPS_REG_T9);
		}

		if (jo.continueJumps && gpr.IsImm(rs) && js.numInstructions < jo.continueMaxInstructions) {
			// Account for the increment in the loop.
			js.compilerPC = gpr.GetImm(rs) - 4;
			// In case the delay slot was a break or something.
			js.compiling = true;
			return;
		}

		gpr.MapReg(rs);
		destReg = gpr.R(rs);  // Safe because FlushAll doesn't change any regs
		FlushAll();
	} else {
		// Delay slot - this case is very rare, might be able to free up R8.
		gpr.MapReg(rs);
		MOV(R8, gpr.R(rs));
		if (andLink)
			gpr.SetImm(rd, js.compilerPC + 8);
		CompileDelaySlot(DELAYSLOT_NICE);
		FlushAll();
	}

	switch (op & 0x3f) 
	{
	case 8: //jr
		break;
	case 9: //jalr
		break;
	default:
		_dbg_assert_msg_(CPU,0,"Trying to compile instruction that can't be compiled");
		break;
	}

	WriteExitDestInR(destReg);
	js.compiling = false;
}

	
void Jit::Comp_Syscall(MIPSOpcode op)
{
	// If we're in a delay slot, this is off by one.
	const int offset = js.inDelaySlot ? -1 : 0;
	WriteDownCount(offset);
	js.downcountAmount = -offset;

	// TODO: Maybe discard v0, v1, and some temps?  Definitely at?
	FlushAll();

	SaveDowncount();
	// Skip the CallSyscall where possible.
	void *quickFunc = GetQuickSyscallFunc(op);
	if (quickFunc)
	{
		gpr.SetRegImm(R0, (u32)(intptr_t)GetSyscallInfo(op));
		QuickCallFunction(R1, quickFunc);
	}
	else
	{
		gpr.SetRegImm(R0, op.encoding);
		QuickCallFunction(R1, (void *)&CallSyscall);
	}
	RestoreDowncount();

	WriteSyscallExit();
	js.compiling = false;
}

void Jit::Comp_Break(MIPSOpcode op)
{
	Comp_Generic(op);
	WriteSyscallExit();
	js.compiling = false;
}

}   // namespace Mipscomp
