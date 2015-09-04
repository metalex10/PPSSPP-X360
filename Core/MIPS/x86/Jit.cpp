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
#include <iterator>

#include "math/math_util.h"

#include "Common/ChunkFile.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/CoreTiming.h"
#include "Core/Config.h"
#include "Core/Reporting.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSCodeUtils.h"
#include "Core/MIPS/MIPSInt.h"
#include "Core/MIPS/MIPSTables.h"

#include "RegCache.h"
#include "Jit.h"

#include "Core/Host.h"
#include "Core/Debugger/Breakpoints.h"

namespace MIPSComp
{

#ifdef _M_IX86

#define SAVE_FLAGS PUSHF();
#define LOAD_FLAGS POPF();

#else

static u64 saved_flags;

#define SAVE_FLAGS {PUSHF(); POP(64, R(EAX)); MOV(64, M(&saved_flags), R(EAX));}
#define LOAD_FLAGS {MOV(64, R(EAX), M(&saved_flags)); PUSH(64, R(EAX)); POPF();}

#endif

const bool USE_JIT_MISSMAP = false;
static std::map<std::string, u32> notJitOps;

template<typename A, typename B>
std::pair<B,A> flip_pair(const std::pair<A,B> &p)
{
    return std::pair<B, A>(p.second, p.first);
}

u32 JitBreakpoint()
{
	// Should we skip this breakpoint?
	if (CBreakPoints::CheckSkipFirst() == currentMIPS->pc)
		return 0;

	auto cond = CBreakPoints::GetBreakPointCondition(currentMIPS->pc);
	if (cond && !cond->Evaluate())
		return 0;

	Core_EnableStepping(true);
	host->SetDebugMode(true);

	// There's probably a better place for this.
	if (USE_JIT_MISSMAP)
	{
		std::map<u32, std::string> notJitSorted;
		std::transform(notJitOps.begin(), notJitOps.end(), std::inserter(notJitSorted, notJitSorted.begin()), flip_pair<std::string, u32>);

		std::string message;
		char temp[256];
		int remaining = 15;
		for (auto it = notJitSorted.rbegin(), end = notJitSorted.rend(); it != end && --remaining >= 0; ++it)
		{
			snprintf(temp, 256, " (%d), ", it->first);
			message += it->second + temp;
		}

		if (message.size() > 2)
			message.resize(message.size() - 2);

		NOTICE_LOG(JIT, "Top ops compiled to interpreter: %s", message.c_str());
	}

	return 1;
}

static void JitLogMiss(MIPSOpcode op)
{
	if (USE_JIT_MISSMAP)
		notJitOps[MIPSGetName(op)]++;

	MIPSInterpretFunc func = MIPSGetInterpretFunc(op);
	func(op);
}

JitOptions::JitOptions()
{
	enableBlocklink = true;
	// WARNING: These options don't work properly with cache clearing.
	// Need to find a smart way to handle before enabling.
	immBranches = false;
	continueBranches = false;
	continueJumps = false;
	continueMaxInstructions = 300;
}

#ifdef _MSC_VER
// JitBlockCache doesn't use this, just stores it.
#pragma warning(disable:4355)
#endif
Jit::Jit(MIPSState *mips) : blocks(mips, this), mips_(mips)
{
	blocks.Init();
	gpr.SetEmitter(this);
	fpr.SetEmitter(this);
	AllocCodeSpace(1024 * 1024 * 16);
	asm_.Init(mips, this);
	// TODO: If it becomes possible to switch from the interpreter, this should be set right.
	js.startDefaultPrefix = true;
}

Jit::~Jit() {
}

void Jit::DoState(PointerWrap &p)
{
	auto s = p.Section("Jit", 1);
	if (!s)
		return;

	p.Do(js.startDefaultPrefix);
}

// This is here so the savestate matches between jit and non-jit.
void Jit::DoDummyState(PointerWrap &p)
{
	auto s = p.Section("Jit", 1);
	if (!s)
		return;

	bool dummy = false;
	p.Do(dummy);
}


void Jit::GetStateAndFlushAll(RegCacheState &state)
{
	gpr.GetState(state.gpr);
	fpr.GetState(state.fpr);
	FlushAll();
}

void Jit::RestoreState(const RegCacheState state)
{
	gpr.RestoreState(state.gpr);
	fpr.RestoreState(state.fpr);
}

void Jit::FlushAll()
{
	gpr.Flush();
	fpr.Flush();
	FlushPrefixV();
}

void Jit::FlushPrefixV()
{
	if ((js.prefixSFlag & JitState::PREFIX_DIRTY) != 0)
	{
		MOV(32, M((void *)&mips_->vfpuCtrl[VFPU_CTRL_SPREFIX]), Imm32(js.prefixS));
		js.prefixSFlag = (JitState::PrefixState) (js.prefixSFlag & ~JitState::PREFIX_DIRTY);
	}

	if ((js.prefixTFlag & JitState::PREFIX_DIRTY) != 0)
	{
		MOV(32, M((void *)&mips_->vfpuCtrl[VFPU_CTRL_TPREFIX]), Imm32(js.prefixT));
		js.prefixTFlag = (JitState::PrefixState) (js.prefixTFlag & ~JitState::PREFIX_DIRTY);
	}

	if ((js.prefixDFlag & JitState::PREFIX_DIRTY) != 0)
	{
		MOV(32, M((void *)&mips_->vfpuCtrl[VFPU_CTRL_DPREFIX]), Imm32(js.prefixD));
		js.prefixDFlag = (JitState::PrefixState) (js.prefixDFlag & ~JitState::PREFIX_DIRTY);
	}
}

void Jit::WriteDowncount(int offset)
{
	const int downcount = js.downcountAmount + offset;
	SUB(32, M(&currentMIPS->downcount), downcount > 127 ? Imm32(downcount) : Imm8(downcount));
}

void Jit::ClearCache()
{
	blocks.Clear();
	ClearCodeSpace();
}

void Jit::ClearCacheAt(u32 em_address, int length)
{
	blocks.InvalidateICache(em_address, length);
}

void Jit::CompileDelaySlot(int flags, RegCacheState *state)
{
	const u32 addr = js.compilerPC + 4;

	// Need to offset the downcount which was already incremented for the branch + delay slot.
	CheckJitBreakpoint(addr, -2);

	if (flags & DELAYSLOT_SAFE)
		SAVE_FLAGS; // preserve flag around the delay slot!

	js.inDelaySlot = true;
	MIPSOpcode op = Memory::Read_Instruction(addr);
	MIPSCompileOp(op);
	js.inDelaySlot = false;

	if (flags & DELAYSLOT_FLUSH)
	{
		if (state != NULL)
			GetStateAndFlushAll(*state);
		else
			FlushAll();
	}
	if (flags & DELAYSLOT_SAFE)
		LOAD_FLAGS; // restore flag!
}

void Jit::CompileAt(u32 addr)
{
	CheckJitBreakpoint(addr, 0);
	MIPSOpcode op = Memory::Read_Instruction(addr);
	MIPSCompileOp(op);
}

void Jit::EatInstruction(MIPSOpcode op)
{
	MIPSInfo info = MIPSGetInfo(op);
	if (info & DELAYSLOT) {
		ERROR_LOG_REPORT_ONCE(ateDelaySlot, JIT, "Ate a branch op.");
	}
	if (js.inDelaySlot) {
		ERROR_LOG_REPORT_ONCE(ateInDelaySlot, JIT, "Ate an instruction inside a delay slot.")
	}

	CheckJitBreakpoint(js.compilerPC + 4, 0);
	js.numInstructions++;
	js.compilerPC += 4;
	js.downcountAmount += MIPSGetInstructionCycleEstimate(op);
}

void Jit::Compile(u32 em_address)
{
	if (GetSpaceLeft() < 0x10000 || blocks.IsFull())
	{
		ClearCache();
	}

	int block_num = blocks.AllocateBlock(em_address);
	JitBlock *b = blocks.GetBlock(block_num);
	DoJit(em_address, b);
	blocks.FinalizeBlock(block_num, jo.enableBlocklink);

	// Drat.  The VFPU hit an uneaten prefix at the end of a block.
	if (js.startDefaultPrefix && js.MayHavePrefix()) {
		WARN_LOG(JIT, "Uneaten prefix at end of block: %08x", js.compilerPC - 4);
		js.startDefaultPrefix = false;
		// Our assumptions are all wrong so it's clean-slate time.
		ClearCache();

		// Let's try that one more time.  We won't get back here because we toggled the value.
		Compile(em_address);
	}
}

void Jit::RunLoopUntil(u64 globalticks)
{
	// TODO: copy globalticks somewhere
	((void (*)())asm_.enterCode)();
	// NOTICE_LOG(JIT, "Exited jitted code at %i, corestate=%i, dc=%i", CoreTiming::GetTicks() / 1000, (int)coreState, CoreTiming::downcount);
}

const u8 *Jit::DoJit(u32 em_address, JitBlock *b)
{
	js.cancel = false;
	js.blockStart = js.compilerPC = mips_->pc;
	js.nextExit = 0;
	js.downcountAmount = 0;
	js.curBlock = b;
	js.compiling = true;
	js.inDelaySlot = false;
	js.afterOp = JitState::AFTER_NONE;
	js.PrefixStart();

	// We add a check before the block, used when entering from a linked block.
	b->checkedEntry = GetCodePtr();
	// Downcount flag check. The last block decremented downcounter, and the flag should still be available.
	FixupBranch skip = J_CC(CC_NBE);
	MOV(32, M(&mips_->pc), Imm32(js.blockStart));
	JMP(asm_.outerLoop, true);  // downcount hit zero - go advance.
	SetJumpTarget(skip);

	b->normalEntry = GetCodePtr();

	MIPSAnalyst::AnalysisResults analysis = MIPSAnalyst::Analyze(em_address);

	gpr.Start(mips_, analysis);
	fpr.Start(mips_, analysis);

	js.numInstructions = 0;
	while (js.compiling) {
		// Jit breakpoints are quite fast, so let's do them in release too.
		CheckJitBreakpoint(js.compilerPC, 0);

		MIPSOpcode inst = Memory::Read_Instruction(js.compilerPC);
		js.downcountAmount += MIPSGetInstructionCycleEstimate(inst);

		MIPSCompileOp(inst);

		if (js.afterOp & JitState::AFTER_CORE_STATE) {
			// TODO: Save/restore?
			FlushAll();

			// If we're rewinding, CORE_NEXTFRAME should not cause a rewind.
			// It doesn't really matter either way if we're not rewinding.
			// CORE_RUNNING is <= CORE_NEXTFRAME.
			CMP(32, M((void*)&coreState), Imm32(CORE_NEXTFRAME));
			FixupBranch skipCheck = J_CC(CC_LE);
			if (js.afterOp & JitState::AFTER_REWIND_PC_BAD_STATE)
				MOV(32, M(&mips_->pc), Imm32(js.compilerPC));
			else
				MOV(32, M(&mips_->pc), Imm32(js.compilerPC + 4));
			WriteSyscallExit();
			SetJumpTarget(skipCheck);

			js.afterOp = JitState::AFTER_NONE;
		}

		js.compilerPC += 4;
		js.numInstructions++;

		// Safety check, in case we get a bunch of really large jit ops without a lot of branching.
		if (GetSpaceLeft() < 0x800)
		{
			FlushAll();
			WriteExit(js.compilerPC, js.nextExit++);
			js.compiling = false;
		}
	}

	b->codeSize = (u32)(GetCodePtr() - b->normalEntry);
	NOP();
	AlignCode4();
	b->originalSize = js.numInstructions;
	return b->normalEntry;
}

void Jit::Comp_RunBlock(MIPSOpcode op)
{
	// This shouldn't be necessary, the dispatcher should catch us before we get here.
	ERROR_LOG(JIT, "Comp_RunBlock");
}

void Jit::Comp_Generic(MIPSOpcode op)
{
	FlushAll();
	MIPSInterpretFunc func = MIPSGetInterpretFunc(op);
	_dbg_assert_msg_(JIT, (MIPSGetInfo(op) & DELAYSLOT) == 0, "Cannot use interpreter for branch ops.");

	if (func)
	{
		MOV(32, M(&mips_->pc), Imm32(js.compilerPC));
		if (USE_JIT_MISSMAP)
			ABI_CallFunctionC((void *)&JitLogMiss, op.encoding);
		else
			ABI_CallFunctionC((void *)func, op.encoding);
	}
	else
		ERROR_LOG_REPORT(JIT, "Trying to compile instruction %08x that can't be interpreted", op.encoding);

	const MIPSInfo info = MIPSGetInfo(op);
	if ((info & IS_VFPU) != 0 && (info & VFPU_NO_PREFIX) == 0)
	{
		// If it does eat them, it'll happen in MIPSCompileOp().
		if ((info & OUT_EAT_PREFIX) == 0)
			js.PrefixUnknown();
	}
}

void Jit::WriteExit(u32 destination, int exit_num)
{
	_dbg_assert_msg_(JIT, exit_num < MAX_JIT_BLOCK_EXITS, "Expected a valid exit_num");

	if (!Memory::IsValidAddress(destination)) {
		ERROR_LOG_REPORT(JIT, "Trying to write block exit to illegal destination %08x: pc = %08x", destination, currentMIPS->pc);
	}
	// If we need to verify coreState and rewind, we may not jump yet.
	if (js.afterOp & (JitState::AFTER_CORE_STATE | JitState::AFTER_REWIND_PC_BAD_STATE))
	{
		// CORE_RUNNING is <= CORE_NEXTFRAME.
		CMP(32, M((void*)&coreState), Imm32(CORE_NEXTFRAME));
		FixupBranch skipCheck = J_CC(CC_LE);
		MOV(32, M(&mips_->pc), Imm32(js.compilerPC));
		WriteSyscallExit();
		SetJumpTarget(skipCheck);

		js.afterOp = JitState::AFTER_NONE;
	}

	WriteDowncount();

	//If nobody has taken care of this yet (this can be removed when all branches are done)
	JitBlock *b = js.curBlock;
	b->exitAddress[exit_num] = destination;
	b->exitPtrs[exit_num] = GetWritableCodePtr();

	// Link opportunity!
	int block = blocks.GetBlockNumberFromStartAddress(destination);
	if (block >= 0 && jo.enableBlocklink) {
		// It exists! Joy of joy!
		JMP(blocks.GetBlock(block)->checkedEntry, true);
		b->linkStatus[exit_num] = true;
	} else {
		// No blocklinking.
		MOV(32, M(&mips_->pc), Imm32(destination));
		JMP(asm_.dispatcher, true);
	}
}

void Jit::WriteExitDestInEAX()
{
	// TODO: Some wasted potential, dispatcher will always read this back into EAX.
	MOV(32, M(&mips_->pc), R(EAX));

	// If we need to verify coreState and rewind, we may not jump yet.
	if (js.afterOp & (JitState::AFTER_CORE_STATE | JitState::AFTER_REWIND_PC_BAD_STATE))
	{
		// CORE_RUNNING is <= CORE_NEXTFRAME.
		CMP(32, M((void*)&coreState), Imm32(CORE_NEXTFRAME));
		FixupBranch skipCheck = J_CC(CC_LE);
		MOV(32, M(&mips_->pc), Imm32(js.compilerPC));
		WriteSyscallExit();
		SetJumpTarget(skipCheck);

		js.afterOp = JitState::AFTER_NONE;
	}

	WriteDowncount();

	// Validate the jump to avoid a crash?
	if (!g_Config.bFastMemory)
	{
		CMP(32, R(EAX), Imm32(PSP_GetKernelMemoryBase()));
		FixupBranch tooLow = J_CC(CC_B);
		CMP(32, R(EAX), Imm32(PSP_GetUserMemoryEnd()));
		FixupBranch tooHigh = J_CC(CC_AE);

		// Need to set neg flag again if necessary.
		SUB(32, M(&currentMIPS->downcount), Imm32(0));
		JMP(asm_.dispatcher, true);

		SetJumpTarget(tooLow);
		SetJumpTarget(tooHigh);

		CallProtectedFunction((void *) Memory::GetPointer, R(EAX));
		CMP(32, R(EAX), Imm32(0));
		FixupBranch skip = J_CC(CC_NE);

		// TODO: "Ignore" this so other threads can continue?
		if (g_Config.bIgnoreBadMemAccess)
			CallProtectedFunction((void *) Core_UpdateState, Imm32(CORE_ERROR));

		SUB(32, M(&currentMIPS->downcount), Imm32(0));
		JMP(asm_.dispatcherCheckCoreState, true);
		SetJumpTarget(skip);

		SUB(32, M(&currentMIPS->downcount), Imm32(0));
		J_CC(CC_NE, asm_.dispatcher, true);
	}
	else
		JMP(asm_.dispatcher, true);
}

void Jit::WriteSyscallExit()
{
	WriteDowncount();
	JMP(asm_.dispatcherCheckCoreState, true);
}

bool Jit::CheckJitBreakpoint(u32 addr, int downcountOffset)
{
	if (CBreakPoints::IsAddressBreakPoint(addr))
	{
		SAVE_FLAGS;
		FlushAll();
		MOV(32, M(&mips_->pc), Imm32(js.compilerPC));
		ABI_CallFunction((void *)&JitBreakpoint);

		// If 0, the conditional breakpoint wasn't taken.
		CMP(32, R(EAX), Imm32(0));
		FixupBranch skip = J_CC(CC_Z);
		WriteDowncount(downcountOffset);
		// Just to fix the stack.
		LOAD_FLAGS;
		JMP(asm_.dispatcherCheckCoreState, true);
		SetJumpTarget(skip);

		LOAD_FLAGS;

		return true;
	}

	return false;
}

Jit::JitSafeMem::JitSafeMem(Jit *jit, MIPSGPReg raddr, s32 offset, u32 alignMask)
	: jit_(jit), raddr_(raddr), offset_(offset), needsCheck_(false), needsSkip_(false), alignMask_(alignMask)
{
	// This makes it more instructions, so let's play it safe and say we need a far jump.
	far_ = !g_Config.bIgnoreBadMemAccess || !CBreakPoints::GetMemChecks().empty();
	if (jit_->gpr.IsImm(raddr_))
		iaddr_ = jit_->gpr.GetImm(raddr_) + offset_;
	else
		iaddr_ = (u32) -1;

	fast_ = g_Config.bFastMemory || raddr == MIPS_REG_SP;
}

void Jit::JitSafeMem::SetFar()
{
	_dbg_assert_msg_(JIT, !needsSkip_, "Sorry, you need to call SetFar() earlier.");
	far_ = true;
}

bool Jit::JitSafeMem::PrepareWrite(OpArg &dest, int size)
{
	size_ = size;
	// If it's an immediate, we can do the write if valid.
	if (iaddr_ != (u32) -1)
	{
		if (ImmValid())
		{
			MemCheckImm(MEM_WRITE);

#ifdef _M_IX86
			dest = M(Memory::base + (iaddr_ & Memory::MEMVIEW32_MASK & alignMask_));
#else
			dest = MDisp(RBX, iaddr_ & alignMask_);
#endif
			return true;
		}
		else
			return false;
	}
	// Otherwise, we always can do the write (conditionally.)
	else
		dest = PrepareMemoryOpArg(MEM_WRITE);
	return true;
}

bool Jit::JitSafeMem::PrepareRead(OpArg &src, int size)
{
	size_ = size;
	if (iaddr_ != (u32) -1)
	{
		if (ImmValid())
		{
			MemCheckImm(MEM_READ);

#ifdef _M_IX86
			src = M(Memory::base + (iaddr_ & Memory::MEMVIEW32_MASK & alignMask_));
#else
			src = MDisp(RBX, iaddr_ & alignMask_);
#endif
			return true;
		}
		else
			return false;
	}
	else
		src = PrepareMemoryOpArg(MEM_READ);
	return true;
}

OpArg Jit::JitSafeMem::NextFastAddress(int suboffset)
{
	if (jit_->gpr.IsImm(raddr_))
	{
		u32 addr = (jit_->gpr.GetImm(raddr_) + offset_ + suboffset) & alignMask_;

#ifdef _M_IX86
		return M(Memory::base + (addr & Memory::MEMVIEW32_MASK));
#else
		return MDisp(RBX, addr);
#endif
	}

	_dbg_assert_msg_(JIT, (suboffset & alignMask_) == suboffset, "suboffset must be aligned");

#ifdef _M_IX86
	return MDisp(xaddr_, (u32) Memory::base + offset_ + suboffset);
#else
	return MComplex(RBX, xaddr_, SCALE_1, offset_ + suboffset);
#endif
}

OpArg Jit::JitSafeMem::PrepareMemoryOpArg(ReadType type)
{
	// We may not even need to move into EAX as a temporary.
	bool needTemp = alignMask_ != 0xFFFFFFFF;
#ifdef _M_IX86
	// We always mask on 32 bit in fast memory mode.
	needTemp = needTemp || fast_;
#endif

	if (jit_->gpr.R(raddr_).IsSimpleReg() && !needTemp)
	{
		jit_->gpr.MapReg(raddr_, true, false);
		xaddr_ = jit_->gpr.RX(raddr_);
	}
	else
	{
		jit_->MOV(32, R(EAX), jit_->gpr.R(raddr_));
		xaddr_ = EAX;
	}

	MemCheckAsm(type);

	if (!fast_)
	{
		// Is it in physical ram?
		jit_->CMP(32, R(xaddr_), Imm32(PSP_GetKernelMemoryBase() - offset_));
		tooLow_ = jit_->J_CC(CC_B);
		jit_->CMP(32, R(xaddr_), Imm32(PSP_GetUserMemoryEnd() - offset_ - (size_ - 1)));
		tooHigh_ = jit_->J_CC(CC_AE);

		// We may need to jump back up here.
		safe_ = jit_->GetCodePtr();
	}
	else
	{
#ifdef _M_IX86
		jit_->AND(32, R(EAX), Imm32(Memory::MEMVIEW32_MASK));
#endif
	}

	// TODO: This could be more optimal, but the common case is that we want xaddr_ not to include offset_.
	// Since we need to align them after add, we add and subtract.
	if (alignMask_ != 0xFFFFFFFF)
	{
		jit_->ADD(32, R(xaddr_), Imm32(offset_));
		jit_->AND(32, R(xaddr_), Imm32(alignMask_));
		jit_->SUB(32, R(xaddr_), Imm32(offset_));
	}

#ifdef _M_IX86
	return MDisp(xaddr_, (u32) Memory::base + offset_);
#else
	return MComplex(RBX, xaddr_, SCALE_1, offset_);
#endif
}

void Jit::JitSafeMem::PrepareSlowAccess()
{
	// Skip the fast path (which the caller wrote just now.)
	skip_ = jit_->J(far_);
	needsSkip_ = true;
	jit_->SetJumpTarget(tooLow_);
	jit_->SetJumpTarget(tooHigh_);

	// Might also be the scratchpad.
	jit_->CMP(32, R(xaddr_), Imm32(PSP_GetScratchpadMemoryBase() - offset_));
	FixupBranch tooLow = jit_->J_CC(CC_B);
	jit_->CMP(32, R(xaddr_), Imm32(PSP_GetScratchpadMemoryEnd() - offset_ - (size_ - 1)));
	jit_->J_CC(CC_B, safe_);
	jit_->SetJumpTarget(tooLow);
}

bool Jit::JitSafeMem::PrepareSlowWrite()
{
	// If it's immediate, we only need a slow write on invalid.
	if (iaddr_ != (u32) -1)
		return !fast_ && !ImmValid();

	if (!fast_)
	{
		PrepareSlowAccess();
		return true;
	}
	else
		return false;
}

void Jit::JitSafeMem::DoSlowWrite(void *safeFunc, const OpArg src, int suboffset)
{
	if (iaddr_ != (u32) -1)
		jit_->MOV(32, R(EAX), Imm32((iaddr_ + suboffset) & alignMask_));
	else
	{
		jit_->LEA(32, EAX, MDisp(xaddr_, offset_ + suboffset));
		if (alignMask_ != 0xFFFFFFFF)
			jit_->AND(32, R(EAX), Imm32(alignMask_));
	}

	jit_->CallProtectedFunction(safeFunc, src, R(EAX));
	needsCheck_ = true;
}

bool Jit::JitSafeMem::PrepareSlowRead(void *safeFunc)
{
	if (!fast_)
	{
		if (iaddr_ != (u32) -1)
		{
			// No slow read necessary.
			if (ImmValid())
				return false;
			jit_->MOV(32, R(EAX), Imm32(iaddr_ & alignMask_));
		}
		else
		{
			PrepareSlowAccess();
			jit_->LEA(32, EAX, MDisp(xaddr_, offset_));
			if (alignMask_ != 0xFFFFFFFF)
				jit_->AND(32, R(EAX), Imm32(alignMask_));
		}

		jit_->CallProtectedFunction(safeFunc, R(EAX));
		needsCheck_ = true;
		return true;
	}
	else
		return false;
}

void Jit::JitSafeMem::NextSlowRead(void *safeFunc, int suboffset)
{
	_dbg_assert_msg_(JIT, !fast_, "NextSlowRead() called in fast memory mode?");

	// For simplicity, do nothing for 0.  We already read in PrepareSlowRead().
	if (suboffset == 0)
		return;

	if (jit_->gpr.IsImm(raddr_))
	{
		_dbg_assert_msg_(JIT, !Memory::IsValidAddress(iaddr_ + suboffset), "NextSlowRead() for an invalid immediate address?");

		jit_->MOV(32, R(EAX), Imm32((iaddr_ + suboffset) & alignMask_));
	}
	// For GPR, if xaddr_ was the dest register, this will be wrong.  Don't use in GPR.
	else
	{
		jit_->LEA(32, EAX, MDisp(xaddr_, offset_ + suboffset));
		if (alignMask_ != 0xFFFFFFFF)
			jit_->AND(32, R(EAX), Imm32(alignMask_));
	}

	jit_->CallProtectedFunction(safeFunc, R(EAX));
}

bool Jit::JitSafeMem::ImmValid()
{
	return iaddr_ != (u32) -1 && Memory::IsValidAddress(iaddr_) && Memory::IsValidAddress(iaddr_ + size_ - 1);
}

void Jit::JitSafeMem::Finish()
{
	// Memory::Read_U32/etc. may have tripped coreState.
	if (needsCheck_ && !g_Config.bIgnoreBadMemAccess)
		jit_->js.afterOp |= JitState::AFTER_CORE_STATE;
	if (needsSkip_)
		jit_->SetJumpTarget(skip_);
	for (auto it = skipChecks_.begin(), end = skipChecks_.end(); it != end; ++it)
		jit_->SetJumpTarget(*it);
}

void JitMemCheck(u32 addr, int size, int isWrite)
{
	// Should we skip this breakpoint?
	if (CBreakPoints::CheckSkipFirst() == currentMIPS->pc)
		return;

	// Did we already hit one?
	if (coreState != CORE_RUNNING && coreState != CORE_NEXTFRAME)
		return;

	CBreakPoints::ExecMemCheck(addr, isWrite == 1, size, currentMIPS->pc);
}

void Jit::JitSafeMem::MemCheckImm(ReadType type)
{
	MemCheck *check = CBreakPoints::GetMemCheck(iaddr_, size_);
	if (check)
	{
		if (!(check->cond & MEMCHECK_READ) && type == MEM_READ)
			return;
		if (!(check->cond & MEMCHECK_WRITE) && type == MEM_WRITE)
			return;

		jit_->MOV(32, M(&jit_->mips_->pc), Imm32(jit_->js.compilerPC));
		jit_->CallProtectedFunction((void *)&JitMemCheck, iaddr_, size_, type == MEM_WRITE ? 1 : 0);

		// CORE_RUNNING is <= CORE_NEXTFRAME.
		jit_->CMP(32, M((void*)&coreState), Imm32(CORE_NEXTFRAME));
		skipChecks_.push_back(jit_->J_CC(CC_G, true));
		jit_->js.afterOp |= JitState::AFTER_CORE_STATE | JitState::AFTER_REWIND_PC_BAD_STATE;
	}
}

void Jit::JitSafeMem::MemCheckAsm(ReadType type)
{
	const auto memchecks = CBreakPoints::GetMemCheckRanges();
	bool possible = false;
	for (auto it = memchecks.begin(), end = memchecks.end(); it != end; ++it)
	{
		if (!(it->cond & MEMCHECK_READ) && type == MEM_READ)
			continue;
		if (!(it->cond & MEMCHECK_WRITE) && type == MEM_WRITE)
			continue;

		possible = true;

		FixupBranch skipNext, skipNextRange;
		if (it->end != 0)
		{
			jit_->CMP(32, R(xaddr_), Imm32(it->start - offset_ - size_));
			skipNext = jit_->J_CC(CC_BE);
			jit_->CMP(32, R(xaddr_), Imm32(it->end - offset_));
			skipNextRange = jit_->J_CC(CC_AE);
		}
		else
		{
			jit_->CMP(32, R(xaddr_), Imm32(it->start - offset_));
			skipNext = jit_->J_CC(CC_NE);
		}

		// Keep the stack 16-byte aligned, just PUSH/POP 4 times.
		for (int i = 0; i < 4; ++i)
			jit_->PUSH(xaddr_);
		jit_->MOV(32, M(&jit_->mips_->pc), Imm32(jit_->js.compilerPC));
		jit_->ADD(32, R(xaddr_), Imm32(offset_));
		jit_->CallProtectedFunction((void *)&JitMemCheck, R(xaddr_), size_, type == MEM_WRITE ? 1 : 0);
		for (int i = 0; i < 4; ++i)
			jit_->POP(xaddr_);

		jit_->SetJumpTarget(skipNext);
		if (it->end != 0)
			jit_->SetJumpTarget(skipNextRange);
	}

	if (possible)
	{
		// CORE_RUNNING is <= CORE_NEXTFRAME.
		jit_->CMP(32, M((void*)&coreState), Imm32(CORE_NEXTFRAME));
		skipChecks_.push_back(jit_->J_CC(CC_G, true));
		jit_->js.afterOp |= JitState::AFTER_CORE_STATE | JitState::AFTER_REWIND_PC_BAD_STATE;
	}
}

void Jit::CallProtectedFunction(void *func, const OpArg &arg1)
{
	// We don't regcache RCX, so the below is safe (and also faster, maybe branch prediction?)
	ABI_CallFunctionA(thunks.ProtectFunction(func, 1), arg1);
}

void Jit::CallProtectedFunction(void *func, const OpArg &arg1, const OpArg &arg2)
{
	// We don't regcache RCX/RDX, so the below is safe (and also faster, maybe branch prediction?)
	ABI_CallFunctionAA(thunks.ProtectFunction(func, 2), arg1, arg2);
}

void Jit::CallProtectedFunction(void *func, const u32 arg1, const u32 arg2, const u32 arg3)
{
	// On x64, we need to save R8, which is caller saved.
	ABI_CallFunction((void *)thunks.GetSaveRegsFunction());
	ABI_CallFunctionCCC(func, arg1, arg2, arg3);
	ABI_CallFunction((void *)thunks.GetLoadRegsFunction());
}

void Jit::CallProtectedFunction(void *func, const OpArg &arg1, const u32 arg2, const u32 arg3)
{
	// On x64, we need to save R8, which is caller saved.
	ABI_CallFunction((void *)thunks.GetSaveRegsFunction());
	ABI_CallFunctionACC(func, arg1, arg2, arg3);
	ABI_CallFunction((void *)thunks.GetLoadRegsFunction());
}

void Jit::Comp_DoNothing(MIPSOpcode op) { }

} // namespace
