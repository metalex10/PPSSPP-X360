// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include "ArmEmitter.h"
#include "CPUDetect.h"

#include "base/logging.h"

#include <assert.h>
#include <stdarg.h>
#include <stddef.h>

// For cache flushing on Symbian/iOS/Blackberry
#ifdef __SYMBIAN32__
#include <e32std.h>
#endif

#ifdef IOS
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#endif

#ifdef BLACKBERRY
#include <sys/mman.h>
#endif

// __FUNCTION__ is misused a lot below, it's no longer a string literal but a virtual
// variable so this use fails in some compilers. Just define it away for now.
#ifndef _MSC_VER
#define __FUNCTION__ "(n/a)"
#endif

namespace ArmGen
{

inline u32 RotR(u32 a, int amount) {
	if (!amount) return a;
	return (a >> amount) | (a << (32 - amount));
}

inline u32 RotL(u32 a, int amount) {
	if (!amount) return a;
	return (a << amount) | (a >> (32 - amount));
}

bool TryMakeOperand2(u32 imm, Operand2 &op2) {
	// Just brute force it.
	for (int i = 0; i < 16; i++) {
		int mask = RotR(0xFF, i * 2);
		if ((imm & mask) == imm) {
			op2 = Operand2((u8)(RotL(imm, i * 2)), (u8)i);
			return true;
		}
	}
	return false;
}

bool TryMakeOperand2_AllowInverse(u32 imm, Operand2 &op2, bool *inverse)
{
	if (!TryMakeOperand2(imm, op2)) {
		*inverse = true;
		return TryMakeOperand2(~imm, op2);
	} else {
		*inverse = false;
		return true;
	}
}

bool TryMakeOperand2_AllowNegation(s32 imm, Operand2 &op2, bool *negated)
{
	if (!TryMakeOperand2(imm, op2)) {
		*negated = true;
		return TryMakeOperand2(-imm, op2);
	} else {
		*negated = false;
		return true;
	}
}

Operand2 AssumeMakeOperand2(u32 imm) {
	Operand2 op2;
	bool result = TryMakeOperand2(imm, op2);
	_dbg_assert_msg_(JIT, result, "Could not make assumed Operand2.");
	if (!result) {
		// Make double sure that we get it logged.
		ERROR_LOG(JIT, "Could not make assumed Operand2.");
	}
	return op2;
}

bool ARMXEmitter::TrySetValue_TwoOp(ARMReg reg, u32 val)
{
	int ops = 0;
	for (int i = 0; i < 16; i++)
	{
		if ((val >> (i*2)) & 0x3)
		{
			ops++;
			i+=3;
		}
	}
	if (ops > 2)
		return false;
	
	bool first = true;
	for (int i = 0; i < 16; i++, val >>=2) {
		if (val & 0x3) {
			first ? MOV(reg, Operand2((u8)val, (u8)((16-i) & 0xF)))
				  : ORR(reg, reg, Operand2((u8)val, (u8)((16-i) & 0xF)));
			first = false;
			i+=3;
			val >>= 6;
		}
	}
	return true;
}

void ARMXEmitter::MOVI2F(ARMReg dest, float val, ARMReg tempReg, bool negate)
{
	union {float f; u32 u;} conv;
	conv.f = negate ? -val : val;
	// Try moving directly first if mantisse is empty
	if (cpu_info.bVFPv3 && ((conv.u & 0x7FFFF) == 0))
	{
		// VFP Encoding for Imms: <7> Not(<6>) Repeat(<6>,5) <5:0> Zeros(19)
		bool bit6 = (conv.u & 0x40000000) == 0x40000000;
		bool canEncode = true;
		for (u32 mask = 0x20000000; mask >= 0x02000000; mask >>= 1)
		{
			if (((conv.u & mask) == mask) == bit6)
				canEncode = false;
		}
		if (canEncode)
		{
			u32 imm8 = (conv.u & 0x80000000) >> 24; // sign bit
			imm8 |= (!bit6 << 6);
			imm8 |= (conv.u & 0x01F80000) >> 19;
			VMOV(dest, IMM(imm8));
			return;
		}
	}
	MOVI2R(tempReg, conv.u);
	VMOV(dest, tempReg);
	// Otherwise, possible to use a literal pool and VLDR directly (+- 1020)
}

void ARMXEmitter::ADDI2R(ARMReg rd, ARMReg rs, u32 val, ARMReg scratch)
{
	if (val == 0) {
		if (rd != rs)
			MOV(rd, rs);
		return;
	}
	Operand2 op2;
	bool negated;
	if (TryMakeOperand2_AllowNegation(val, op2, &negated)) {
		if (!negated)
			ADD(rd, rs, op2);
		else
			SUB(rd, rs, op2);
	} else {
		// Try 16-bit additions and subtractions - easy to test for.
		// Should also try other rotations...
		if ((val & 0xFFFF0000) == 0) {
			// Decompose into two additions.
			ADD(rd, rs, Operand2((u8)(val >> 8), 12));   // rotation right by 12*2 == rotation left by 8
			ADD(rd, rd, Operand2((u8)(val), 0));
		} else if ((((u32)-(s32)val) & 0xFFFF0000) == 0) {
			val = (u32)-(s32)val;
			SUB(rd, rs, Operand2((u8)(val >> 8), 12));
			SUB(rd, rd, Operand2((u8)(val), 0));
		} else {
			MOVI2R(scratch, val);
			ADD(rd, rs, scratch);
		}
	}
}

void ARMXEmitter::ANDI2R(ARMReg rd, ARMReg rs, u32 val, ARMReg scratch)
{
	Operand2 op2;
	bool inverse;
	if (val == 0) {
		// Avoid the ALU, may improve pipeline.
		MOV(rd, 0);
	} else if (TryMakeOperand2_AllowInverse(val, op2, &inverse)) {
		if (!inverse) {
			AND(rd, rs, op2);
		} else {
			BIC(rd, rs, op2);
		}
	} else {
		int ops = 0;
		for (int i = 0; i < 32; i += 2) {
			u8 bits = RotR(val, i) & 0xFF;
			// If either low bit is not set, we need to use a BIC for them.
			if ((bits & 3) != 3) {
				++ops;
				i += 8 - 2;
			}
		}

		// The worst case is 4 (e.g. 0x55555555.)
#ifdef HAVE_ARMV7
		if (ops > 3) {
			MOVI2R(scratch, val);
			AND(rd, rs, scratch);
		} else
#endif
		{
			bool first = true;
			for (int i = 0; i < 32; i += 2) {
				u8 bits = RotR(val, i) & 0xFF;
				if ((bits & 3) != 3) {
					u8 rotation = i == 0 ? 0 : 16 - i / 2;
					if (first) {
						BIC(rd, rs, Operand2(~bits, rotation));
						first = false;
					} else {
						BIC(rd, rd, Operand2(~bits, rotation));
					}
					// Well, we took care of these other bits while we were at it.
					i += 8 - 2;
				}
			}
		}
	}
}

void ARMXEmitter::CMPI2R(ARMReg rs, u32 val, ARMReg scratch)
{
	Operand2 op2;
	bool negated;
	if (TryMakeOperand2_AllowNegation(val, op2, &negated)) {
		if (!negated)
			CMP(rs, op2);
		else
			CMN(rs, op2);
	} else {
		MOVI2R(scratch, val);
		CMP(rs, scratch);
	}
}

void ARMXEmitter::TSTI2R(ARMReg rs, u32 val, ARMReg scratch)
{
	Operand2 op2;
	if (TryMakeOperand2(val, op2)) {
		TST(rs, op2);
	} else {
		MOVI2R(scratch, val);
		TST(rs, scratch);
	}
}

void ARMXEmitter::ORI2R(ARMReg rd, ARMReg rs, u32 val, ARMReg scratch)
{
	Operand2 op2;
	if (val == 0) {
		// Avoid the ALU, may improve pipeline.
		if (rd != rs) {
			MOV(rd, rs);
		}
	} else if (TryMakeOperand2(val, op2)) {
		ORR(rd, rs, op2);
	} else {
		int ops = 0;
		for (int i = 0; i < 32; i += 2) {
			u8 bits = RotR(val, i) & 0xFF;
			// If either low bit is set, we need to use a ORR for them.
			if ((bits & 3) != 0) {
				++ops;
				i += 8 - 2;
			}
		}

		// The worst case is 4 (e.g. 0x55555555.)  But MVN can make it 2.  Not sure if better.
		bool inversed;
		if (TryMakeOperand2_AllowInverse(val, op2, &inversed) && ops >= 3) {
			MVN(scratch, op2);
			ORR(rd, rs, scratch);
#ifdef HAVE_ARMV7
		} else if (ops > 3) {
			MOVI2R(scratch, val);
			ORR(rd, rs, scratch);
#endif
		} else {
			bool first = true;
			for (int i = 0; i < 32; i += 2) {
				u8 bits = RotR(val, i) & 0xFF;
				if ((bits & 3) != 0) {
					u8 rotation = i == 0 ? 0 : 16 - i / 2;
					if (first) {
						ORR(rd, rs, Operand2(bits, rotation));
						first = false;
					} else {
						ORR(rd, rd, Operand2(bits, rotation));
					}
					// Well, we took care of these other bits while we were at it.
					i += 8 - 2;
				}
			}
		}
	}
}

void ARMXEmitter::FlushLitPool()
{
	for(std::vector<LiteralPool>::iterator it = currentLitPool.begin(); it != currentLitPool.end(); ++it) {
		// Search for duplicates
		for(std::vector<LiteralPool>::iterator old_it = currentLitPool.begin(); old_it != it; ++old_it) {
			if ((*old_it).val == (*it).val)
				(*it).loc = (*old_it).loc;
		}

		// Write the constant to Literal Pool
		if (!(*it).loc)
		{
			(*it).loc = (intptr_t)code;
			Write32((*it).val);
		}
		intptr_t offset = (*it).loc - (intptr_t)(*it).ldr_address - 8;

		// Backpatch the LDR
		*(u32*)(*it).ldr_address |= (offset >= 0) << 23 | abs(offset);
	}
	// TODO: Save a copy of previous pools in case they are still in range.
	currentLitPool.clear();
}

void ARMXEmitter::AddNewLit(u32 val)
{
	LiteralPool pool_item;
	pool_item.loc = 0;
	pool_item.val = val;
	pool_item.ldr_address = code;
	currentLitPool.push_back(pool_item);
}

void ARMXEmitter::MOVI2R(ARMReg reg, u32 val, bool optimize)
{
	Operand2 op2;
	bool inverse;

#ifdef HAVE_ARMV7
	// Unused
	if (!optimize)
	{
		// For backpatching on ARMv7
		MOVW(reg, val & 0xFFFF);
		MOVT(reg, val, true);
		return;
	}
#endif

	if (TryMakeOperand2_AllowInverse(val, op2, &inverse)) {
		inverse ? MVN(reg, op2) : MOV(reg, op2);
	} else {
#ifdef HAVE_ARMV7
		// Use MOVW+MOVT for ARMv7+
		MOVW(reg, val & 0xFFFF);
		if(val & 0xFFFF0000)
			MOVT(reg, val, true);
#else
		if (!TrySetValue_TwoOp(reg,val)) {
			bool first = true;
			for (int i = 0; i < 32; i += 2) {
				u8 bits = RotR(val, i) & 0xFF;
				if ((bits & 3) != 0) {
					u8 rotation = i == 0 ? 0 : 16 - i / 2;
					if (first) {
						MOV(reg, Operand2(bits, rotation));
						first = false;
					} else {
						ORR(reg, reg, Operand2(bits, rotation));
					}
					// Well, we took care of these other bits while we were at it.
					i += 8 - 2;
				}
			}
			// Use literal pool for ARMv6.
			// Disabled for now as it is crashing since Vertex Decoder JIT
//			AddNewLit(val);
//			LDR(reg, _PC); // To be backpatched later
		}
#endif
	}
}

void ARMXEmitter::QuickCallFunction(ARMReg reg, void *func) {
	if (BLInRange(func)) {
		BL(func);
	} else {
		MOVI2R(reg, (u32)(uintptr_t)(func));
		BL(reg);
	}
}

void ARMXEmitter::SetCodePtr(u8 *ptr)
{
	code = ptr;
	startcode = code;
	lastCacheFlushEnd = ptr;
}

const u8 *ARMXEmitter::GetCodePtr() const
{
	return code;
}

u8 *ARMXEmitter::GetWritableCodePtr()
{
	return code;
}

void ARMXEmitter::ReserveCodeSpace(u32 bytes)
{
	for (u32 i = 0; i < bytes/4; i++)
		Write32(0xE1200070); //bkpt 0
}

const u8 *ARMXEmitter::AlignCode16()
{
	ReserveCodeSpace((-(intptr_t)code) & 15);
	return code;
}

const u8 *ARMXEmitter::AlignCodePage()
{
	ReserveCodeSpace((-(intptr_t)code) & 4095);
	return code;
}

void ARMXEmitter::FlushIcache()
{
	FlushIcacheSection(lastCacheFlushEnd, code);
	lastCacheFlushEnd = code;
}

void ARMXEmitter::FlushIcacheSection(u8 *start, u8 *end)
{
#ifdef __SYMBIAN32__
	User::IMB_Range(start, end);
#elif defined(BLACKBERRY)
	msync(start, end - start, MS_SYNC | MS_INVALIDATE_ICACHE);
#elif defined(IOS)
	// Header file says this is equivalent to: sys_icache_invalidate(start, end - start);
	sys_cache_control(kCacheFunctionPrepareForExecution, start, end - start);
#elif !defined(_WIN32)
#if defined(ARM)
#ifdef __clang__
	__clear_cache(start, end);
#else
	__builtin___clear_cache(start, end);
#endif
#endif
#endif
}

void ARMXEmitter::SetCC(CCFlags cond)
{
	condition = cond << 28;
}

void ARMXEmitter::NOP(int count)
{
	for (int i = 0; i < count; i++) {
		Write32(condition | 0x01A00000);
	}
}

void ARMXEmitter::SETEND(bool BE)
{
	//SETEND is non-conditional
	Write32( 0xF1010000 | (BE << 9));
}
void ARMXEmitter::BKPT(u16 arg)
{
	Write32(condition | 0x01200070 | (arg << 4 & 0x000FFF00) | (arg & 0x0000000F));
}
void ARMXEmitter::YIELD()
{
	Write32(condition | 0x0320F001);
}

FixupBranch ARMXEmitter::B()
{
	FixupBranch branch;
	branch.type = 0; // Zero for B
	branch.ptr = code;
	branch.condition = condition;
	//We'll write NOP here for now.
	Write32(condition | 0x01A00000);
	return branch;
}
FixupBranch ARMXEmitter::BL()
{
	FixupBranch branch;
	branch.type = 1; // Zero for B
	branch.ptr = code;
	branch.condition = condition;
	//We'll write NOP here for now.
	Write32(condition | 0x01A00000);
	return branch;
}

FixupBranch ARMXEmitter::B_CC(CCFlags Cond)
{
	FixupBranch branch;
	branch.type = 0; // Zero for B
	branch.ptr = code;
	branch.condition = Cond << 28;
	//We'll write NOP here for now.
	Write32(condition | 0x01A00000);
	return branch;
}
void ARMXEmitter::B_CC(CCFlags Cond, const void *fnptr)
{
	ptrdiff_t distance = (intptr_t)fnptr - ((intptr_t)(code) + 8);
	_assert_msg_(JIT, distance > -33554432
                     && distance <=  33554432,
                     "B_CC out of range (%p calls %p)", code, fnptr);

	Write32((Cond << 28) | 0x0A000000 | ((distance >> 2) & 0x00FFFFFF));
}
FixupBranch ARMXEmitter::BL_CC(CCFlags Cond)
{
	FixupBranch branch;
	branch.type = 1; // Zero for B
	branch.ptr = code;
	branch.condition = Cond << 28;
	//We'll write NOP here for now.
	Write32(condition | 0x01A00000);
	return branch;
}
void ARMXEmitter::SetJumpTarget(FixupBranch const &branch)
{
	ptrdiff_t distance =  ((intptr_t)(code) - 8)  - (intptr_t)branch.ptr;
	_assert_msg_(JIT, distance > -33554432
                     && distance <=  33554432,
                     "SetJumpTarget out of range (%p calls %p)", code,
					 branch.ptr);
	if(branch.type == 0) // B
		*(u32*)branch.ptr = (u32)(branch.condition | (10 << 24) | ((distance >> 2) &
		0x00FFFFFF)); 
	else // BL
		*(u32*)branch.ptr =	(u32)(branch.condition | 0x0B000000 | ((distance >> 2)
		& 0x00FFFFFF));
}
void ARMXEmitter::B (const void *fnptr)
{
	ptrdiff_t distance = (intptr_t)fnptr - (intptr_t(code) + 8);
	_assert_msg_(JIT, distance > -33554432
                     && distance <=  33554432,
                     "B out of range (%p calls %p)", code, fnptr);

	Write32(condition | 0x0A000000 | ((distance >> 2) & 0x00FFFFFF));
}

void ARMXEmitter::B(ARMReg src)
{
	Write32(condition | 0x012FFF10 | src);
}

bool ARMXEmitter::BLInRange(const void *fnptr) {
	ptrdiff_t distance = (intptr_t)fnptr - (intptr_t(code) + 8);
	if (distance <= -33554432 || distance > 33554432)
		return false;
	else
		return true;
}

void ARMXEmitter::BL(const void *fnptr)
{
	ptrdiff_t distance = (intptr_t)fnptr - (intptr_t(code) + 8);
	_assert_msg_(JIT, distance > -33554432
                     && distance <=  33554432,
                     "BL out of range (%p calls %p)", code, fnptr);
	Write32(condition | 0x0B000000 | ((distance >> 2) & 0x00FFFFFF));
}
void ARMXEmitter::BL(ARMReg src)
{
	Write32(condition | 0x012FFF30 | src);
}
void ARMXEmitter::PUSH(const int num, ...)
{
	u16 RegList = 0;
	u8 Reg;
	int i;
	va_list vl;
	va_start(vl, num);
	for (i=0;i<num;i++)
	{
		Reg = va_arg(vl, u32);
		RegList |= (1 << Reg);
	}
	va_end(vl);
	Write32(condition | (2349 << 16) | RegList);
}
void ARMXEmitter::POP(const int num, ...)
{
	u16 RegList = 0;
	u8 Reg;
	int i;
	va_list vl;
	va_start(vl, num);
	for (i=0;i<num;i++)
	{
		Reg = va_arg(vl, u32);
		RegList |= (1 << Reg);
	}
	va_end(vl);
	Write32(condition | (2237 << 16) | RegList);
}

void ARMXEmitter::WriteShiftedDataOp(u32 op, bool SetFlags, ARMReg dest, ARMReg src, Operand2 op2)
{
	Write32(condition | (13 << 21) | (SetFlags << 20) | (dest << 12) | op2.Imm5() | (op << 4) | src);
}
void ARMXEmitter::WriteShiftedDataOp(u32 op, bool SetFlags, ARMReg dest, ARMReg src, ARMReg op2)
{
	Write32(condition | (13 << 21) | (SetFlags << 20) | (dest << 12) | (op2 << 8) | (op << 4) | src);
}

// IMM, REG, IMMSREG, RSR 
// -1 for invalid if the instruction doesn't support that
const s32 InstOps[][4] = {{16, 0, 0, 0}, // AND(s)
						  {17, 1, 1, 1}, // EOR(s)
						  {18, 2, 2, 2}, // SUB(s)
						  {19, 3, 3, 3}, // RSB(s)
						  {20, 4, 4, 4}, // ADD(s)
						  {21, 5, 5, 5}, // ADC(s)
						  {22, 6, 6, 6}, // SBC(s)
						  {23, 7, 7, 7}, // RSC(s)
						  {24, 8, 8, 8}, // TST
						  {25, 9, 9, 9}, // TEQ
						  {26, 10, 10, 10}, // CMP
						  {27, 11, 11, 11}, // CMN
						  {28, 12, 12, 12}, // ORR(s)
						  {29, 13, 13, 13}, // MOV(s)
						  {30, 14, 14, 14}, // BIC(s)
						  {31, 15, 15, 15}, // MVN(s)
						  {24, -1, -1, -1}, // MOVW
						  {26, -1, -1, -1}, // MOVT
						 }; 

const char *InstNames[] = { "AND",
							"EOR",
							"SUB",
							"RSB",
							"ADD",
							"ADC",
							"SBC",
							"RSC",
							"TST",
							"TEQ",
							"CMP",
							"CMN",
							"ORR",
							"MOV",
							"BIC",
							"MVN"
						  };

void ARMXEmitter::AND (ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(0, Rd, Rn, Rm); }
void ARMXEmitter::ANDS(ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(0, Rd, Rn, Rm, true); }
void ARMXEmitter::EOR (ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(1, Rd, Rn, Rm); }
void ARMXEmitter::EORS(ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(1, Rd, Rn, Rm, true); }
void ARMXEmitter::SUB (ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(2, Rd, Rn, Rm); }
void ARMXEmitter::SUBS(ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(2, Rd, Rn, Rm, true); }
void ARMXEmitter::RSB (ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(3, Rd, Rn, Rm); }
void ARMXEmitter::RSBS(ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(3, Rd, Rn, Rm, true); }
void ARMXEmitter::ADD (ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(4, Rd, Rn, Rm); }
void ARMXEmitter::ADDS(ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(4, Rd, Rn, Rm, true); }
void ARMXEmitter::ADC (ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(5, Rd, Rn, Rm); }
void ARMXEmitter::ADCS(ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(5, Rd, Rn, Rm, true); }
void ARMXEmitter::SBC (ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(6, Rd, Rn, Rm); }
void ARMXEmitter::SBCS(ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(6, Rd, Rn, Rm, true); }
void ARMXEmitter::RSC (ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(7, Rd, Rn, Rm); }
void ARMXEmitter::RSCS(ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(7, Rd, Rn, Rm, true); }
void ARMXEmitter::TST (			  ARMReg Rn, Operand2 Rm) { WriteInstruction(8, R0, Rn, Rm, true); }
void ARMXEmitter::TEQ (			  ARMReg Rn, Operand2 Rm) { WriteInstruction(9, R0, Rn, Rm, true); }
void ARMXEmitter::CMP (			  ARMReg Rn, Operand2 Rm) { WriteInstruction(10, R0, Rn, Rm, true); }
void ARMXEmitter::CMN (			  ARMReg Rn, Operand2 Rm) { WriteInstruction(11, R0, Rn, Rm, true); }
void ARMXEmitter::ORR (ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(12, Rd, Rn, Rm); }
void ARMXEmitter::ORRS(ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(12, Rd, Rn, Rm, true); }
void ARMXEmitter::MOV (ARMReg Rd,			 Operand2 Rm) { WriteInstruction(13, Rd, R0, Rm); }
void ARMXEmitter::MOVS(ARMReg Rd,			 Operand2 Rm) { WriteInstruction(13, Rd, R0, Rm, true); }
void ARMXEmitter::BIC (ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(14, Rd, Rn, Rm); }
void ARMXEmitter::BICS(ARMReg Rd, ARMReg Rn, Operand2 Rm) { WriteInstruction(14, Rd, Rn, Rm, true); }
void ARMXEmitter::MVN (ARMReg Rd,			 Operand2 Rm) { WriteInstruction(15, Rd, R0, Rm); }
void ARMXEmitter::MVNS(ARMReg Rd,			 Operand2 Rm) { WriteInstruction(15, Rd, R0, Rm, true); }
void ARMXEmitter::MOVW(ARMReg Rd,			 Operand2 Rm) { WriteInstruction(16, Rd, R0, Rm); }
void ARMXEmitter::MOVT(ARMReg Rd, Operand2 Rm, bool TopBits) { WriteInstruction(17, Rd, R0, TopBits ? Rm.Value >> 16 : Rm); }

void ARMXEmitter::WriteInstruction (u32 Op, ARMReg Rd, ARMReg Rn, Operand2 Rm, bool SetFlags) // This can get renamed later
{
	s32 op = InstOps[Op][Rm.GetType()]; // Type always decided by last operand
	u32 Data = Rm.GetData();
	if (Rm.GetType() == TYPE_IMM)
	{
		switch (Op)
		{
			// MOV cases that support IMM16
			case 16:
			case 17:
				Data = Rm.Imm16();
			break;
			default:
			break;
		}
	}
	if (op == -1)
		_assert_msg_(JIT, false, "%s not yet support %d", InstNames[Op], Rm.GetType()); 
	Write32(condition | (op << 21) | (SetFlags ? (1 << 20) : 0) | Rn << 16 | Rd << 12 | Data);
}

// Data Operations
void ARMXEmitter::WriteSignedMultiply(u32 Op, u32 Op2, u32 Op3, ARMReg dest, ARMReg r1, ARMReg r2)
{
	Write32(condition | (0x7 << 24) | (Op << 20) | (dest << 16) | (Op2 << 12) | (r1 << 8) | (Op3 << 5) | (1 << 4) | r2);
}
void ARMXEmitter::UDIV(ARMReg dest, ARMReg dividend, ARMReg divisor)
{
	if (!cpu_info.bIDIVa)
		PanicAlert("Trying to use integer divide on hardware that doesn't support it. Bad programmer.");
	WriteSignedMultiply(3, 0xF, 0, dest, divisor, dividend);
}
void ARMXEmitter::SDIV(ARMReg dest, ARMReg dividend, ARMReg divisor)
{
	if (!cpu_info.bIDIVa)
		PanicAlert("Trying to use integer divide on hardware that doesn't support it. Bad programmer.");
	WriteSignedMultiply(1, 0xF, 0, dest, divisor, dividend);
}

void ARMXEmitter::LSL (ARMReg dest, ARMReg src, Operand2 op2) { WriteShiftedDataOp(0, false, dest, src, op2);}
void ARMXEmitter::LSLS(ARMReg dest, ARMReg src, Operand2 op2) { WriteShiftedDataOp(0, true, dest, src, op2);}
void ARMXEmitter::LSL (ARMReg dest, ARMReg src, ARMReg op2)   { WriteShiftedDataOp(1, false, dest, src, op2);} 
void ARMXEmitter::LSLS(ARMReg dest, ARMReg src, ARMReg op2)   { WriteShiftedDataOp(1, true, dest, src, op2);}
void ARMXEmitter::LSR (ARMReg dest, ARMReg src, Operand2 op2) {
	_assert_msg_(JIT, op2.GetType() != TYPE_IMM || op2.Imm5() != 0, "LSR must have a non-zero shift (use LSL.)"); 
	WriteShiftedDataOp(2, false, dest, src, op2);
}
void ARMXEmitter::LSRS(ARMReg dest, ARMReg src, Operand2 op2) {
	_assert_msg_(JIT, op2.GetType() != TYPE_IMM || op2.Imm5() != 0, "LSRS must have a non-zero shift (use LSLS.)");
	WriteShiftedDataOp(2, true, dest, src, op2);
}
void ARMXEmitter::LSR (ARMReg dest, ARMReg src, ARMReg op2)   { WriteShiftedDataOp(3, false, dest, src, op2);}
void ARMXEmitter::LSRS(ARMReg dest, ARMReg src, ARMReg op2)   { WriteShiftedDataOp(3, true, dest, src, op2);}
void ARMXEmitter::ASR (ARMReg dest, ARMReg src, Operand2 op2) {
	_assert_msg_(JIT, op2.GetType() != TYPE_IMM || op2.Imm5() != 0, "ASR must have a non-zero shift (use LSL.)");
	WriteShiftedDataOp(4, false, dest, src, op2);
}
void ARMXEmitter::ASRS(ARMReg dest, ARMReg src, Operand2 op2) {
	_assert_msg_(JIT, op2.GetType() != TYPE_IMM || op2.Imm5() != 0, "ASRS must have a non-zero shift (use LSLS.)");
	WriteShiftedDataOp(4, true, dest, src, op2);
}
void ARMXEmitter::ASR (ARMReg dest, ARMReg src, ARMReg op2)   { WriteShiftedDataOp(5, false, dest, src, op2);}
void ARMXEmitter::ASRS(ARMReg dest, ARMReg src, ARMReg op2)   { WriteShiftedDataOp(5, true, dest, src, op2);}

void ARMXEmitter::MUL (ARMReg dest,	ARMReg src, ARMReg op2)
{
	Write32(condition | (dest << 16) | (src << 8) | (9 << 4) | op2);
}
void ARMXEmitter::MULS(ARMReg dest,	ARMReg src, ARMReg op2)
{
	Write32(condition | (1 << 20) | (dest << 16) | (src << 8) | (9 << 4) | op2);
}

void ARMXEmitter::Write4OpMultiply(u32 op, ARMReg destLo, ARMReg destHi, ARMReg rm, ARMReg rn) {
	Write32(condition | (op << 20) | (destHi << 16) | (destLo << 12) | (rm << 8) | (9 << 4) | rn);
}

void ARMXEmitter::UMULL(ARMReg destLo, ARMReg destHi, ARMReg rm, ARMReg rn)
{
	Write4OpMultiply(0x8, destLo, destHi, rn, rm);
}

void ARMXEmitter::SMULL(ARMReg destLo, ARMReg destHi, ARMReg rm, ARMReg rn)
{
	Write4OpMultiply(0xC, destLo, destHi, rn, rm);
}

void ARMXEmitter::UMLAL(ARMReg destLo, ARMReg destHi, ARMReg rm, ARMReg rn)
{
	Write4OpMultiply(0xA, destLo, destHi, rn, rm);
}

void ARMXEmitter::SMLAL(ARMReg destLo, ARMReg destHi, ARMReg rm, ARMReg rn)
{
	Write4OpMultiply(0xE, destLo, destHi, rn, rm);
}

void ARMXEmitter::UBFX(ARMReg dest, ARMReg rn, u8 lsb, u8 width)
{
	Write32(condition | (0x7E0 << 16) | ((width - 1) << 16) | (dest << 12) | (lsb << 7) | (5 << 4) | rn);
}

void ARMXEmitter::CLZ(ARMReg rd, ARMReg rm)
{
	Write32(condition | (0x16F << 16) | (rd << 12) | (0xF1 << 4) | rm);
}

void ARMXEmitter::PLD(ARMReg rn, int offset, bool forWrite) {
	_dbg_assert_msg_(JIT, offset < 0x3ff && offset > -0x3ff, "PLD: Max 12 bits of offset allowed");

	bool U = offset >= 0;
	if (offset < 0) offset = -offset;
	bool R = !forWrite;
	// Conditions not allowed
	Write32((0xF5 << 24) | (U << 23) | (R << 22) | (1 << 20) | ((int)rn << 16) | (0xF << 12) | offset);
}


void ARMXEmitter::BFI(ARMReg rd, ARMReg rn, u8 lsb, u8 width)
{
	u32 msb = (lsb + width - 1);
	if (msb > 31) msb = 31;
	Write32(condition | (0x7C0 << 16) | (msb << 16) | (rd << 12) | (lsb << 7) | (1 << 4) | rn);
}

void ARMXEmitter::SXTB (ARMReg dest, ARMReg op2)
{
	Write32(condition | (0x6AF << 16) | (dest << 12) | (7 << 4) | op2);
}

void ARMXEmitter::SXTH (ARMReg dest, ARMReg op2, u8 rotation)
{
	SXTAH(dest, (ARMReg)15, op2, rotation);
}
void ARMXEmitter::SXTAH(ARMReg dest, ARMReg src, ARMReg op2, u8 rotation) 
{
	// bits ten and 11 are the rotation amount, see 8.8.232 for more
	// information
	Write32(condition | (0x6B << 20) | (src << 16) | (dest << 12) | (rotation << 10) | (7 << 4) | op2);
}
void ARMXEmitter::RBIT(ARMReg dest, ARMReg src)
{
	Write32(condition | (0x6F << 20) | (0xF << 16) | (dest << 12) | (0xF3 << 4) | src);
}
void ARMXEmitter::REV (ARMReg dest, ARMReg src) 
{
	Write32(condition | (0x6BF << 16) | (dest << 12) | (0xF3 << 4) | src);
}
void ARMXEmitter::REV16(ARMReg dest, ARMReg src)
{
	Write32(condition | (0x6BF << 16) | (dest << 12) | (0xFB << 4) | src);
}

void ARMXEmitter::_MSR (bool write_nzcvq, bool write_g,		Operand2 op2)
{
	Write32(condition | (0x320F << 12) | (write_nzcvq << 19) | (write_g << 18) | op2.Imm12Mod());
}
void ARMXEmitter::_MSR (bool write_nzcvq, bool write_g,		ARMReg src)
{
	Write32(condition | (0x120F << 12) | (write_nzcvq << 19) | (write_g << 18) | src);
}
void ARMXEmitter::MRS (ARMReg dest)
{
	Write32(condition | (16 << 20) | (15 << 16) | (dest << 12));
}
void ARMXEmitter::LDREX(ARMReg dest, ARMReg base)
{
	Write32(condition | (25 << 20) | (base << 16) | (dest << 12) | 0xF9F);
}
void ARMXEmitter::STREX(ARMReg result, ARMReg base, ARMReg op)
{
	_assert_msg_(JIT, (result != base && result != op), "STREX dest can't be other two registers");
	Write32(condition | (24 << 20) | (base << 16) | (result << 12) | (0xF9 << 4) | op);
}
void ARMXEmitter::DMB ()
{
	Write32(0xF57FF05E);
}
void ARMXEmitter::SVC(Operand2 op)
{
	Write32(condition | (0x0F << 24) | op.Imm24());
}

// IMM, REG, IMMSREG, RSR
// -1 for invalid if the instruction doesn't support that
const s32 LoadStoreOps[][4] = {
	{0x40, 0x60, 0x60, -1}, // STR
	{0x41, 0x61, 0x61, -1}, // LDR
	{0x44, 0x64, 0x64, -1}, // STRB
	{0x45, 0x65, 0x65, -1}, // LDRB
	// Special encodings
	{ 0x4,  0x0,  -1, -1}, // STRH
	{ 0x5,  0x1,  -1, -1}, // LDRH
	{ 0x5,  0x1,  -1, -1}, // LDRSB
	{ 0x5,  0x1,  -1, -1}, // LDRSH
};
const char *LoadStoreNames[] = {
	"STR",
	"LDR",
	"STRB",
	"LDRB",
	"STRH",
	"LDRH",
	"LDRSB",
	"LDRSH",
};

void ARMXEmitter::WriteStoreOp(u32 Op, ARMReg Rt, ARMReg Rn, Operand2 Rm, bool RegAdd)
{
	s32 op = LoadStoreOps[Op][Rm.GetType()]; // Type always decided by last operand
	u32 Data;

	// Qualcomm chipsets get /really/ angry if you don't use index, even if the offset is zero.
	// Some of these encodings require Index at all times anyway. Doesn't really matter.
	// bool Index = op2 != 0 ? true : false;
	bool Index = true;
	bool Add = false;

	// Special Encoding (misc addressing mode)
	bool SpecialOp = false;
	bool Half = false;
	bool SignedLoad = false;

	if (op == -1)
		_assert_msg_(JIT, false, "%s does not support %d", LoadStoreNames[Op], Rm.GetType()); 

	switch (Op)
	{
		case 4: // STRH
			SpecialOp = true;
			Half = true;
			SignedLoad = false;
		break;
		case 5: // LDRH
			SpecialOp = true;
			Half = true;
			SignedLoad = false;
		break;
		case 6: // LDRSB
			SpecialOp = true;
			Half = false;
			SignedLoad = true;
		break;
		case 7: // LDRSH
			SpecialOp = true;
			Half = true;
			SignedLoad = true;
		break;
	}
	switch (Rm.GetType())
	{
		case TYPE_IMM:
		{
			s32 Temp = (s32)Rm.Value;
			Data = abs(Temp);
			// The offset is encoded differently on this one.
			if (SpecialOp)
				Data = ((Data & 0xF0) << 4) | (Data & 0xF);
			if (Temp >= 0) Add = true;
		}
		break;
		case TYPE_REG:
			Data = Rm.GetData();
			Add = RegAdd;
			break;
		case TYPE_IMMSREG:
			if (!SpecialOp)
			{
				Data = Rm.GetData();
				Add = RegAdd;
				break;
			}
			// Intentional fallthrough: TYPE_IMMSREG not supported for misc addressing.
		default:
			// RSR not supported for any of these
			// We already have the warning above
			BKPT(0x2);
			return;
		break;
	}
	if (SpecialOp)
	{
		// Add SpecialOp things
		Data = (0x9 << 4) | (SignedLoad << 6) | (Half << 5) | Data;
	}
	Write32(condition | (op << 20) | (Index << 24) | (Add << 23) | (Rn << 16) | (Rt << 12) | Data);
}

void ARMXEmitter::LDR (ARMReg dest, ARMReg base, Operand2 op2, bool RegAdd) { WriteStoreOp(1, dest, base, op2, RegAdd);}
void ARMXEmitter::LDRB(ARMReg dest, ARMReg base, Operand2 op2, bool RegAdd) { WriteStoreOp(3, dest, base, op2, RegAdd);}
void ARMXEmitter::LDRH(ARMReg dest, ARMReg base, Operand2 op2, bool RegAdd) { WriteStoreOp(5, dest, base, op2, RegAdd);}
void ARMXEmitter::LDRSB(ARMReg dest, ARMReg base, Operand2 op2, bool RegAdd) { WriteStoreOp(6, dest, base, op2, RegAdd);}
void ARMXEmitter::LDRSH(ARMReg dest, ARMReg base, Operand2 op2, bool RegAdd) { WriteStoreOp(7, dest, base, op2, RegAdd);}
void ARMXEmitter::STR  (ARMReg result, ARMReg base, Operand2 op2, bool RegAdd) { WriteStoreOp(0, result, base, op2, RegAdd);}
void ARMXEmitter::STRH (ARMReg result, ARMReg base, Operand2 op2, bool RegAdd) { WriteStoreOp(4, result, base, op2, RegAdd);}
void ARMXEmitter::STRB (ARMReg result, ARMReg base, Operand2 op2, bool RegAdd) { WriteStoreOp(2, result, base, op2, RegAdd);}

#define VA_TO_REGLIST(RegList, Regnum) \
{ \
	u8 Reg; \
	va_list vl; \
	va_start(vl, Regnum); \
	for (int i = 0; i < Regnum; i++) \
	{ \
		Reg = va_arg(vl, u32); \
		RegList |= (1 << Reg); \
	} \
	va_end(vl); \
}

void ARMXEmitter::WriteRegStoreOp(u32 op, ARMReg dest, bool WriteBack, u16 RegList)
{
	Write32(condition | (op << 20) | (WriteBack << 21) | (dest << 16) | RegList);
}
void ARMXEmitter::STMFD(ARMReg dest, bool WriteBack, const int Regnum, ...)
{
	u16 RegList = 0;
	VA_TO_REGLIST(RegList, Regnum);
	WriteRegStoreOp(0x80 | 0x10 | 0, dest, WriteBack, RegList);
}
void ARMXEmitter::LDMFD(ARMReg dest, bool WriteBack, const int Regnum, ...)
{
	u16 RegList = 0;
	VA_TO_REGLIST(RegList, Regnum);
	WriteRegStoreOp(0x80 | 0x08 | 1, dest, WriteBack, RegList);
}
void ARMXEmitter::STMIA(ARMReg dest, bool WriteBack, const int Regnum, ...)
{
	u16 RegList = 0;
	VA_TO_REGLIST(RegList, Regnum);
	WriteRegStoreOp(0x80 | 0x08 | 0, dest, WriteBack, RegList);
}
void ARMXEmitter::LDMIA(ARMReg dest, bool WriteBack, const int Regnum, ...)
{
	u16 RegList = 0;
	VA_TO_REGLIST(RegList, Regnum);
	WriteRegStoreOp(0x80 | 0x08 | 1, dest, WriteBack, RegList);
}
void ARMXEmitter::STM(ARMReg dest, bool Add, bool Before, bool WriteBack, const int Regnum, ...)
{
	u16 RegList = 0;
	VA_TO_REGLIST(RegList, Regnum);
	WriteRegStoreOp(0x80 | (Before << 4) | (Add << 3) | 0, dest, WriteBack, RegList);
}
void ARMXEmitter::LDM(ARMReg dest, bool Add, bool Before, bool WriteBack, const int Regnum, ...)
{
	u16 RegList = 0;
	VA_TO_REGLIST(RegList, Regnum);
	WriteRegStoreOp(0x80 | (Before << 4) | (Add << 3) | 1, dest, WriteBack, RegList);
}

void ARMXEmitter::STMBitmask(ARMReg dest, bool Add, bool Before, bool WriteBack, const u16 RegList)
{
	WriteRegStoreOp(0x80 | (Before << 4) | (Add << 3) | 0, dest, WriteBack, RegList);
}
void ARMXEmitter::LDMBitmask(ARMReg dest, bool Add, bool Before, bool WriteBack, const u16 RegList)
{
	WriteRegStoreOp(0x80 | (Before << 4) | (Add << 3) | 1, dest, WriteBack, RegList);
}

#undef VA_TO_REGLIST

// NEON Specific
void ARMXEmitter::VABD(IntegerSize Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_assert_msg_(JIT, Vd >= D0, "Pass invalid register to VABD(float)");
	_assert_msg_(JIT, cpu_info.bNEON, "Can't use VABD(float) when CPU doesn't support it");
	bool register_quad = Vd >= Q0;

	// Gets encoded as a double register
	Vd = SubBase(Vd);
	Vn = SubBase(Vn);
	Vm = SubBase(Vm);

	Write32((0xF3 << 24) | ((Vd & 0x10) << 18) | (Size << 20) | ((Vn & 0xF) << 16) \
		| ((Vd & 0xF) << 12) | (0xD << 8) | ((Vn & 0x10) << 3) | (register_quad << 6) \
		| ((Vm & 0x10) << 2) | (Vm & 0xF));
}
void ARMXEmitter::VADD(IntegerSize Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_assert_msg_(JIT, Vd >= D0, "Pass invalid register to VADD(integer)");
	_assert_msg_(JIT, cpu_info.bNEON, "Can't use VADD(integer) when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	// Gets encoded as a double register
	Vd = SubBase(Vd);
	Vn = SubBase(Vn);
	Vm = SubBase(Vm);

	Write32((0xF2 << 24) | ((Vd & 0x10) << 18) | (Size << 20) | ((Vn & 0xF) << 16) \
		| ((Vd & 0xF) << 12) | (0x8 << 8) | ((Vn & 0x10) << 3) | (register_quad << 6) \
		| ((Vm & 0x10) << 1) | (Vm & 0xF));

}
void ARMXEmitter::VSUB(IntegerSize Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_assert_msg_(JIT, Vd >= Q0, "Pass invalid register to VSUB(integer)");
	_assert_msg_(JIT, cpu_info.bNEON, "Can't use VSUB(integer) when CPU doesn't support it");

	// Gets encoded as a double register
	Vd = SubBase(Vd);
	Vn = SubBase(Vn);
	Vm = SubBase(Vm);

	Write32((0xF3 << 24) | ((Vd & 0x10) << 18) | (Size << 20) | ((Vn & 0xF) << 16) \
		| ((Vd & 0xF) << 12) | (0x8 << 8) | ((Vn & 0x10) << 3) | (1 << 6) \
		| ((Vm & 0x10) << 2) | (Vm & 0xF));
}

extern const VFPEnc VFPOps[16][2] = {
	{{0xE0, 0xA0}, {  -1,   -1}}, // 0: VMLA
	{{0xE1, 0xA4}, {  -1,   -1}}, // 1: VNMLA
	{{0xE0, 0xA4}, {  -1,   -1}}, // 2: VMLS
	{{0xE1, 0xA0}, {  -1,   -1}}, // 3: VNMLS
	{{0xE3, 0xA0}, {  -1,   -1}}, // 4: VADD
	{{0xE3, 0xA4}, {  -1,   -1}}, // 5: VSUB
	{{0xE2, 0xA0}, {  -1,   -1}}, // 6: VMUL
	{{0xE2, 0xA4}, {  -1,   -1}}, // 7: VNMUL
	{{0xEB, 0xAC}, {  -1 /* 0x3B */,  -1 /* 0x70 */}}, // 8: VABS(Vn(0x0) used for encoding)
	{{0xE8, 0xA0}, {  -1,   -1}}, // 9: VDIV
	{{0xEB, 0xA4}, {  -1 /* 0x3B */,   -1 /* 0x78 */}}, // 10: VNEG(Vn(0x1) used for encoding)
	{{0xEB, 0xAC}, {  -1,   -1}}, // 11: VSQRT (Vn(0x1) used for encoding)
	{{0xEB, 0xA4}, {  -1,   -1}}, // 12: VCMP (Vn(0x4 | #0 ? 1 : 0) used for encoding)
	{{0xEB, 0xAC}, {  -1,   -1}}, // 13: VCMPE (Vn(0x4 | #0 ? 1 : 0) used for encoding)
	{{  -1,   -1}, {0x3B, 0x30}}, // 14: VABSi
	};

const char *VFPOpNames[16] = {
	"VMLA",
	"VNMLA",
	"VMLS",
	"VNMLS",
	"VADD",
	"VSUB",
	"VMUL",
	"VNMUL",
	"VABS",
	"VDIV",
	"VNEG",
	"VSQRT",
	"VCMP",
	"VCMPE",
	"VABSi",
};

u32 EncodeVd(ARMReg Vd)
{
	bool quad_reg = Vd >= Q0;
	bool double_reg = Vd >= D0;

	ARMReg Reg = SubBase(Vd);

	if (quad_reg)
		return ((Reg & 0x10) << 18) | ((Reg & 0xF) << 12);
	else {
		if (double_reg)
			return ((Reg & 0x10) << 18) | ((Reg & 0xF) << 12);
		else
			return ((Reg & 0x1) << 22) | ((Reg & 0x1E) << 11);
	}
}
u32 EncodeVn(ARMReg Vn)
{
	bool quad_reg = Vn >= Q0;
	bool double_reg = Vn >= D0;

	ARMReg Reg = SubBase(Vn);
	if (quad_reg)
		return ((Reg & 0xF) << 16) | ((Reg & 0x10) << 3);
	else {
		if (double_reg)
			return ((Reg & 0xF) << 16) | ((Reg & 0x10) << 3);
		else
			return ((Reg & 0x1E) << 15) | ((Reg & 0x1) << 7);
	}
}
u32 EncodeVm(ARMReg Vm)
{
	bool quad_reg = Vm >= Q0;
	bool double_reg = Vm >= D0;

	ARMReg Reg = SubBase(Vm);

	if (quad_reg)
		return ((Reg & 0x10) << 1) | (Reg & 0xF);
	else {
		if (double_reg)
			return ((Reg & 0x10) << 1) | (Reg & 0xF);
		else
			return ((Reg & 0x1) << 5) | (Reg >> 1);
	}
}

u32 encodedSize(u32 value)
{
	if (value & I_8)
		return 0;
	else if (value & I_16)
		return 1;
	else if ((value & I_32) || (value & F_32))
		return 2;
	else if (value & I_64)
		return 3;
	else
		_dbg_assert_msg_(JIT, false, "Passed invalid size to integer NEON instruction");
	return 0;
}

ARMReg SubBase(ARMReg Reg)
{
	if (Reg >= S0)
	{
		if (Reg >= D0)
		{
			if (Reg >= Q0)
				return (ARMReg)((Reg - Q0) * 2); // Always gets encoded as a double register
			return (ARMReg)(Reg - D0);
		}
		return (ARMReg)(Reg - S0);
	}
	return Reg;
}

ARMReg DScalar(ARMReg dreg, int subScalar) {
	int dr = (int)(SubBase(dreg)) & 0xF;
	int scalar = ((subScalar << 4) | dr);
	ARMReg ret =  (ARMReg)(D0 + scalar);
	// ILOG("Scalar: %i D0: %i AR: %i", scalar, (int)D0, (int)ret);
	return ret;
}

// Convert to a DScalar
ARMReg QScalar(ARMReg qreg, int subScalar) {
	int dr = (int)(SubBase(qreg)) & 0xF;
	if (subScalar & 2) {
		dr++;
	}
	int scalar = (((subScalar & 1) << 4) | dr);
	ARMReg ret =  (ARMReg)(D0 + scalar);
	return ret;
}

void ARMXEmitter::WriteVFPDataOp(u32 Op, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	bool quad_reg = Vd >= Q0;
	bool double_reg = Vd >= D0 && Vd < Q0;

	VFPEnc enc = VFPOps[Op][quad_reg];
	if (enc.opc1 == -1 && enc.opc2 == -1)
		_assert_msg_(JIT, false, "%s does not support %s", VFPOpNames[Op], quad_reg ? "NEON" : "VFP"); 
	u32 VdEnc = EncodeVd(Vd);
	u32 VnEnc = EncodeVn(Vn);
	u32 VmEnc = EncodeVm(Vm);
	u32 cond = quad_reg ? (0xF << 28) : condition;

	Write32(cond | (enc.opc1 << 20) | VnEnc | VdEnc | (enc.opc2 << 4) | (quad_reg << 6) | (double_reg << 8) | VmEnc);
}
void ARMXEmitter::VMLA(ARMReg Vd, ARMReg Vn, ARMReg Vm){ WriteVFPDataOp(0, Vd, Vn, Vm); }
void ARMXEmitter::VNMLA(ARMReg Vd, ARMReg Vn, ARMReg Vm){ WriteVFPDataOp(1, Vd, Vn, Vm); }
void ARMXEmitter::VMLS(ARMReg Vd, ARMReg Vn, ARMReg Vm){ WriteVFPDataOp(2, Vd, Vn, Vm); }
void ARMXEmitter::VNMLS(ARMReg Vd, ARMReg Vn, ARMReg Vm){ WriteVFPDataOp(3, Vd, Vn, Vm); }
void ARMXEmitter::VADD(ARMReg Vd, ARMReg Vn, ARMReg Vm){ WriteVFPDataOp(4, Vd, Vn, Vm); }
void ARMXEmitter::VSUB(ARMReg Vd, ARMReg Vn, ARMReg Vm){ WriteVFPDataOp(5, Vd, Vn, Vm); }
void ARMXEmitter::VMUL(ARMReg Vd, ARMReg Vn, ARMReg Vm){ WriteVFPDataOp(6, Vd, Vn, Vm); }
void ARMXEmitter::VNMUL(ARMReg Vd, ARMReg Vn, ARMReg Vm){ WriteVFPDataOp(7, Vd, Vn, Vm); }
void ARMXEmitter::VABS(ARMReg Vd, ARMReg Vm){ WriteVFPDataOp(8, Vd, D0, Vm); }
void ARMXEmitter::VDIV(ARMReg Vd, ARMReg Vn, ARMReg Vm){ WriteVFPDataOp(9, Vd, Vn, Vm); }
void ARMXEmitter::VNEG(ARMReg Vd, ARMReg Vm){ WriteVFPDataOp(10, Vd, D1, Vm); }
void ARMXEmitter::VSQRT(ARMReg Vd, ARMReg Vm){ WriteVFPDataOp(11, Vd, D1, Vm); }
void ARMXEmitter::VCMP(ARMReg Vd, ARMReg Vm){ WriteVFPDataOp(12, Vd, D4, Vm); }
void ARMXEmitter::VCMPE(ARMReg Vd, ARMReg Vm){ WriteVFPDataOp(13, Vd, D4, Vm); }
void ARMXEmitter::VCMP(ARMReg Vd){ WriteVFPDataOp(12, Vd, D5, D0); }
void ARMXEmitter::VCMPE(ARMReg Vd){ WriteVFPDataOp(13, Vd, D5, D0); }

void ARMXEmitter::VLDR(ARMReg Dest, ARMReg Base, s16 offset)
{
	_assert_msg_(JIT, Dest >= S0 && Dest <= D31, "Passed Invalid dest register to VLDR");
	_assert_msg_(JIT, Base <= R15, "Passed invalid Base register to VLDR");

	bool Add = offset >= 0 ? true : false;
	u32 imm = abs(offset);

	_assert_msg_(JIT, (imm & 0xC03) == 0, "VLDR: Offset needs to be word aligned and small enough");

	if (imm & 0xC03)
		ERROR_LOG(JIT, "VLDR: Bad offset %08x", imm);

	bool single_reg = Dest < D0;

	Dest = SubBase(Dest);

	if (single_reg)
	{
		Write32(condition | (0xD << 24) | (Add << 23) | ((Dest & 0x1) << 22) | (1 << 20) | (Base << 16) \
			| ((Dest & 0x1E) << 11) | (10 << 8) | (imm >> 2));
	}
	else
	{
		Write32(condition | (0xD << 24) | (Add << 23) | ((Dest & 0x10) << 18) | (1 << 20) | (Base << 16) \
			| ((Dest & 0xF) << 12) | (11 << 8) | (imm >> 2));
	}
}
void ARMXEmitter::VSTR(ARMReg Src, ARMReg Base, s16 offset)
{
	_assert_msg_(JIT, Src >= S0 && Src <= D31, "Passed invalid src register to VSTR");
	_assert_msg_(JIT, Base <= R15, "Passed invalid base register to VSTR");

	bool Add = offset >= 0 ? true : false;
	u32 imm = abs(offset);

	_assert_msg_(JIT, (imm & 0xC03) == 0, "VSTR: Offset needs to be word aligned and small enough");

	if (imm & 0xC03)
		ERROR_LOG(JIT, "VSTR: Bad offset %08x", imm);

	bool single_reg = Src < D0;

	Src = SubBase(Src);

	if (single_reg)
	{
		Write32(condition | (0xD << 24) | (Add << 23) | ((Src & 0x1) << 22) | (Base << 16) \
			| ((Src & 0x1E) << 11) | (10 << 8) | (imm >> 2));
	}
	else
	{
		Write32(condition | (0xD << 24) | (Add << 23) | ((Src & 0x10) << 18) | (Base << 16) \
			| ((Src & 0xF) << 12) | (11 << 8) | (imm >> 2));
	}
}

void ARMXEmitter::VMRS_APSR() {
	Write32(condition | 0x0EF10A10 | (15 << 12));
}
void ARMXEmitter::VMRS(ARMReg Rt) {
	Write32(condition | (0xEF << 20) | (1 << 16) | (Rt << 12) | 0xA10);
}
void ARMXEmitter::VMSR(ARMReg Rt) {
	Write32(condition | (0xEE << 20) | (1 << 16) | (Rt << 12) | 0xA10);
}

// VFP and ASIMD
void ARMXEmitter::VMOV(ARMReg Dest, Operand2 op2)
{
	_assert_msg_(JIT, cpu_info.bVFPv3, "VMOV #imm requires VFPv3");
	Write32(condition | (0xEB << 20) | EncodeVd(Dest) | (0xA << 8) | op2.Imm8VFP());
}
void ARMXEmitter::VMOV(ARMReg Dest, ARMReg Src, bool high)
{
	_assert_msg_(JIT, Src < S0, "This VMOV doesn't support SRC other than ARM Reg");
	_assert_msg_(JIT, Dest >= D0, "This VMOV doesn't support DEST other than VFP");

	Dest = SubBase(Dest);

	Write32(condition | (0xE << 24) | (high << 21) | ((Dest & 0xF) << 16) | (Src << 12) \
		| (0xB << 8) | ((Dest & 0x10) << 3) | (1 << 4));
}

void ARMXEmitter::VMOV(ARMReg Dest, ARMReg Src)
{
	if (Dest > R15)
	{
		if (Src < S0)
		{
			if (Dest < D0)
			{
				// Moving to a Neon register FROM ARM Reg
				Dest = (ARMReg)(Dest - S0); 
				Write32(condition | (0xE0 << 20) | ((Dest & 0x1E) << 15) | (Src << 12) \
						| (0xA << 8) | ((Dest & 0x1) << 7) | (1 << 4));
				return;
			}
			else
			{
				// Move 64bit from Arm reg
				_assert_msg_(JIT, false, "This VMOV doesn't support moving 64bit ARM to NEON");
				return;
			}
		}
	}
	else
	{
		if (Src > R15)
		{
			if (Src < D0)
			{
				// Moving to ARM Reg from Neon Register
				Src = (ARMReg)(Src - S0);
				Write32(condition | (0xE1 << 20) | ((Src & 0x1E) << 15) | (Dest << 12) \
						| (0xA << 8) | ((Src & 0x1) << 7) | (1 << 4));
				return;
			}
			else
			{
				// Move 64bit To Arm reg
				_assert_msg_(JIT, false, "This VMOV doesn't support moving 64bit ARM From NEON");
				return;
			}
		}
		else
		{
			// Move Arm reg to Arm reg
			_assert_msg_(JIT, false, "VMOV doesn't support moving ARM registers");
		}
	}
	// Moving NEON registers
	int SrcSize = Src < D0 ? 1 : Src < Q0 ? 2 : 4;
	int DestSize = Dest < D0 ? 1 : Dest < Q0 ? 2 : 4;
	bool Single = DestSize == 1;
	bool Quad = DestSize == 4;

	_assert_msg_(JIT, SrcSize == DestSize, "VMOV doesn't support moving different register sizes");

	Dest = SubBase(Dest);
	Src = SubBase(Src);

	if (Single)
	{
		Write32(condition | (0x1D << 23) | ((Dest & 0x1) << 22) | (0x3 << 20) | ((Dest & 0x1E) << 11) \
				| (0x5 << 9) | (1 << 6) | ((Src & 0x1) << 5) | ((Src & 0x1E) >> 1));
	}
	else
	{
		// Double and quad
		if (Quad)
		{
			_assert_msg_(JIT, cpu_info.bNEON, "Trying to use quad registers when you don't support ASIMD."); 
			// Gets encoded as a Double register
			Write32((0xF2 << 24) | ((Dest & 0x10) << 18) | (2 << 20) | ((Src & 0xF) << 16) \
				| ((Dest & 0xF) << 12) | (1 << 8) | ((Src & 0x10) << 3) | (1 << 6) \
				| ((Src & 0x10) << 1) | (1 << 4) | (Src & 0xF));

		}
		else
		{
			Write32(condition | (0x1D << 23) | ((Dest & 0x10) << 18) | (0x3 << 20) | ((Dest & 0xF) << 12) \
				| (0x2D << 6) | ((Src & 0x10) << 1) | (Src & 0xF));
		}
	}
}

void ARMXEmitter::VCVT(ARMReg Dest, ARMReg Source, int flags)
{
	bool single_reg = (Dest < D0) && (Source < D0);
	bool single_double = !single_reg && (Source < D0 || Dest < D0);
	bool single_to_double = Source < D0;
	int op  = ((flags & TO_INT) ? (flags & ROUND_TO_ZERO) : (flags & IS_SIGNED)) ? 1 : 0;
	int op2 = ((flags & TO_INT) ? (flags & IS_SIGNED) : 0) ? 1 : 0;
	Dest = SubBase(Dest);
	Source = SubBase(Source);

	if (single_double)
	{
		// S32<->F64
		if (flags & TO_INT)
		{
			if (single_to_double)
			{
				Write32(condition | (0x1D << 23) | ((Dest & 0x10) << 18) | (0x7 << 19) \
					| ((Dest & 0xF) << 12) | (op << 7) | (0x2D << 6) | ((Source & 0x1) << 5) | (Source >> 1));
			} else {
				Write32(condition | (0x1D << 23) | ((Dest & 0x1) << 22) | (0x7 << 19) | ((flags & TO_INT) << 18) | (op2 << 16) \
					| ((Dest & 0x1E) << 11) | (op << 7) | (0x2D << 6) | ((Source & 0x10) << 1) | (Source & 0xF));
			}
		}
		// F32<->F64
		else {
			if (single_to_double)
			{
				Write32(condition | (0x1D << 23) | ((Dest & 0x10) << 18) | (0x3 << 20) | (0x7 << 16) \
					| ((Dest & 0xF) << 12) | (0x2F << 6) | ((Source & 0x1) << 5) | (Source >> 1));
			} else {
				Write32(condition | (0x1D << 23) | ((Dest & 0x1) << 22) | (0x3 << 20) | (0x7 << 16) \
					| ((Dest & 0x1E) << 11) | (0x2B << 6) | ((Source & 0x10) << 1) | (Source & 0xF));
			}
		}
	} else if (single_reg) {
		Write32(condition | (0x1D << 23) | ((Dest & 0x1) << 22) | (0x7 << 19) | ((flags & TO_INT) << 18) | (op2 << 16) \
			| ((Dest & 0x1E) << 11) | (op << 7) | (0x29 << 6) | ((Source & 0x1) << 5) | (Source >> 1));
	} else {
		Write32(condition | (0x1D << 23) | ((Dest & 0x10) << 18) | (0x7 << 19) | ((flags & TO_INT) << 18) | (op2 << 16) \
			| ((Dest & 0xF) << 12) | (1 << 8) | (op << 7) | (0x29 << 6) | ((Source & 0x10) << 1) | (Source & 0xF));
	}
}

void ARMXEmitter::VABA(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");
	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | ((Size & I_UNSIGNED ? 1 : 0) << 24) | EncodeVn(Vn) \
		| (encodedSize(Size) << 20) | EncodeVd(Vd) | (0x71 << 4) | (register_quad << 6) | EncodeVm(Vm));
}

void ARMXEmitter::VABAL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vn >= D0 && Vn < Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vm >= D0 && Vm < Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");

	Write32((0xF2 << 24) | ((Size & I_UNSIGNED ? 1 : 0) << 24) | (1 << 23) | EncodeVn(Vn) \
		| (encodedSize(Size) << 20) | EncodeVd(Vd) | (0x50 << 4) | EncodeVm(Vm));
}

void ARMXEmitter::VABD(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	bool register_quad = Vd >= Q0;

	if (Size & F_32)
		Write32((0xF3 << 24) | (1 << 21) | EncodeVn(Vn) | EncodeVd(Vd) | (0xD << 8) | EncodeVm(Vm));
	else
		Write32((0xF2 << 24) | ((Size & I_UNSIGNED ? 1 : 0) << 24) | EncodeVn(Vn) \
			| (encodedSize(Size) << 20) | EncodeVd(Vd) | (0x70 << 4) | (register_quad << 6) | EncodeVm(Vm));
}

void ARMXEmitter::VABDL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vn >= D0 && Vn < Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vm >= D0 && Vm < Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");

	Write32((0xF2 << 24) | ((Size & I_UNSIGNED ? 1 : 0) << 24) | (1 << 23) | EncodeVn(Vn) \
		| (encodedSize(Size) << 20) | EncodeVd(Vd) | (0x70 << 4) | EncodeVm(Vm));
}

void ARMXEmitter::VABS(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (0xB1 << 16) | (encodedSize(Size) << 18) | EncodeVd(Vd) \
		| ((Size & F_32 ? 1 : 0) << 10) | (0x30 << 4) | (register_quad << 6) | EncodeVm(Vm));
}

void ARMXEmitter::VACGE(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	// Only Float
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | EncodeVn(Vn) | EncodeVd(Vd) \
		| (0xD1 << 4) | (register_quad << 6) | EncodeVm(Vm));
}

void ARMXEmitter::VACGT(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	// Only Float
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (1 << 21) | EncodeVn(Vn) | EncodeVd(Vd) \
		| (0xD1 << 4) | (register_quad << 6) | EncodeVm(Vm));
}

void ARMXEmitter::VACLE(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	VACGE(Vd, Vm, Vn);
}

void ARMXEmitter::VACLT(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	VACGT(Vd, Vn, Vm);
}

void ARMXEmitter::VADD(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	if (Size & F_32)
		Write32((0xF2 << 24) | EncodeVn(Vn) | EncodeVd(Vd) | (0xD0 << 4) | (register_quad << 6) | EncodeVm(Vm));
	else
		Write32((0xF2 << 24) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) \
			| (0x8 << 8) | (register_quad << 6) | EncodeVm(Vm));
}

void ARMXEmitter::VADDHN(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd < Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vn >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vm >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");

	Write32((0xF2 << 24) | (1 << 23) | (encodedSize(Size) << 20) | EncodeVn(Vn) \
		| EncodeVd(Vd) | (0x80 << 4) | EncodeVm(Vm));
}

void ARMXEmitter::VADDL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vn >= D0 && Vn < Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vm >= D0 && Vm < Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");

	Write32((0xF2 << 24) | ((Size & I_UNSIGNED ? 1 : 0) << 24) | (1 << 23) | (encodedSize(Size) << 20) | EncodeVn(Vn) \
		| EncodeVd(Vd) | EncodeVm(Vm));
}
void ARMXEmitter::VADDW(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vn >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vm >= D0 && Vm < Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");

	Write32((0xF2 << 24) | ((Size & I_UNSIGNED ? 1 : 0) << 24) | (1 << 23) | (encodedSize(Size) << 20) | EncodeVn(Vn) \
		| EncodeVd(Vd) | (1 << 8) | EncodeVm(Vm));
}
void ARMXEmitter::VAND(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	// _dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");
	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | EncodeVn(Vn) | EncodeVd(Vd) | (0x11 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VBIC(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	//  _dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");
	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (1 << 20) | EncodeVn(Vn) | EncodeVd(Vd) | (0x11 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VBIF(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	// _dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");
	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (3 << 20) | EncodeVn(Vn) | EncodeVd(Vd) | (0x11 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VBIT(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	// _dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");
	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (2 << 20) | EncodeVn(Vn) | EncodeVd(Vd) | (0x11 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VBSL(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	// _dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");
	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (1 << 20) | EncodeVn(Vn) | EncodeVd(Vd) | (0x11 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VCEQ(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;
	if (Size & F_32)
		Write32((0xF2 << 24) | EncodeVn(Vn) | EncodeVd(Vd) | (0xE0 << 4) | (register_quad << 6) | EncodeVm(Vm));
	else
		Write32((0xF3 << 24) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) \
			| (0x81 << 4) | (register_quad << 6) | EncodeVm(Vm));

}
void ARMXEmitter::VCEQ(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (0xB << 20) | (encodedSize(Size) << 18) | (1 << 16) \
		| EncodeVd(Vd) | ((Size & F_32 ? 1 : 0) << 10) | (0x10 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VCGE(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;
	if (Size & F_32)
		Write32((0xF3 << 24) | EncodeVn(Vn) | EncodeVd(Vd) | (0xE0 << 4) | (register_quad << 6) | EncodeVm(Vm));
	else
		Write32((0xF2 << 24) | ((Size & I_UNSIGNED ? 1 : 0) << 24)  | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) \
			| (0x31 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VCGE(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;
	Write32((0xF3 << 24) | (0xB << 20) | (encodedSize(Size) << 18) | (1 << 16) \
		| EncodeVd(Vd) | ((Size & F_32 ? 1 : 0) << 10) | (0x8 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VCGT(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;
	if (Size & F_32)
		Write32((0xF3 << 24) | (1 << 21) | EncodeVn(Vn) | EncodeVd(Vd) | (0xE0 << 4) | (register_quad << 6) | EncodeVm(Vm));
	else
		Write32((0xF2 << 24) | ((Size & I_UNSIGNED ? 1 : 0) << 24)  | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) \
			| (0x30 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VCGT(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;
	Write32((0xF3 << 24) | (0xD << 20) | (encodedSize(Size) << 18) | (1 << 16) \
		| EncodeVd(Vd) | ((Size & F_32 ? 1 : 0) << 10) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VCLE(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	VCGE(Size, Vd, Vm, Vn);
}
void ARMXEmitter::VCLE(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;
	Write32((0xF3 << 24) | (0xD << 20) | (encodedSize(Size) << 18) | (1 << 16) \
		| EncodeVd(Vd) | ((Size & F_32 ? 1 : 0) << 10) | (3 << 7) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VCLS(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");

	bool register_quad = Vd >= Q0;
	Write32((0xF3 << 24) | (0xD << 20) | (encodedSize(Size) << 18) \
		| EncodeVd(Vd) | (1 << 10) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VCLT(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	VCGT(Size, Vd, Vm, Vn);
}
void ARMXEmitter::VCLT(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;
	Write32((0xF3 << 24) | (0xD << 20) | (encodedSize(Size) << 18) | (1 << 16) \
		| EncodeVd(Vd) | ((Size & F_32 ? 1 : 0) << 10) | (0x20 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VCLZ(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;
	Write32((0xF3 << 24) | (0xD << 20) | (encodedSize(Size) << 18) \
		| EncodeVd(Vd) | (0x48 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VCNT(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, Size & I_8, "Can only use I_8 with " __FUNCTION__);

	bool register_quad = Vd >= Q0;
	Write32((0xF3 << 24) | (0xD << 20) | (encodedSize(Size) << 18) \
		| EncodeVd(Vd) | (0x90 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VDUP(u32 Size, ARMReg Vd, ARMReg Vm, u8 index)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;
	u32 imm4 = 0;
	if (Size & I_8)
		imm4 = (index << 1) | 1;
	else if (Size & I_16)
		imm4 = (index << 2) | 2;
	else if (Size & (I_32 | F_32))
		imm4 = (index << 3) | 4;
	Write32((0xF3 << 24) | (0xB << 20) | (imm4 << 16)  \
		| EncodeVd(Vd) | (0xC << 8) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VDUP(u32 Size, ARMReg Vd, ARMReg Rt)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Rt < D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;
	Vd = SubBase(Vd);
	u8 sizeEncoded = 0;
	if (Size & I_8)
		sizeEncoded = 2;
	else if (Size & I_16)
		sizeEncoded = 1;
	else if (Size & I_32)
		sizeEncoded = 0;

	Write32((0xEE << 24) | (0x8 << 20) | ((sizeEncoded & 2) << 21) | (register_quad << 21) \
		| ((Vd & 0xF) << 16) | (Rt << 12) | (0xD1 << 4) | ((Vd & 0x10) << 3) | (1 << 4));
}
void ARMXEmitter::VEOR(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | EncodeVn(Vn) | EncodeVd(Vd) | (0x11 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VEXT(ARMReg Vd, ARMReg Vn, ARMReg Vm, u8 index)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (0xB << 20) | EncodeVn(Vn) | EncodeVd(Vd) | (index & 0xF) \
		| (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VFMA(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, cpu_info.bVFPv4, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | EncodeVn(Vn) | EncodeVd(Vd) | (0xC1 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VFMS(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, cpu_info.bVFPv4, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (1 << 21) | EncodeVn(Vn) | EncodeVd(Vd) | (0xC1 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VHADD(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | ((Size & I_UNSIGNED ? 1 : 0) << 23) | (encodedSize(Size) << 20) \
		| EncodeVn(Vn) | EncodeVd(Vd) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VHSUB(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | ((Size & I_UNSIGNED ? 1 : 0) << 23) | (encodedSize(Size) << 20) \
		| EncodeVn(Vn) | EncodeVd(Vd) | (1 << 9) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VMAX(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	if (Size & F_32)
		Write32((0xF2 << 24) | EncodeVn(Vn) | EncodeVd(Vd) | (0xF0 << 4) | (register_quad << 6) | EncodeVm(Vm));
	else
		Write32((0xF2 << 24) | ((Size & I_UNSIGNED ? 1 : 0) << 23) | (encodedSize(Size) << 20) \
			| EncodeVn(Vn) | EncodeVd(Vd) | (0x60 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VMIN(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	if (Size & F_32)
		Write32((0xF2 << 24) | (1 << 21) | EncodeVn(Vn) | EncodeVd(Vd) | (0xF0 << 4) | (register_quad << 6) | EncodeVm(Vm));
	else
		Write32((0xF2 << 24) | ((Size & I_UNSIGNED ? 1 : 0) << 23) | (encodedSize(Size) << 20) \
			| EncodeVn(Vn) | EncodeVd(Vd) | (0x61 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VMLA(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	if (Size & F_32)
		Write32((0xF2 << 24) | EncodeVn(Vn) | EncodeVd(Vd) | (0xD1 << 4) | (register_quad << 6) | EncodeVm(Vm));
	else
		Write32((0xF2 << 24) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | (0x90 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VMLS(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	if (Size & F_32)
		Write32((0xF2 << 24) | (1 << 21) | EncodeVn(Vn) | EncodeVd(Vd) | (0xD1 << 4) | (register_quad << 6) | EncodeVm(Vm));
	else
		Write32((0xF2 << 24) | (1 << 24) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | (0x90 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VMLAL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vn >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vm >= D0 && Vm < Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");

	Write32((0xF2 << 24) | ((Size & I_UNSIGNED ? 1 : 0) << 24) | (encodedSize(Size) << 20) \
		| EncodeVn(Vn) | EncodeVd(Vd) | (0x80 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VMLSL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vn >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vm >= D0 && Vm < Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float.");

	Write32((0xF2 << 24) | ((Size & I_UNSIGNED ? 1 : 0) << 24) | (encodedSize(Size) << 20) \
		| EncodeVn(Vn) | EncodeVd(Vd) | (0xA0 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VMUL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	if (Size & F_32)
		Write32((0xF3 << 24) | EncodeVn(Vn) | EncodeVd(Vd) | (0xD1 << 4) | (register_quad << 6) | EncodeVm(Vm));
	else
		Write32((0xF2 << 24) | ((Size & I_POLYNOMIAL) ? (1 << 24) : 0) | (encodedSize(Size) << 20) | \
				EncodeVn(Vn) | EncodeVd(Vd) | (0x91 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VMULL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	Write32((0xF2 << 24) | (1 << 23) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0xC0 << 4) | ((Size & I_POLYNOMIAL) ? 1 << 9 : 0) | EncodeVm(Vm));
}
void ARMXEmitter::VMLA_scalar(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	// No idea if the Non-Q case here works. Not really that interested.
	if (Size & F_32)
		Write32((0xF2 << 24) | (register_quad << 24) | (1 << 23) | (2 << 20) | EncodeVn(Vn) | EncodeVd(Vd) | (0x14 << 4) | EncodeVm(Vm));
	else
		_dbg_assert_msg_(JIT, false, "VMLA_scalar only supports float atm");
	//else
	//	Write32((0xF2 << 24) | (1 << 23) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | (0x90 << 4) | (1 << 6) | EncodeVm(Vm));
	// Unsigned support missing
}
void ARMXEmitter::VMUL_scalar(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	int VmEnc = EncodeVm(Vm);
	// No idea if the Non-Q case here works. Not really that interested.
	if (Size & F_32)  // Q flag
		Write32((0xF2 << 24) | (register_quad << 24) | (1 << 23) | (2 << 20) | EncodeVn(Vn) | EncodeVd(Vd) | (0x94 << 4) | VmEnc);
	else
		_dbg_assert_msg_(JIT, false, "VMUL_scalar only supports float atm");
	
		// Write32((0xF2 << 24) | ((Size & I_POLYNOMIAL) ? (1 << 24) : 0) | (1 << 23) | (encodedSize(Size) << 20) | 
		// EncodeVn(Vn) | EncodeVd(Vd) | (0x84 << 4) | (register_quad << 6) | EncodeVm(Vm));
	// Unsigned support missing
}

void ARMXEmitter::VNEG(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (0xB << 20) | (encodedSize(Size) << 18) | (1 << 16) | \
			EncodeVd(Vd) | ((Size & F_32) ? 1 << 10 : 0) | (0xE << 6) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VORN(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (3 << 20) | EncodeVn(Vn) | EncodeVd(Vd) | (0x11 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VORR(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (2 << 20) | EncodeVn(Vn) | EncodeVd(Vd) | (0x11 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VPADAL(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (0xB << 20) | (encodedSize(Size) << 18) | EncodeVd(Vd) | \
			(0x60 << 4) | ((Size & I_UNSIGNED) ? 1 << 7 : 0) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VPADD(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	if (Size & F_32)
		Write32((0xF3 << 24) | EncodeVn(Vn) | EncodeVd(Vd) | (0xD0 << 4) | EncodeVm(Vm));
	else
		Write32((0xF2 << 24) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
				(0xB1 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VPADDL(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (0xB << 20) | (encodedSize(Size) << 18) | EncodeVd(Vd) | \
			(0x20 << 4) | (Size & I_UNSIGNED ? 1 << 7 : 0) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VPMAX(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	if (Size & F_32)
		Write32((0xF3 << 24) | EncodeVn(Vn) | EncodeVd(Vd) | (0xF0 << 4) | EncodeVm(Vm));
	else
		Write32((0xF2 << 24) | (Size & I_UNSIGNED ? 1 << 24 : 0) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
				(0xA0 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VPMIN(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	if (Size & F_32)
		Write32((0xF3 << 24) | (1 << 21) | EncodeVn(Vn) | EncodeVd(Vd) | (0xF0 << 4) | EncodeVm(Vm));
	else
		Write32((0xF2 << 24) | (Size & I_UNSIGNED ? 1 << 24 : 0) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
				(0xA1 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VQABS(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (0xB << 20) | (encodedSize(Size) << 18) | EncodeVd(Vd) | \
			(0x70 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VQADD(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0x1 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VQDMLAL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	Write32((0xF2 << 24) | (1 << 23) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0x90 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VQDMLSL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	Write32((0xF2 << 24) | (1 << 23) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0xB0 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VQDMULH(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	Write32((0xF2 << 24) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0xB0 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VQDMULL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	Write32((0xF2 << 24) | (1 << 23) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0xD0 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VQNEG(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (0xB << 20) | (encodedSize(Size) << 18) | EncodeVd(Vd) | \
			(0x78 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VQRDMULH(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	Write32((0xF3 << 24) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0xB0 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VQRSHL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (Size & I_UNSIGNED ? 1 << 24 : 0) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0x51 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VQSHL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (Size & I_UNSIGNED ? 1 << 24 : 0) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0x41 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VQSUB(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (Size & I_UNSIGNED ? 1 << 24 : 0) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0x21 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VRADDHN(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	Write32((0xF3 << 24) | (1 << 23) | ((encodedSize(Size) - 1) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0x40 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VRECPE(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (0xB << 20) | (0xB << 16) | EncodeVd(Vd) | \
			(0x40 << 4) | (Size & F_32 ? 1 << 8 : 0) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VRECPS(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | EncodeVn(Vn) | EncodeVd(Vd) | (0xF1 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VRHADD(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (Size & I_UNSIGNED ? 1 << 24 : 0) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0x10 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VRSHL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (Size & I_UNSIGNED ? 1 << 24 : 0) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0x50 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VRSQRTE(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;
	Vd = SubBase(Vd);
	Vm = SubBase(Vm);

	Write32((0xF3 << 24) | (0xB << 20) | ((Vd & 0x10) << 18) | (0xB << 16)
			| ((Vd & 0xF) << 12) | (9 << 7) | (Size & F_32 ? (1 << 8) : 0) | (register_quad << 6)
			| ((Vm & 0x10) << 1) | (Vm & 0xF));
}
void ARMXEmitter::VRSQRTS(ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (1 << 21) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0xF1 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VRSUBHN(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	Write32((0xF3 << 24) | (1 << 23) | ((encodedSize(Size) - 1) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0x60 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VSHL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, !(Size & F_32), __FUNCTION__ " doesn't support float");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (Size & I_UNSIGNED ? 1 << 24 : 0) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0x40 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VSUB(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	if (Size & F_32)
		Write32((0xF2 << 24) | (1 << 21) | EncodeVn(Vn) | EncodeVd(Vd) | \
				(0xD0 << 4) | (register_quad << 6) | EncodeVm(Vm));
	else
		Write32((0xF3 << 24) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
				(0x80 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VSUBHN(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	Write32((0xF2 << 24) | (1 << 23) | ((encodedSize(Size) - 1) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0x60 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VSUBL(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	Write32((0xF2 << 24) | (Size & I_UNSIGNED ? 1 << 24 : 0) | (1 << 23) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0x20 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VSUBW(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	Write32((0xF2 << 24) | (Size & I_UNSIGNED ? 1 << 24 : 0) | (1 << 23) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0x30 << 4) | EncodeVm(Vm));
}
void ARMXEmitter::VSWP(ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (0xB << 20) | (1 << 17) | EncodeVd(Vd) | \
			(register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VTRN(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (0xB << 20) | (encodedSize(Size) << 18) | (1 << 17) | EncodeVd(Vd) | \
			(1 << 7) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VTST(u32 Size, ARMReg Vd, ARMReg Vn, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	Write32((0xF2 << 24) | (encodedSize(Size) << 20) | EncodeVn(Vn) | EncodeVd(Vd) | \
			(0x81 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VUZP(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (0xB << 20) | (encodedSize(Size) << 18) | (1 << 17) | EncodeVd(Vd) | \
			(0x10 << 4) | (register_quad << 6) | EncodeVm(Vm));
}
void ARMXEmitter::VZIP(u32 Size, ARMReg Vd, ARMReg Vm)
{
	 _dbg_assert_msg_(JIT, Vd >= D0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	Write32((0xF3 << 24) | (0xB << 20) | (encodedSize(Size) << 18) | (1 << 17) | EncodeVd(Vd) | \
			(0x18 << 4) | (register_quad << 6) | EncodeVm(Vm));
}

void ARMXEmitter::VMOVL(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vd >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vm >= D0 && Vm <= D31, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");
	_dbg_assert_msg_(JIT, (Size & (I_UNSIGNED | I_SIGNED)) != 0, "Must specify I_SIGNED or I_UNSIGNED in VMOVL");

	bool unsign = (Size & I_UNSIGNED) != 0;
	int imm3 = 0;
	if (Size & I_8) imm3 = 1;
	if (Size & I_16) imm3 = 2;
	if (Size & I_32) imm3 = 4;

	Write32((0xF2 << 24) | (unsign << 24) | (1 << 23) | (imm3 << 19) | EncodeVd(Vd) | \
		(0xA1 << 4) | EncodeVm(Vm));
}

void ARMXEmitter::VMOVN(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, Vm >= Q0, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, Vd >= D0 && Vd <= D31, "Pass invalid register to " __FUNCTION__);
	_dbg_assert_msg_(JIT, cpu_info.bNEON, "Can't use " __FUNCTION__ " when CPU doesn't support it");

	bool register_quad = Vd >= Q0;

	Write32((0xF3B << 20) | (encodedSize(Size) << 18) | (1 << 17) | EncodeVd(Vd) | (1 << 9) | EncodeVm(Vm));
}

void ARMXEmitter::VCVT(u32 Size, ARMReg Vd, ARMReg Vm)
{
	_dbg_assert_msg_(JIT, (Size & (I_UNSIGNED | I_SIGNED)) != 0, "Must specify I_SIGNED or I_UNSIGNED in VCVT NEON");

	bool register_quad = Vd >= Q0;
	bool toInteger = (Size & I_32) != 0;
	bool isUnsigned = (Size & I_UNSIGNED) != 0;
	int op = (toInteger << 1) | (int)isUnsigned;

	Write32((0xF3 << 24) | (0xBB << 16) | EncodeVd(Vd) | (0x3 << 9) | (op << 7) | (register_quad << 6) | EncodeVm(Vm));
}

static int RegCountToType(int nRegs, NEONAlignment align) {
	switch (nRegs) {
	case 1:
		_dbg_assert_msg_(JIT, !((int)align & 1), "align & 1 must be == 0");
		return 7;
	case 2:
		_dbg_assert_msg_(JIT, !((int)align & 3), "align & 3 must be == 0");
		return 10;
	case 3:
		_dbg_assert_msg_(JIT, !((int)align & 1), "align & 1 must be == 0");
		return 6;
	case 4:
		return 4;
	default:
		_dbg_assert_msg_(JIT, false, "Invalid number of registers passed to vector load/store");
		return 0;
	}
}

void ARMXEmitter::WriteVLDST1(bool load, u32 Size, ARMReg Vd, ARMReg Rn, int regCount, NEONAlignment align, ARMReg Rm)
{
	u32 spacing = RegCountToType(regCount, align); // Only support loading to 1 reg
	// Gets encoded as a double register
	Vd = SubBase(Vd);

	Write32((0xF4 << 24) | ((Vd & 0x10) << 18) | (load << 21) | (Rn << 16)
			| ((Vd & 0xF) << 12) | (spacing << 8) | (encodedSize(Size) << 6)
			| (align << 4) | Rm);
}

void ARMXEmitter::VLD1(u32 Size, ARMReg Vd, ARMReg Rn, int regCount, NEONAlignment align, ARMReg Rm) {
	WriteVLDST1(true, Size, Vd, Rn, regCount, align, Rm);
}

void ARMXEmitter::VST1(u32 Size, ARMReg Vd, ARMReg Rn, int regCount, NEONAlignment align, ARMReg Rm) {
	WriteVLDST1(false, Size, Vd, Rn, regCount, align, Rm);
}

void ARMXEmitter::WriteVLDST1_lane(bool load, u32 Size, ARMReg Vd, ARMReg Rn, int lane, bool aligned, ARMReg Rm) 
{
	bool register_quad = Vd >= Q0;

	Vd = SubBase(Vd);
	// Support quad lanes by converting to D lanes
	if (register_quad && lane > 1) {
		Vd = (ARMReg)((int)Vd + 1);
		lane -= 2;
	}
	int encSize = encodedSize(Size);
	int index_align = 0;
	switch (encSize) {
	case 0: index_align = lane << 1; break;
	case 1: index_align = lane << 2; if (aligned) index_align |= 1; break;
	case 2: index_align = lane << 3; if (aligned) index_align |= 3; break;
	default:
		break;
	}

	Write32((0xF4 << 24) | (1 << 23) | ((Vd & 0x10) << 18) | (load << 21) | (Rn << 16)
		| ((Vd & 0xF) << 12) | (encSize << 10)
		| (index_align << 4) | Rm);
}

void ARMXEmitter::VLD1_lane(u32 Size, ARMReg Vd, ARMReg Rn, int lane, bool aligned, ARMReg Rm) {
	WriteVLDST1_lane(true, Size, Vd, Rn, lane, aligned, Rm);
}

void ARMXEmitter::VST1_lane(u32 Size, ARMReg Vd, ARMReg Rn, int lane, bool aligned, ARMReg Rm) {
	WriteVLDST1_lane(false, Size, Vd, Rn, lane, aligned, Rm);
}

void ARMXEmitter::VLD1_all_lanes(u32 Size, ARMReg Vd, ARMReg Rn, bool aligned, ARMReg Rm) {
	bool register_quad = Vd >= Q0;

	Vd = SubBase(Vd);

	int T = register_quad;  // two D registers

	Write32((0xF4 << 24) | (1 << 23) | ((Vd & 0x10) << 18) | (1 << 21) | (Rn << 16)
		| ((Vd & 0xF) << 12) | (0xC << 8) | (encodedSize(Size) << 6)
		| (T << 5) | (aligned << 4) | Rm);
}

/*
void ARMXEmitter::VLD2(u32 Size, ARMReg Vd, ARMReg Rn, int regCount, NEONAlignment align, ARMReg Rm)
{
	u32 spacing = 0x8; // Single spaced registers
	// Gets encoded as a double register
	Vd = SubBase(Vd);

	Write32((0xF4 << 24) | ((Vd & 0x10) << 18) | (1 << 21) | (Rn << 16)
			| ((Vd & 0xF) << 12) | (spacing << 8) | (encodedSize(Size) << 6)
			| (align << 4) | Rm);
}
*/

void ARMXEmitter::VREVX(u32 size, u32 Size, ARMReg Vd, ARMReg Vm)
{
	bool register_quad = Vd >= Q0;
	Vd = SubBase(Vd);
	Vm = SubBase(Vm);

	Write32((0xF3 << 24) | (1 << 23) | ((Vd & 0x10) << 18) | (0x3 << 20)
			| (encodedSize(Size) << 18) | ((Vd & 0xF) << 12) | (size << 7)
			| (register_quad << 6) | ((Vm & 0x10) << 1) | (Vm & 0xF));
}

void ARMXEmitter::VREV64(u32 Size, ARMReg Vd, ARMReg Vm)
{
	VREVX(0, Size, Vd, Vm);
}

void ARMXEmitter::VREV32(u32 Size, ARMReg Vd, ARMReg Vm)
{
	VREVX(1, Size, Vd, Vm);
}

void ARMXEmitter::VREV16(u32 Size, ARMReg Vd, ARMReg Vm)
{
	VREVX(2, Size, Vd, Vm);
}

// UNTESTED

// See page A8-878 in ARMv7-A Architecture Reference Manual

// Dest is a Q register, Src is a D register.
void ARMXEmitter::VCVTF32F16(ARMReg Dest, ARMReg Src) {
	_assert_msg_(JIT, cpu_info.bVFPv4, "Can't use half-float conversions when you don't support VFPv4");
	if (Dest < Q0 || Dest > Q15 || Src < D0 || Src > D15) {
		_assert_msg_(JIT, cpu_info.bNEON, "Bad inputs to VCVTF32F16"); 
		// Invalid!
	}

	Dest = SubBase(Dest);
	Src = SubBase(Src);
	
	int op = 1;
	Write32((0xF3B6 << 16) | ((Dest & 0x10) << 18) | ((Dest & 0xF) << 12) | 0x600 | (op << 8) | ((Src & 0x10) << 1) | (Src & 0xF));
}

// UNTESTED
// Dest is a D register, Src is a Q register.
void ARMXEmitter::VCVTF16F32(ARMReg Dest, ARMReg Src) {
	_assert_msg_(JIT, cpu_info.bVFPv4, "Can't use half-float conversions when you don't support VFPv4");
	if (Dest < D0 || Dest > D15 || Src < Q0 || Src > Q15) {
		_assert_msg_(JIT, cpu_info.bNEON, "Bad inputs to VCVTF32F16"); 
		// Invalid!
	}
	Dest = SubBase(Dest);
	Src = SubBase(Src);
	int op = 0;
	Write32((0xF3B6 << 16) | ((Dest & 0x10) << 18) | ((Dest & 0xF) << 12) | 0x600 | (op << 8) | ((Src & 0x10) << 1) | (Src & 0xF));
}

}
