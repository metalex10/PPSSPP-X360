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

#include "base/basictypes.h"
#include "Windows/resource.h"
#include "Windows/InputBox.h"
#include "Windows/GEDebugger/GEDebugger.h"
#include "Windows/GEDebugger/TabState.h"
#include "GPU/GPUState.h"
#include "GPU/GeDisasm.h"
#include "GPU/Common/GPUDebugInterface.h"

// TODO: Show an icon or something for breakpoints, toggle.
static const GenericListViewColumn stateValuesCols[] = {
	{ L"Name", 0.50f },
	{ L"Value", 0.50f },
};

enum StateValuesCols {
	STATEVALUES_COL_NAME,
	STATEVALUES_COL_VALUE,
};

enum CmdFormatType {
	CMD_FMT_HEX = 0,
	CMD_FMT_NUM,
	CMD_FMT_FLOAT24,
	CMD_FMT_PTRWIDTH,
	CMD_FMT_XY,
	CMD_FMT_XYXY,
	CMD_FMT_XYZ,
	CMD_FMT_TEXSIZE,
	CMD_FMT_F16_XY,
	CMD_FMT_VERTEXTYPE,
	CMD_FMT_TEXFMT,
	CMD_FMT_CLUTFMT,
	CMD_FMT_COLORTEST,
	CMD_FMT_ALPHATEST,
	CMD_FMT_STENCILTEST,
	CMD_FMT_ZTEST,
	CMD_FMT_OFFSETADDR,
	CMD_FMT_VADDR,
	CMD_FMT_IADDR,
	CMD_FMT_MATERIALUPDATE,
	CMD_FMT_STENCILOP,
	CMD_FMT_BLENDMODE,
	CMD_FMT_FLAG,
	CMD_FMT_CLEARMODE,
	CMD_FMT_TEXFUNC,
	CMD_FMT_TEXMODE,
	CMD_FMT_LOGICOP,
	CMD_FMT_TEXWRAP,
	CMD_FMT_TEXFILTER,
	CMD_FMT_TEXMAPMODE,
};

struct TabStateRow {
	const TCHAR *title;
	u8 cmd;
	CmdFormatType fmt;
	u8 enableCmd;
	u8 otherCmd;
	u8 otherCmd2;
};

static const TabStateRow stateFlagsRows[] = {
	{ L"Lighting enable",      GE_CMD_LIGHTINGENABLE,          CMD_FMT_FLAG },
	{ L"Light 0 enable",       GE_CMD_LIGHTENABLE0,            CMD_FMT_FLAG },
	{ L"Light 1 enable",       GE_CMD_LIGHTENABLE1,            CMD_FMT_FLAG },
	{ L"Light 2 enable",       GE_CMD_LIGHTENABLE2,            CMD_FMT_FLAG },
	{ L"Light 3 enable",       GE_CMD_LIGHTENABLE3,            CMD_FMT_FLAG },
	{ L"Clip enable",          GE_CMD_CLIPENABLE,              CMD_FMT_FLAG },
	{ L"Cullface enable",      GE_CMD_CULLFACEENABLE,          CMD_FMT_FLAG },
	{ L"Texture map enable",   GE_CMD_TEXTUREMAPENABLE,        CMD_FMT_FLAG },
	{ L"Fog enable",           GE_CMD_FOGENABLE,               CMD_FMT_FLAG },
	{ L"Dither enable",        GE_CMD_DITHERENABLE,            CMD_FMT_FLAG },
	{ L"Alpha blend enable",   GE_CMD_ALPHABLENDENABLE,        CMD_FMT_FLAG },
	{ L"Alpha test enable",    GE_CMD_ALPHATESTENABLE,         CMD_FMT_FLAG },
	{ L"Depth test enable",    GE_CMD_ZTESTENABLE,             CMD_FMT_FLAG },
	{ L"Stencil test enable",  GE_CMD_STENCILTESTENABLE,       CMD_FMT_FLAG },
	{ L"Antialias enable",     GE_CMD_ANTIALIASENABLE,         CMD_FMT_FLAG },
	{ L"Patch cull enable",    GE_CMD_PATCHCULLENABLE,         CMD_FMT_FLAG },
	{ L"Color test enable",    GE_CMD_COLORTESTENABLE,         CMD_FMT_FLAG },
	{ L"Logic op enable",      GE_CMD_LOGICOPENABLE,           CMD_FMT_FLAG },
	{ L"Depth write disable",  GE_CMD_ZWRITEDISABLE,           CMD_FMT_FLAG },
};

static const TabStateRow stateLightingRows[] = {
	{ L"Ambient color",        GE_CMD_AMBIENTCOLOR,            CMD_FMT_HEX },
	{ L"Ambient alpha",        GE_CMD_AMBIENTALPHA,            CMD_FMT_HEX },
	{ L"Material update",      GE_CMD_MATERIALUPDATE,          CMD_FMT_MATERIALUPDATE },
	{ L"Material emissive",    GE_CMD_MATERIALEMISSIVE,        CMD_FMT_HEX },
	{ L"Material ambient",     GE_CMD_MATERIALAMBIENT,         CMD_FMT_HEX },
	{ L"Material diffuse",     GE_CMD_MATERIALDIFFUSE,         CMD_FMT_HEX },
	{ L"Material alpha",       GE_CMD_MATERIALALPHA,           CMD_FMT_HEX },
	{ L"Material specular",    GE_CMD_MATERIALSPECULAR,        CMD_FMT_HEX },
	{ L"Mat. specular coef",   GE_CMD_MATERIALSPECULARCOEF,    CMD_FMT_FLOAT24 },
	{ L"Reverse normals",      GE_CMD_REVERSENORMAL,           CMD_FMT_FLAG },
	// TODO: Format?
	{ L"Shade model",          GE_CMD_SHADEMODE,               CMD_FMT_NUM },
	// TODO: Format?
	{ L"Light mode",           GE_CMD_LIGHTMODE,               CMD_FMT_NUM, GE_CMD_LIGHTINGENABLE },
	{ L"Light type 0",         GE_CMD_LIGHTTYPE0,              CMD_FMT_NUM, GE_CMD_LIGHTENABLE0 },
	{ L"Light type 1",         GE_CMD_LIGHTTYPE1,              CMD_FMT_NUM, GE_CMD_LIGHTENABLE1 },
	{ L"Light type 2",         GE_CMD_LIGHTTYPE2,              CMD_FMT_NUM, GE_CMD_LIGHTENABLE2 },
	{ L"Light type 3",         GE_CMD_LIGHTTYPE3,              CMD_FMT_NUM, GE_CMD_LIGHTENABLE3 },
	{ L"Light pos 0",          GE_CMD_LX0,                     CMD_FMT_XYZ, GE_CMD_LIGHTENABLE0, GE_CMD_LY0, GE_CMD_LZ0 },
	{ L"Light pos 1",          GE_CMD_LX1,                     CMD_FMT_XYZ, GE_CMD_LIGHTENABLE1, GE_CMD_LY1, GE_CMD_LZ1 },
	{ L"Light pos 2",          GE_CMD_LX2,                     CMD_FMT_XYZ, GE_CMD_LIGHTENABLE2, GE_CMD_LY2, GE_CMD_LZ2 },
	{ L"Light pos 3",          GE_CMD_LX3,                     CMD_FMT_XYZ, GE_CMD_LIGHTENABLE3, GE_CMD_LY3, GE_CMD_LZ3 },
	{ L"Light dir 0",          GE_CMD_LDX0,                    CMD_FMT_XYZ, GE_CMD_LIGHTENABLE0, GE_CMD_LDY0, GE_CMD_LDZ0 },
	{ L"Light dir 1",          GE_CMD_LDX1,                    CMD_FMT_XYZ, GE_CMD_LIGHTENABLE1, GE_CMD_LDY1, GE_CMD_LDZ1 },
	{ L"Light dir 2",          GE_CMD_LDX2,                    CMD_FMT_XYZ, GE_CMD_LIGHTENABLE2, GE_CMD_LDY2, GE_CMD_LDZ2 },
	{ L"Light dir 3",          GE_CMD_LDX3,                    CMD_FMT_XYZ, GE_CMD_LIGHTENABLE3, GE_CMD_LDY3, GE_CMD_LDZ3 },
	// TODO: Is this a reasonable display format?
	{ L"Light att 0",          GE_CMD_LKA0,                    CMD_FMT_XYZ, GE_CMD_LIGHTENABLE0, GE_CMD_LKB0, GE_CMD_LKC0 },
	{ L"Light att 1",          GE_CMD_LKA1,                    CMD_FMT_XYZ, GE_CMD_LIGHTENABLE1, GE_CMD_LKB1, GE_CMD_LKC1 },
	{ L"Light att 2",          GE_CMD_LKA2,                    CMD_FMT_XYZ, GE_CMD_LIGHTENABLE2, GE_CMD_LKB2, GE_CMD_LKC2 },
	{ L"Light att 3",          GE_CMD_LKA3,                    CMD_FMT_XYZ, GE_CMD_LIGHTENABLE3, GE_CMD_LKB3, GE_CMD_LKC3 },
	{ L"Lightspot coef 0",     GE_CMD_LKS0,                    CMD_FMT_FLOAT24, GE_CMD_LIGHTENABLE0 },
	{ L"Lightspot coef 1",     GE_CMD_LKS1,                    CMD_FMT_FLOAT24, GE_CMD_LIGHTENABLE1 },
	{ L"Lightspot coef 2",     GE_CMD_LKS2,                    CMD_FMT_FLOAT24, GE_CMD_LIGHTENABLE2 },
	{ L"Lightspot coef 3",     GE_CMD_LKS3,                    CMD_FMT_FLOAT24, GE_CMD_LIGHTENABLE3 },
	{ L"Light angle 0",        GE_CMD_LKO0,                    CMD_FMT_FLOAT24, GE_CMD_LIGHTENABLE0 },
	{ L"Light angle 1",        GE_CMD_LKO1,                    CMD_FMT_FLOAT24, GE_CMD_LIGHTENABLE1 },
	{ L"Light angle 2",        GE_CMD_LKO2,                    CMD_FMT_FLOAT24, GE_CMD_LIGHTENABLE2 },
	{ L"Light angle 3",        GE_CMD_LKO3,                    CMD_FMT_FLOAT24, GE_CMD_LIGHTENABLE3 },
	{ L"Light ambient 0",      GE_CMD_LAC0,                    CMD_FMT_HEX, GE_CMD_LIGHTENABLE0 },
	{ L"Light diffuse 0",      GE_CMD_LDC0,                    CMD_FMT_HEX, GE_CMD_LIGHTENABLE0 },
	{ L"Light specular 0",     GE_CMD_LSC0,                    CMD_FMT_HEX, GE_CMD_LIGHTENABLE0 },
	{ L"Light ambient 1",      GE_CMD_LAC1,                    CMD_FMT_HEX, GE_CMD_LIGHTENABLE1 },
	{ L"Light diffuse 1",      GE_CMD_LDC1,                    CMD_FMT_HEX, GE_CMD_LIGHTENABLE1 },
	{ L"Light specular 1",     GE_CMD_LSC1,                    CMD_FMT_HEX, GE_CMD_LIGHTENABLE1 },
	{ L"Light ambient 2",      GE_CMD_LAC2,                    CMD_FMT_HEX, GE_CMD_LIGHTENABLE2 },
	{ L"Light diffuse 2",      GE_CMD_LDC2,                    CMD_FMT_HEX, GE_CMD_LIGHTENABLE2 },
	{ L"Light specular 2",     GE_CMD_LSC2,                    CMD_FMT_HEX, GE_CMD_LIGHTENABLE2 },
	{ L"Light ambient 3",      GE_CMD_LAC3,                    CMD_FMT_HEX, GE_CMD_LIGHTENABLE3 },
	{ L"Light diffuse 3",      GE_CMD_LDC3,                    CMD_FMT_HEX, GE_CMD_LIGHTENABLE3 },
	{ L"Light specular 3",     GE_CMD_LSC3,                    CMD_FMT_HEX, GE_CMD_LIGHTENABLE3 },
};

static const TabStateRow stateTextureRows[] = {
	{ L"Tex U scale",          GE_CMD_TEXSCALEU,               CMD_FMT_FLOAT24, GE_CMD_TEXTUREMAPENABLE },
	{ L"Tex V scale",          GE_CMD_TEXSCALEV,               CMD_FMT_FLOAT24, GE_CMD_TEXTUREMAPENABLE },
	{ L"Tex U offset",         GE_CMD_TEXOFFSETU,              CMD_FMT_FLOAT24, GE_CMD_TEXTUREMAPENABLE },
	{ L"Tex V offset",         GE_CMD_TEXOFFSETV,              CMD_FMT_FLOAT24, GE_CMD_TEXTUREMAPENABLE },
	{ L"Tex mapping mode",     GE_CMD_TEXMAPMODE,              CMD_FMT_TEXMAPMODE, GE_CMD_TEXTUREMAPENABLE },
	// TODO: Format.
	{ L"Tex shade srcs",       GE_CMD_TEXSHADELS,              CMD_FMT_HEX, GE_CMD_TEXTUREMAPENABLE },
	{ L"Tex mode",             GE_CMD_TEXMODE,                 CMD_FMT_TEXMODE, GE_CMD_TEXTUREMAPENABLE },
	{ L"Tex format",           GE_CMD_TEXFORMAT,               CMD_FMT_TEXFMT, GE_CMD_TEXTUREMAPENABLE },
	{ L"Tex filtering",        GE_CMD_TEXFILTER,               CMD_FMT_TEXFILTER, GE_CMD_TEXTUREMAPENABLE },
	{ L"Tex wrapping",         GE_CMD_TEXWRAP,                 CMD_FMT_TEXWRAP, GE_CMD_TEXTUREMAPENABLE },
	// TODO: Format.
	{ L"Tex level/bias",       GE_CMD_TEXLEVEL,                CMD_FMT_HEX, GE_CMD_TEXTUREMAPENABLE },
	// TODO: Format.
	{ L"Tex lod slope",        GE_CMD_TEXLODSLOPE,             CMD_FMT_HEX, GE_CMD_TEXTUREMAPENABLE },
	{ L"Tex func",             GE_CMD_TEXFUNC,                 CMD_FMT_TEXFUNC, GE_CMD_TEXTUREMAPENABLE },
	{ L"Tex env color",        GE_CMD_TEXENVCOLOR,             CMD_FMT_HEX, GE_CMD_TEXTUREMAPENABLE },
	{ L"CLUT",                 GE_CMD_CLUTADDR,                CMD_FMT_PTRWIDTH, GE_CMD_TEXTUREMAPENABLE, GE_CMD_CLUTADDRUPPER },
	{ L"CLUT format",          GE_CMD_CLUTFORMAT,              CMD_FMT_CLUTFMT, GE_CMD_TEXTUREMAPENABLE },
	{ L"Texture L0 addr",      GE_CMD_TEXADDR0,                CMD_FMT_PTRWIDTH, GE_CMD_TEXTUREMAPENABLE, GE_CMD_TEXBUFWIDTH0 },
	{ L"Texture L1 addr",      GE_CMD_TEXADDR1,                CMD_FMT_PTRWIDTH, GE_CMD_TEXTUREMAPENABLE, GE_CMD_TEXBUFWIDTH1 },
	{ L"Texture L2 addr",      GE_CMD_TEXADDR2,                CMD_FMT_PTRWIDTH, GE_CMD_TEXTUREMAPENABLE, GE_CMD_TEXBUFWIDTH2 },
	{ L"Texture L3 addr",      GE_CMD_TEXADDR3,                CMD_FMT_PTRWIDTH, GE_CMD_TEXTUREMAPENABLE, GE_CMD_TEXBUFWIDTH3 },
	{ L"Texture L4 addr",      GE_CMD_TEXADDR4,                CMD_FMT_PTRWIDTH, GE_CMD_TEXTUREMAPENABLE, GE_CMD_TEXBUFWIDTH4 },
	{ L"Texture L5 addr",      GE_CMD_TEXADDR5,                CMD_FMT_PTRWIDTH, GE_CMD_TEXTUREMAPENABLE, GE_CMD_TEXBUFWIDTH5 },
	{ L"Texture L6 addr",      GE_CMD_TEXADDR6,                CMD_FMT_PTRWIDTH, GE_CMD_TEXTUREMAPENABLE, GE_CMD_TEXBUFWIDTH6 },
	{ L"Texture L7 addr",      GE_CMD_TEXADDR7,                CMD_FMT_PTRWIDTH, GE_CMD_TEXTUREMAPENABLE, GE_CMD_TEXBUFWIDTH7 },
	{ L"Texture L0 size",      GE_CMD_TEXSIZE0,                CMD_FMT_TEXSIZE, GE_CMD_TEXTUREMAPENABLE },
	{ L"Texture L1 size",      GE_CMD_TEXSIZE1,                CMD_FMT_TEXSIZE, GE_CMD_TEXTUREMAPENABLE },
	{ L"Texture L2 size",      GE_CMD_TEXSIZE2,                CMD_FMT_TEXSIZE, GE_CMD_TEXTUREMAPENABLE },
	{ L"Texture L3 size",      GE_CMD_TEXSIZE3,                CMD_FMT_TEXSIZE, GE_CMD_TEXTUREMAPENABLE },
	{ L"Texture L4 size",      GE_CMD_TEXSIZE4,                CMD_FMT_TEXSIZE, GE_CMD_TEXTUREMAPENABLE },
	{ L"Texture L5 size",      GE_CMD_TEXSIZE5,                CMD_FMT_TEXSIZE, GE_CMD_TEXTUREMAPENABLE },
	{ L"Texture L6 size",      GE_CMD_TEXSIZE6,                CMD_FMT_TEXSIZE, GE_CMD_TEXTUREMAPENABLE },
	{ L"Texture L7 size",      GE_CMD_TEXSIZE7,                CMD_FMT_TEXSIZE, GE_CMD_TEXTUREMAPENABLE },
};

static const TabStateRow stateSettingsRows[] = {
	{ L"Clear mode",           GE_CMD_CLEARMODE,               CMD_FMT_CLEARMODE },
	{ L"Framebuffer",          GE_CMD_FRAMEBUFPTR,             CMD_FMT_PTRWIDTH, 0, GE_CMD_FRAMEBUFWIDTH },
	{ L"Framebuffer format",   GE_CMD_FRAMEBUFPIXFORMAT,       CMD_FMT_TEXFMT },
	{ L"Depthbuffer",          GE_CMD_ZBUFPTR,                 CMD_FMT_PTRWIDTH, 0, GE_CMD_ZBUFWIDTH },
	{ L"Vertex type",          GE_CMD_VERTEXTYPE,              CMD_FMT_VERTEXTYPE },
	{ L"Offset addr",          GE_CMD_OFFSETADDR,              CMD_FMT_OFFSETADDR },
	{ L"Vertex addr",          GE_CMD_VADDR,                   CMD_FMT_VADDR },
	{ L"Index addr",           GE_CMD_IADDR,                   CMD_FMT_IADDR },
	{ L"Region",               GE_CMD_REGION1,                 CMD_FMT_XYXY, 0, GE_CMD_REGION2 },
	{ L"Scissor",              GE_CMD_SCISSOR1,                CMD_FMT_XYXY, 0, GE_CMD_SCISSOR2 },
	{ L"Min Z",                GE_CMD_MINZ,                    CMD_FMT_HEX },
	{ L"Max Z",                GE_CMD_MAXZ,                    CMD_FMT_HEX },
	{ L"Viewport 1",           GE_CMD_VIEWPORTX1,              CMD_FMT_XYZ, 0, GE_CMD_VIEWPORTY1, GE_CMD_VIEWPORTZ1 },
	{ L"Viewport 2",           GE_CMD_VIEWPORTX2,              CMD_FMT_XYZ, 0, GE_CMD_VIEWPORTY2, GE_CMD_VIEWPORTZ2 },
	{ L"Offset",               GE_CMD_OFFSETX,                 CMD_FMT_F16_XY, 0, GE_CMD_OFFSETY },
	// TODO: Format.
	{ L"Cull mode",            GE_CMD_CULL,                    CMD_FMT_NUM, GE_CMD_CULLFACEENABLE },
	{ L"Color test",           GE_CMD_COLORTEST,               CMD_FMT_COLORTEST, GE_CMD_COLORTESTENABLE, GE_CMD_COLORREF, GE_CMD_COLORTESTMASK },
	{ L"Alpha test",           GE_CMD_ALPHATEST,               CMD_FMT_ALPHATEST, GE_CMD_ALPHATESTENABLE },
	{ L"Stencil test",         GE_CMD_STENCILTEST,             CMD_FMT_STENCILTEST, GE_CMD_STENCILTESTENABLE },
	{ L"Stencil test op",      GE_CMD_STENCILOP,               CMD_FMT_STENCILOP, GE_CMD_STENCILTESTENABLE },
	{ L"Depth test",           GE_CMD_ZTEST,                   CMD_FMT_ZTEST, GE_CMD_ZTESTENABLE },
	{ L"Alpha blend mode",     GE_CMD_BLENDMODE,               CMD_FMT_BLENDMODE, GE_CMD_ALPHABLENDENABLE },
	{ L"Blend color A",        GE_CMD_BLENDFIXEDA,             CMD_FMT_HEX, GE_CMD_ALPHABLENDENABLE },
	{ L"Blend color B",        GE_CMD_BLENDFIXEDB,             CMD_FMT_HEX, GE_CMD_ALPHABLENDENABLE },
	{ L"Logic Op",             GE_CMD_LOGICOP,                 CMD_FMT_LOGICOP, GE_CMD_LOGICOPENABLE },
	{ L"Fog 1",                GE_CMD_FOG1,                    CMD_FMT_FLOAT24, GE_CMD_FOGENABLE },
	{ L"Fog 2",                GE_CMD_FOG2,                    CMD_FMT_FLOAT24, GE_CMD_FOGENABLE },
	{ L"Fog color",            GE_CMD_FOGCOLOR,                CMD_FMT_HEX, GE_CMD_FOGENABLE },
	{ L"RGB mask",             GE_CMD_MASKRGB,                 CMD_FMT_HEX },
	{ L"Stencil/alpha mask",   GE_CMD_MASKALPHA,               CMD_FMT_HEX },
	{ L"Morph Weight 0",       GE_CMD_MORPHWEIGHT0,            CMD_FMT_FLOAT24 },
	{ L"Morph Weight 1",       GE_CMD_MORPHWEIGHT1,            CMD_FMT_FLOAT24 },
	{ L"Morph Weight 2",       GE_CMD_MORPHWEIGHT2,            CMD_FMT_FLOAT24 },
	{ L"Morph Weight 3",       GE_CMD_MORPHWEIGHT3,            CMD_FMT_FLOAT24 },
	{ L"Morph Weight 4",       GE_CMD_MORPHWEIGHT4,            CMD_FMT_FLOAT24 },
	{ L"Morph Weight 5",       GE_CMD_MORPHWEIGHT5,            CMD_FMT_FLOAT24 },
	{ L"Morph Weight 6",       GE_CMD_MORPHWEIGHT6,            CMD_FMT_FLOAT24 },
	{ L"Morph Weight 7",       GE_CMD_MORPHWEIGHT7,            CMD_FMT_FLOAT24 },
	// TODO: Enabled?  Formats?
	{ L"Patch division",       GE_CMD_PATCHDIVISION,           CMD_FMT_HEX },
	{ L"Patch primitive",      GE_CMD_PATCHPRIMITIVE,          CMD_FMT_HEX },
	{ L"Patch facing",         GE_CMD_PATCHFACING,             CMD_FMT_HEX },
	{ L"Dither 0",             GE_CMD_DITH0,                   CMD_FMT_HEX, GE_CMD_DITHERENABLE },
	{ L"Dither 1",             GE_CMD_DITH1,                   CMD_FMT_HEX, GE_CMD_DITHERENABLE },
	{ L"Dither 2",             GE_CMD_DITH2,                   CMD_FMT_HEX, GE_CMD_DITHERENABLE },
	{ L"Dither 3",             GE_CMD_DITH3,                   CMD_FMT_HEX, GE_CMD_DITHERENABLE },
	{ L"Transfer src",         GE_CMD_TRANSFERSRC,             CMD_FMT_PTRWIDTH, 0, GE_CMD_TRANSFERSRCW },
	{ L"Transfer src pos",     GE_CMD_TRANSFERSRCPOS,          CMD_FMT_XY },
	{ L"Transfer dst",         GE_CMD_TRANSFERDST,             CMD_FMT_PTRWIDTH, 0, GE_CMD_TRANSFERDSTW },
	{ L"Transfer dst pos",     GE_CMD_TRANSFERDSTPOS,          CMD_FMT_XY },
	{ L"Transfer size",        GE_CMD_TRANSFERSIZE,            CMD_FMT_XY },
};

// TODO: Commands not present in the above lists (some because they don't have meaningful values...):
//   GE_CMD_PRIM, GE_CMD_BEZIER, GE_CMD_SPLINE, GE_CMD_BOUNDINGBOX,
//   GE_CMD_JUMP, GE_CMD_BJUMP, GE_CMD_CALL, GE_CMD_RET, GE_CMD_END, GE_CMD_SIGNAL, GE_CMD_FINISH,
//   GE_CMD_BONEMATRIXNUMBER, GE_CMD_BONEMATRIXDATA, GE_CMD_WORLDMATRIXNUMBER, GE_CMD_WORLDMATRIXDATA,
//   GE_CMD_VIEWMATRIXNUMBER, GE_CMD_VIEWMATRIXDATA, GE_CMD_PROJMATRIXNUMBER, GE_CMD_PROJMATRIXDATA,
//   GE_CMD_TGENMATRIXNUMBER, GE_CMD_TGENMATRIXDATA,
//   GE_CMD_LOADCLUT, GE_CMD_TEXFLUSH, GE_CMD_TEXSYNC,
//   GE_CMD_TRANSFERSTART,
//   GE_CMD_UNKNOWN_*

CtrlStateValues::CtrlStateValues(const TabStateRow *rows, int rowCount, HWND hwnd)
	: GenericListControl(hwnd, stateValuesCols, ARRAY_SIZE(stateValuesCols)),
	  rows_(rows), rowCount_(rowCount) {
	Update();
}

void FormatStateRow(wchar_t *dest, const TabStateRow &info, u32 value, bool enabled, u32 otherValue, u32 otherValue2) {
	switch (info.fmt) {
	case CMD_FMT_HEX:
		swprintf(dest, L"%06x", value);
		break;

	case CMD_FMT_NUM:
		swprintf(dest, L"%d", value);
		break;

	case CMD_FMT_FLOAT24:
		swprintf(dest, L"%f", getFloat24(value));
		break;

	case CMD_FMT_PTRWIDTH:
		value |= (otherValue & 0x00FF0000) << 8;
		otherValue &= 0xFFFF;
		swprintf(dest, L"%08x, w=%d", value, otherValue);
		break;

	case CMD_FMT_XY:
		{
			int x = value & 0x3FF;
			int y = value >> 10;
			swprintf(dest, L"%d,%d", x, y);
		}
		break;

	case CMD_FMT_XYXY:
		{
			int x1 = value & 0x3FF;
			int y1 = value >> 10;
			int x2 = otherValue & 0x3FF;
			int y2 = otherValue >> 10;
			swprintf(dest, L"%d,%d - %d,%d", x1, y1, x2, y2);
		}
		break;

	case CMD_FMT_XYZ:
		{
			float x = getFloat24(value);
			float y = getFloat24(otherValue);
			float z = getFloat24(otherValue2);
			swprintf(dest, L"%f, %f, %f", x, y, z);
		}
		break;

	case CMD_FMT_TEXSIZE:
		{
			int w = 1 << (value & 0x1f);
			int h = 1 << ((value >> 8) & 0x1f);
			swprintf(dest, L"%dx%d", w, h);
		}
		break;

	case CMD_FMT_F16_XY:
		{
			float x = (float)value / 16.0f;
			float y = (float)otherValue / 16.0f;
			swprintf(dest, L"%fx%f", x, y);
		}
		break;

	case CMD_FMT_VERTEXTYPE:
		{
			char buffer[256];
			GeDescribeVertexType(value, buffer);
			swprintf(dest, L"%S", buffer);
		}
		break;

	case CMD_FMT_TEXFMT:
		{
			static const char *texformats[] = {
				"5650",
				"5551",
				"4444",
				"8888",
				"CLUT4",
				"CLUT8",
				"CLUT16",
				"CLUT32",
				"DXT1",
				"DXT3",
				"DXT5",
			};
			if (value < (u32)ARRAY_SIZE(texformats)) {
				swprintf(dest, L"%S", texformats[value]);
			} else {
				swprintf(dest, L"%06x", value);
			}
		}
		break;

	case CMD_FMT_CLUTFMT:
		{
			const char *clutformats[] = {
				"BGR 5650",
				"ABGR 1555",
				"ABGR 4444",
				"ABGR 8888",
			};
			const u8 palette = (value >> 0) & 0xFF;
			const u8 mask = (value >> 8) & 0xFF;
			const u8 offset = (value >> 16) & 0xFF;
			if (palette < (u8)ARRAY_SIZE(clutformats) && offset < 0x20) {
				if (offset == 0) {
					swprintf(dest, L"%S & %02x", clutformats[palette], mask);
				} else {
					swprintf(dest, L"%S & %02x, offset +%d", clutformats[palette], mask, offset);
				}
			} else {
				swprintf(dest, L"%06x", value);
			}
		}
		break;

	case CMD_FMT_COLORTEST:
		{
			static const char *colorTests[] = {"NEVER", "ALWAYS", " == ", " != "};
			const u32 mask = otherValue2;
			const u32 ref = otherValue;
			if (value < (u32)ARRAY_SIZE(colorTests)) {
				swprintf(dest, L"pass if (c & %06x) %S (%06x & %06x)", mask, colorTests[value], ref, mask);
			} else {
				swprintf(dest, L"%06x, ref=%06x, maks=%06x", value, ref, mask);
			}
		}
		break;

	case CMD_FMT_ALPHATEST:
	case CMD_FMT_STENCILTEST:
		{
			static const char *alphaTestFuncs[] = { "NEVER", "ALWAYS", "==", "!=", "<", "<=", ">", ">=" };
			const u8 mask = (value >> 16) & 0xff;
			const u8 ref = (value >> 8) & 0xff;
			const u8 func = (value >> 0) & 0xff;
			if (func < (u8)ARRAY_SIZE(alphaTestFuncs)) {
				if (info.fmt == CMD_FMT_ALPHATEST) {
					swprintf(dest, L"pass if (a & %02x) %S (%02x & %02x)", mask, alphaTestFuncs[func], ref, mask);
				} else if (info.fmt == CMD_FMT_STENCILTEST) {
					// Stencil test is the other way around.
					swprintf(dest, L"pass if (%02x & %02x) %S (a & %02x)", ref, mask, alphaTestFuncs[func], mask);
				}
			} else {
				swprintf(dest, L"%06x", value);
			}
		}
		break;

	case CMD_FMT_ZTEST:
		{
			static const char *zTestFuncs[] = { "NEVER", "ALWAYS", "==", "!=", "<", "<=", ">", ">=" };
			if (value < (u32)ARRAY_SIZE(zTestFuncs)) {
				swprintf(dest, L"pass if src %S dst", zTestFuncs[value]);
			} else {
				swprintf(dest, L"%06x", value);
			}
		}
		break;

	case CMD_FMT_OFFSETADDR:
		swprintf(dest, L"%08x", gpuDebug->GetRelativeAddress(0));
		break;

	case CMD_FMT_VADDR:
		swprintf(dest, L"%08x", gpuDebug->GetVertexAddress());
		break;

	case CMD_FMT_IADDR:
		swprintf(dest, L"%08x", gpuDebug->GetIndexAddress());
		break;

	case CMD_FMT_MATERIALUPDATE:
		{
			static const char *materialTypes[] = {
				"none",
				"ambient",
				"diffuse",
				"ambient, diffuse",
				"specular",
				"ambient, specular",
				"diffuse, specular",
				"ambient, diffuse, specular",
			};
			if (value < (u32)ARRAY_SIZE(materialTypes)) {
				swprintf(dest, L"%S", materialTypes[value]);
			} else {
				swprintf(dest, L"%06x", value);
			}
		}
		break;

	case CMD_FMT_STENCILOP:
		{
			static const char *stencilOps[] = { "KEEP", "ZERO", "REPLACE", "INVERT", "INCREMENT", "DECREMENT" };
			const u8 sfail = (value >> 0) & 0xFF;
			const u8 zfail = (value >> 8) & 0xFF;
			const u8 pass = (value >> 16) & 0xFF;
			const u8 totalValid = (u8)ARRAY_SIZE(stencilOps);
			if (sfail < totalValid && zfail < totalValid && pass < totalValid) {
				swprintf(dest, L"fail=%S, pass/depthfail=%S, pass=%S", stencilOps[sfail], stencilOps[zfail], stencilOps[pass]);
			} else {
				swprintf(dest, L"%06x", value);
			}
		}
		break;

	case CMD_FMT_BLENDMODE:
		{
			const char *blendModes[] = {
				"add",
				"subtract",
				"reverse subtract",
				"min",
				"max",
				"abs subtract",
			};
			const char *blendFactorsA[] = {
				"dst",
				"1.0 - dst",
				"src.a",
				"1.0 - src.a",
				"dst.a",
				"1.0 - dst.a",
				"2.0 * src.a",
				"1.0 - 2.0 * src.a",
				"2.0 * dst.a",
				"1.0 - 2.0 * dst.a",
				"fixed",
			};
			const char *blendFactorsB[] = {
				"src",
				"1.0 - src",
				"src.a",
				"1.0 - src.a",
				"dst.a",
				"1.0 - dst.a",
				"2.0 * src.a",
				"1.0 - 2.0 * src.a",
				"2.0 * dst.a",
				"1.0 - 2.0 * dst.a",
				"fixed",
			};
			const u8 blendFactorA = (value >> 0) & 0xF;
			const u8 blendFactorB = (value >> 4) & 0xF;
			const u32 blendMode = (value >> 8);

			if (blendFactorA < (u8)ARRAY_SIZE(blendFactorsA) && blendFactorB < (u8)ARRAY_SIZE(blendFactorsB) && blendMode < (u32)ARRAY_SIZE(blendModes)) {
				swprintf(dest, L"%S: %S, %S", blendModes[blendMode], blendFactorsA[blendFactorA], blendFactorsB[blendFactorB]);
			} else {
				swprintf(dest, L"%06x", value);
			}
		}
		break;

	case CMD_FMT_CLEARMODE:
		if (value == 0) {
			swprintf(dest, L"%d", value);
		} else if ((value & ~(GE_CLEARMODE_ALL | 1)) == 0) {
			const char *clearmodes[] = {
				"1, write disabled",
				"1, write color",
				"1, write alpha/stencil",
				"1, write color, alpha/stencil",
				"1, write depth",
				"1, write color, depth",
				"1, write alpha/stencil, depth",
				"1, write color, alpha/stencil, depth",
			};
			swprintf(dest, L"%S", clearmodes[value >> 8]);
		} else {
			swprintf(dest, L"%06x", value);
		}
		break;

	case CMD_FMT_TEXFUNC:
		{
			const char *texfuncs[] = {
				"modulate",
				"decal",
				"blend",
				"replace",
				"add",
			};
			const u8 func = (value >> 0) & 0xFF;
			const u8 rgba = (value >> 8) & 0xFF;
			const u8 colorDouble = (value >> 16) & 0xFF;

			if (rgba <= 1 && colorDouble <= 1 && func < (u8)ARRAY_SIZE(texfuncs)) {
				swprintf(dest, L"%S, %S%S", texfuncs[func], rgba ? "RGBA" : "RGB", colorDouble ? ", color doubling" : "");
			} else {
				swprintf(dest, L"%06x", value);
			}
		}
		break;

	case CMD_FMT_TEXMODE:
		{
			const u8 swizzle = (value >> 0) & 0xFF;
			const u8 clutLevels = (value >> 8) & 0xFF;
			const u8 maxLevel = (value >> 16) & 0xFF;

			if (swizzle <= 1 && clutLevels <= 1 && maxLevel <= 7) {
				swprintf(dest, L"%S%d levels%S", swizzle ? "swizzled, " : "", maxLevel + 1, clutLevels ? ", CLUT per level" : "");
			} else {
				swprintf(dest, L"%06x", value);
			}
		}
		break;

	case CMD_FMT_LOGICOP:
		{
			const char *logicOps[] = {
				"clear",
				"and",
				"reverse and",
				"copy",
				"inverted and",
				"noop",
				"xor",
				"or",
				"negated or",
				"equivalence",
				"inverted",
				"reverse or",
				"inverted copy",
				"inverted or",
				"negated and",
				"set",
			};

			if (value < ARRAY_SIZE(logicOps)) {
				swprintf(dest, L"%S", logicOps[value]);
			} else {
				swprintf(dest, L"%06x", value);
			}
		}
		break;

	case CMD_FMT_TEXWRAP:
		{
			if ((value & ~0x0101) == 0) {
				const bool clampS = (value & 0x0001) != 0;
				const bool clampT = (value & 0x0100) != 0;
				swprintf(dest, L"%S s, %S t", clampS ? "clamp" : "wrap", clampT ? "clamp" : "wrap");
			} else {
				swprintf(dest, L"%06x", value);
			}
		}
		break;

	case CMD_FMT_TEXFILTER:
		{
			const char *textureFilters[] = {
				"nearest",
				"linear",
				NULL,
				NULL,
				"nearest, mipmap nearest",
				"linear, mipmap nearest",
				"nearest, mipmap linear",
				"linear, mipmap linear",
			};
			if ((value & ~0x0107) == 0 && textureFilters[value & 7] != NULL) {
				const int min = (value & 0x0007) >> 0;
				const int mag = (value & 0x0100) >> 8;
				swprintf(dest, L"min: %S, mag: %S", textureFilters[min], textureFilters[mag]);
			} else {
				swprintf(dest, L"%06x", value);
			}
		}
		break;

	case CMD_FMT_TEXMAPMODE:
		{
			const char *uvGenModes[] = {
				"tex coords",
				"tex matrix",
				"tex env map",
				"unknown (tex coords?)",
			};
			const char *uvProjModes[] = {
				"pos",
				"uv",
				"normalized normal",
				"normal",
			};
			if ((value & ~0x0303) == 0) {
				const int uvGen = (value & 0x0003) >> 0;
				const int uvProj = (value & 0x0300) >> 8;
				swprintf(dest, L"gen: %S, proj: %S", uvGenModes[uvGen], uvProjModes[uvProj]);
			} else {
				swprintf(dest, L"%06x", value);
			}
		}
		break;

	case CMD_FMT_FLAG:
		if ((value & ~1) == 0) {
			swprintf(dest, L"%d", value);
		} else {
			swprintf(dest, L"%06x", value);
		}
		break;

	default:
		swprintf(dest, L"BAD FORMAT %06x", value);
	}

	// TODO: Turn row grey or some such?
	if (!enabled) {
		wcscat(dest, L" (disabled)");
	}
}

void CtrlStateValues::GetColumnText(wchar_t *dest, int row, int col) {
	if (row < 0 || row >= rowCount_) {
		return;
	}

	switch (col) {
	case STATEVALUES_COL_NAME:
		wcscpy(dest, rows_[row].title);
		break;

	case STATEVALUES_COL_VALUE:
		{
			if (gpuDebug == NULL) {
				wcscpy(dest, L"N/A");
				break;
			}

			const auto info = rows_[row];
			const auto state = gpuDebug->GetGState();
			const bool enabled = info.enableCmd == 0 || (state.cmdmem[info.enableCmd] & 1) == 1;
			const u32 value = state.cmdmem[info.cmd] & 0xFFFFFF;
			const u32 otherValue = state.cmdmem[info.otherCmd] & 0xFFFFFF;
			const u32 otherValue2 = state.cmdmem[info.otherCmd2] & 0xFFFFFF;

			FormatStateRow(dest, info, value, enabled, otherValue, otherValue2);
			break;
		}
	}
}

void CtrlStateValues::OnDoubleClick(int row, int column) {
	if (gpuDebug == NULL) {
		return;
	}

	const auto info = rows_[row];
	switch (info.fmt) {
	case CMD_FMT_FLAG:
		{
			const auto state = gpuDebug->GetGState();
			u32 newValue = state.cmdmem[info.cmd] ^ 1;
			SetCmdValue(newValue);
		}
		break;

	default:
		{
			// TODO: Floats/etc., and things with multiple cmds.
			const auto state = gpuDebug->GetGState();
			u32 newValue = state.cmdmem[info.cmd] & 0x00FFFFFF;
			if (InputBox_GetHex(GetModuleHandle(NULL), GetHandle(), L"New value", newValue, newValue)) {
				newValue |= state.cmdmem[info.cmd] & 0xFF000000;
				SetCmdValue(newValue);
			}
		}
		break;
	}
}

void CtrlStateValues::OnRightClick(int row, int column, const POINT& point) {
	if (gpuDebug == NULL) {
		return;
	}

	// TODO: Copy, break, watch... enable?
}

void CtrlStateValues::SetCmdValue(u32 op) {
	SendMessage(GetParent(GetParent(GetHandle())), WM_GEDBG_SETCMDWPARAM, op, NULL);
		Update();
}

TabStateValues::TabStateValues(const TabStateRow *rows, int rowCount, LPCSTR dialogID, HINSTANCE _hInstance, HWND _hParent)
	: Dialog(dialogID, _hInstance, _hParent) {
	values = new CtrlStateValues(rows, rowCount, GetDlgItem(m_hDlg, IDC_GEDBG_VALUES));
}

TabStateValues::~TabStateValues() {
	delete values;
}

void TabStateValues::UpdateSize(WORD width, WORD height) {
	struct Position {
		int x,y;
		int w,h;
	};

	Position position;
	static const int borderMargin = 5;

	position.x = borderMargin;
	position.y = borderMargin;
	position.w = width - 2 * borderMargin;
	position.h = height - 2 * borderMargin;

	HWND handle = GetDlgItem(m_hDlg,IDC_GEDBG_VALUES);
	MoveWindow(handle, position.x, position.y, position.w, position.h, TRUE);
}

BOOL TabStateValues::DlgProc(UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message) {
	case WM_INITDIALOG:
		return TRUE;

	case WM_SIZE:
		UpdateSize(LOWORD(lParam), HIWORD(lParam));
		return TRUE;

	case WM_NOTIFY:
		switch (wParam)
		{
		case IDC_GEDBG_VALUES:
			values->HandleNotify(lParam);
			break;
		}
		break;
	}

	return FALSE;
}

TabStateFlags::TabStateFlags(HINSTANCE _hInstance, HWND _hParent)
	: TabStateValues(stateFlagsRows, ARRAY_SIZE(stateFlagsRows), (LPCSTR)IDD_GEDBG_TAB_VALUES, _hInstance, _hParent) {
}

TabStateLighting::TabStateLighting(HINSTANCE _hInstance, HWND _hParent)
	: TabStateValues(stateLightingRows, ARRAY_SIZE(stateLightingRows), (LPCSTR)IDD_GEDBG_TAB_VALUES, _hInstance, _hParent) {
}

TabStateSettings::TabStateSettings(HINSTANCE _hInstance, HWND _hParent)
	: TabStateValues(stateSettingsRows, ARRAY_SIZE(stateSettingsRows), (LPCSTR)IDD_GEDBG_TAB_VALUES, _hInstance, _hParent) {
}

TabStateTexture::TabStateTexture(HINSTANCE _hInstance, HWND _hParent)
	: TabStateValues(stateTextureRows, ARRAY_SIZE(stateTextureRows), (LPCSTR)IDD_GEDBG_TAB_VALUES, _hInstance, _hParent) {
}
