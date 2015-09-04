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

// UnitTests
//
// This is a program to directly test various functions, without going
// through a PSP. Especially useful for things like opcode emitters,
// hashes, and various data conversion utility function.
//
// TODO: Make a test of nice unittest asserts and count successes etc.
// Or just integrate with an existing testing framework.


#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>

#include "base/NativeApp.h"
#include "Common/ArmEmitter.h"
#include "ext/disarm.h"
#include "math/math_util.h"
#include "util/text/parsers.h"

#define EXPECT_TRUE(a) if (!(a)) { printf(__FUNCTION__ ":%i: Test Fail\n", __LINE__); return false; }
#define EXPECT_FALSE(a) if ((a)) { printf(__FUNCTION__ ":%i: Test Fail\n", __LINE__); return false; }
#define EXPECT_EQ_FLOAT(a, b) if ((a) != (b)) { printf(__FUNCTION__ ":" __LINE__ ": Test Fail\n%f\nvs\n%f\n", a, b); return false; }
#define EXPECT_EQ_STR(a, b) if (a != b) { printf(__FUNCTION__ ": Test Fail\n%s\nvs\n%s\n", a.c_str(), b.c_str()); return false; }

#define RET(a) if (!(a)) { return false; }

std::string System_GetProperty(SystemProperty prop) { return ""; }

#define M_PI_2     1.57079632679489661923

// TODO:
// Fast approximate sincos for NEON
// http://blog.julien.cayzac.name/2009/12/fast-sinecosine-for-armv7neon.html
// Fast sincos
// http://www.dspguru.com/dsp/tricks/parabolic-approximation-of-sin-and-cos

// minimax (surprisingly terrible! something must be wrong)
// double asin_plus_sqrtthing = .9998421793 + (1.012386649 + (-.6575341673 + .8999841642 + (-1.669668977 + (1.571945105 - .5860008052 * x) * x) * x) * x) * x;

// VERY good. 6 MAD, one division.
// double asin_plus_sqrtthing = (1.807607311 + (.191900116 + (-2.511278506 + (1.062519236 + (-.3572142480 + .1087063463 * x) * x) * x) * x) * x) / (1.807601897 - 1.615203794 * x);
// float asin_plus_sqrtthing_correct_ends =
// 	(1.807607311f + (.191900116f + (-2.511278506f + (1.062519236f + (-.3572142480f + .1087063463f * x) * x) * x) * x) * x) / (1.807607311f - 1.615195094 * x);

// Unfortunately this is very serial.
// At least there are only 8 constants needed - load them into two low quads and go to town.
// For every step, VDUP the constant into a new register (out of two alternating), then VMLA or VFMA into it.


// http://www.ecse.rpi.edu/~wrf/Research/Short_Notes/arcsin/
// minimax polynomial rational approx, pretty good, get four digits consistently.
// unfortunately fastasin(1.0) / M_PI_2  != 1.0f, but it's pretty close.
float fastasin(double x) {
	float sign = x >= 0.0f ? 1.0f : -1.0f;
	x = fabs(x);
	float sqrtthing = sqrt(1.0f - x * x);
	// note that the sqrt can run parallel while we do the rest
	// if the hardware supports it

	float y = -.3572142480f + .1087063463f * x;
	y = y * x + 1.062519236f;
	y = y * x + -2.511278506f;
	y = y * x + .191900116f;
	y = y * x + 1.807607311f;
	y /= (1.807607311f - 1.615195094 * x);
	return sign * (y - sqrtthing);
}

double atan_66s(double x) { 
	const double c1=1.6867629106; 
	const double c2=0.4378497304; 
	const double c3=1.6867633134; 
	
	double x2; // The input argument squared 

	x2=x * x; 
	return (x*(c1 + x2*c2)/(c3 + x2));
}

// Terrible.
double fastasin2(double x) {
	return atan_66s(x / sqrt(1 - x * x));
}

// Also terrible.
float fastasin3(float x) {
	return x + x * x * x * x * x * 0.4971;
}

// Great! This is the one we'll use. Can be easily rescaled to get the right range for free.
// http://mathforum.org/library/drmath/view/54137.html
// http://www.musicdsp.org/showone.php?id=115
float fastasin4(float x) {
	float sign = x >= 0.0f ? 1.0f : -1.0f;
	x = fabs(x);
	x = M_PI/2 - sqrtf(1.0f - x) * (1.5707288 + -0.2121144*x + 0.0742610*x*x + -0.0187293*x*x*x);
	return sign * x;
}

// Or this:
float fastasin5(float x)
{
	float sign = x >= 0.0f ? 1.0f : -1.0f;
	x = fabs(x);
	float fRoot = sqrtf(1.0f - x);
	float fResult = 0.0742610f + -0.0187293f  * x;
	fResult = -0.2121144f + fResult * x;
	fResult = 1.5707288f + fResult * x;
	fResult = M_PI/2 - fRoot*fResult;
	return sign * fResult;
}


// This one is unfortunately not very good. But lets us avoid PI entirely
// thanks to the special arguments of the PSP functions.
// http://www.dspguru.com/dsp/tricks/parabolic-approximation-of-sin-and-cos
#define C            0.70710678118654752440f    // 1.0f / sqrt(2.0f)
// Some useful constants (PI and <math.h> are not part of algo)
#define BITSPERQUARTER (20)
void fcs(float angle, float &sinout, float &cosout) {
	int phasein = angle * (1 << BITSPERQUARTER);
	// Modulo phase into quarter, convert to float 0..1
	float modphase = (phasein & ((1<<BITSPERQUARTER)-1)) * (1.0f / (1<<BITSPERQUARTER));
	// Extract quarter bits 
	int quarter = phasein >> BITSPERQUARTER;
	// Recognize quarter
	if (!quarter) { 
		// First quarter, angle = 0 .. pi/2
		float x = modphase - 0.5f;      // 1 sub
		float temp = (2 - 4*C)*x*x + C; // 2 mul, 1 add
		sinout = temp + x;              // 1 add
		cosout = temp - x;              // 1 sub
	} else if (quarter == 1) {
		// Second quarter, angle = pi/2 .. pi
		float x = 0.5f - modphase;      // 1 sub
		float temp = (2 - 4*C)*x*x + C; // 2 mul, 1 add
		sinout = x + temp;              // 1 add
		cosout = x - temp;              // 1 sub
	} else if (quarter == 2) {
		// Third quarter, angle = pi .. 1.5pi
		float x = modphase - 0.5f;      // 1 sub
		float temp = (4*C - 2)*x*x - C; // 2 mul, 1 sub
		sinout = temp - x;              // 1 sub
		cosout = temp + x;              // 1 add
	} else if (quarter == 3) {
		// Fourth quarter, angle = 1.5pi..2pi
		float x = modphase - 0.5f;      // 1 sub
		float temp = (2 - 4*C)*x*x + C; // 2 mul, 1 add
		sinout = x - temp;              // 1 sub
		cosout = x + temp;              // 1 add
	}
}
#undef C


const float PI_SQR      = 9.86960440108935861883449099987615114f;

//https://code.google.com/p/math-neon/source/browse/trunk/math_floorf.c?r=18
// About 2 correct decimals. Not great.
void fcs2(float theta, float &outsine, float &outcosine) {
	float gamma = theta + 1;
	gamma += 2;
	gamma /= 4;
	theta += 2;
	theta /= 4;
	//theta -= (float)(int)theta;
	//gamma -= (float)(int)gamma;
	theta -= floorf(theta);
	gamma -= floorf(gamma);
	theta *= 4;
	theta -= 2;
	gamma *= 4;
	gamma -= 2;

	const float B = 2;

	float x = 2 * gamma - gamma * abs(gamma);
	float y = 2 * theta - theta * abs(theta);
	const float P = 0.225;
	outsine = P * (y * abs(y) - y) + y;   // Q * y + P * y * abs(y)
	outcosine = P * (x * abs(x) - x) + x;   // Q * y + P * y * abs(y)
}



void fastsincos(float x, float &sine, float &cosine) {
	fcs2(x, sine, cosine);
}

bool TestSinCos() {
	for (int i = -100; i <= 100; i++) {
		float f = i / 30.0f;

		// The PSP sin/cos take as argument angle * M_PI_2.
		// We need to match that.
		float slowsin = sinf(f * M_PI_2), slowcos = cosf(f * M_PI_2);
		float fastsin, fastcos;
		fastsincos(f, fastsin, fastcos);
		printf("%f: slow: %0.8f, %0.8f fast: %0.8f, %0.8f\n", f, slowsin, slowcos, fastsin, fastcos);
	}
	return true;
}


bool TestAsin() {
	for (int i = -100; i <= 100; i++) {
		float f = i / 100.0f;
		float slowval = asinf(f) / M_PI_2;
		float fastval = fastasin5(f) / M_PI_2;
		printf("slow: %0.16f fast: %0.16f\n", slowval, fastval);
		float diff = fabsf(slowval - fastval);
		// EXPECT_TRUE(diff < 0.0001f);
	}
	// EXPECT_TRUE(fastasin(1.0) / M_PI_2 <= 1.0f);
	return true;
}



bool CheckLast(ArmGen::ARMXEmitter &emit, const char *comp) {
	u32 instr;
	memcpy(&instr, emit.GetCodePtr() - 4, 4);
	char disasm[512];
	ArmDis(0, instr, disasm);
	EXPECT_EQ_STR(std::string(disasm), std::string(comp));
	return true;
}


bool TestArmEmitter() {
	using namespace ArmGen;

	u32 code[512];
	ARMXEmitter emitter((u8 *)code);
	emitter.LDR(R3, R7);
	RET(CheckLast(emitter, "e5973000 LDR r3, [r7, #0]"));
	emitter.VLDR(S3, R8, 48);
	RET(CheckLast(emitter, "edd81a0c VLDR s3, [r8, #48]"));
	emitter.VSTR(S5, R12, -36);
	RET(CheckLast(emitter, "ed4c2a09 VSTR s5, [r12, #-36]"));
	emitter.VADD(S1, S2, S3);
	RET(CheckLast(emitter, "ee710a21 VADD s1, s2, s3"));
	emitter.VMUL(S7, S8, S9);
	RET(CheckLast(emitter, "ee643a24 VMUL s7, s8, s9"));
	emitter.VMLA(S7, S8, S9);
	RET(CheckLast(emitter, "ee443a24 VMLA s7, s8, s9"));
	emitter.VNMLA(S7, S8, S9);
	RET(CheckLast(emitter, "ee543a64 VNMLA s7, s8, s9"));
	emitter.VABS(S1, S2);
	RET(CheckLast(emitter, "eef00ac1 VABS s1, s2"));
	emitter.VSQRT(S1, S2);
	RET(CheckLast(emitter, "eef10ac1 VSQRT s1, s2"));
	emitter.VDIV(S1, S2, S3);
	RET(CheckLast(emitter, "eec10a21 VDIV s1, s2, s3"));
	emitter.VMRS(R1);
	RET(CheckLast(emitter, "eef11a10 VMRS r1"));
	emitter.VMSR(R7);
	RET(CheckLast(emitter, "eee17a10 VMSR r7"));
	emitter.VMRS_APSR();
	RET(CheckLast(emitter, "eef1fa10 VMRS APSR"));
	emitter.VCVT(S0, S1, TO_INT | IS_SIGNED);
	RET(CheckLast(emitter, "eebd0a60 VCVT ..."));


	// WTF?
	//emitter.VSUB(S4, S5, S6);
	//RET(CheckLast(emitter, "ee322ac3 VSUB s4, s5, s6"));


	emitter.VMOV(S3, S6);
	RET(CheckLast(emitter, "eef01a43 VMOV s3, s6"));
	return true;
}

bool TestMathUtil() {
	EXPECT_FALSE(my_isinf(1.0));
	volatile float zero = 0.0f;
	EXPECT_TRUE(my_isinf(1.0f/zero));
	EXPECT_FALSE(my_isnan(1.0f/zero));
	return true;
}

bool TestParsers() {
	const char *macstr = "01:02:03:ff:fe:fd";
	uint8_t mac[6];
	ParseMacAddress(macstr, mac);
	EXPECT_TRUE(mac[0] == 1);
	EXPECT_TRUE(mac[1] == 2);
	EXPECT_TRUE(mac[2] == 3);
	EXPECT_TRUE(mac[3] == 255);
	EXPECT_TRUE(mac[4] == 254);
	EXPECT_TRUE(mac[5] == 253);
	return true;
}

int main(int argc, const char *argv[])
{
	TestAsin();
	//TestSinCos();
	//TestArmEmitter();
	TestMathUtil();
	TestParsers();
	return 0;
}