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



// Ideas for speeding things up on mobile OpenGL ES implementations
//
// Use superbuffers! Yes I just invented that name.
//
// The idea is to avoid respecifying the vertex format between every draw call (multiple glVertexAttribPointer ...)
// by combining the contents of multiple draw calls into one buffer, as long as
// they have exactly the same output vertex format. (different input formats is fine! This way
// we can combine the data for multiple draws with different numbers of bones, as we consider numbones < 4 to be = 4)
// into one VBO.
//
// This will likely be a win because I believe that between every change of VBO + glVertexAttribPointer*N, the driver will
// perform a lot of validation, probably at draw call time, while all the validation can be skipped if the only thing
// that changes between two draw calls is simple state or texture or a matrix etc, not anything vertex related.
// Also the driver will have to manage hundreds instead of thousands of VBOs in games like GTA.
//
// * Every 10 frames or something, do the following:
//   - Frame 1:
//		 + Mark all drawn buffers with in-frame sequence numbers (alternatively,
//		   just log them in an array)
//	 - Frame 2 (beginning?):
//	   + Take adjacent buffers that have the same output vertex format, and add them
//	     to a list of buffers to combine. Create said buffers with appropriate sizes
//	     and precompute the offsets that the draws should be written into.
//	 - Frame 2 (end):
//	   + Actually do the work of combining the buffers. This probably means re-decoding
//	     the vertices into a new one. Will also have to apply index offsets.
//
// Also need to change the drawing code so that we don't glBindBuffer and respecify glVAP if
// two subsequent drawcalls come from the same superbuffer.
//
// Or we ignore all of this including vertex caching and simply find a way to do highly optimized vertex streaming,
// like Dolphin is trying to. That will likely never be able to reach the same speed as perfectly optimized
// superbuffers though. For this we will have to JIT the vertex decoder but that's not too hard.
//
// Now, when do we delete superbuffers? Maybe when half the buffers within have been killed?
//
// Another idea for GTA which switches textures a lot while not changing much other state is to use ES 3 Array
// textures, if they are the same size (even if they aren't, might be okay to simply resize the textures to match
// if they're just a multiple of 2 away) or something. Then we'd have to add a W texture coordinate to choose the
// texture within the bound texture array to the vertex data when merging into superbuffers.
//
// There are even more things to try. For games that do matrix palette skinning by quickly switching bones and
// just drawing a few triangles per call (NBA, FF:CC, Tekken 6 etc) we could even collect matrices, upload them
// all at once, writing matrix indices into the vertices in addition to the weights, and then doing a single
// draw call with specially generated shader to draw the whole mesh. This code will be seriously complex though.

#include "base/logging.h"
#include "base/timeutil.h"

#include "Common/MemoryUtil.h"
#include "Core/MemMap.h"
#include "Core/Host.h"
#include "Core/System.h"
#include "Core/Reporting.h"
#include "Core/Config.h"
#include "Core/CoreTiming.h"

#include "helper/dx_state.h"
#include "ext/xxhash.h"

#include "GPU/Math3D.h"
#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"

#include "GPU/Directx9/StateMappingDX9.h"
#include "GPU/Directx9/TextureCacheDX9.h"
#include "GPU/Directx9/TransformPipelineDX9.h"
#include "GPU/Directx9/VertexDecoderDX9.h"
#include "GPU/Directx9/ShaderManagerDX9.h"
#include "GPU/Directx9/GPU_DX9.h"

namespace DX9 {

const D3DPRIMITIVETYPE glprim[8] = {
	D3DPT_POINTLIST,
	D3DPT_LINELIST,
	D3DPT_LINESTRIP,
	D3DPT_TRIANGLELIST,
	D3DPT_TRIANGLESTRIP,
	D3DPT_TRIANGLEFAN,
#ifndef _XBOX
	D3DPT_TRIANGLELIST,	 // With OpenGL ES we have to expand sprites into triangles, tripling the data instead of doubling. sigh. OpenGL ES, Y U NO SUPPORT GL_QUADS?
#else
	D3DPT_TRIANGLELIST,
#endif
};

#ifndef _XBOX
// hrydgard's quick guesses - TODO verify
static const int D3DPRIMITIVEVERTEXCOUNT[8][2] = {
    {0, 0}, // invalid
    {1, 0}, // 1 = D3DPT_POINTLIST,
    {2, 0}, // 2 = D3DPT_LINELIST,
    {2, 1}, // 3 = D3DPT_LINESTRIP,
    {3, 0}, // 4 = D3DPT_TRIANGLELIST,
    {1, 2}, // 5 = D3DPT_TRIANGLESTRIP,
    {1, 2}, // 6 = D3DPT_TRIANGLEFAN,
};

int D3DPrimCount(D3DPRIMITIVETYPE prim, int size) {
    return (size / D3DPRIMITIVEVERTEXCOUNT[prim][0]) - D3DPRIMITIVEVERTEXCOUNT[prim][1];
}
#endif

enum {
	VERTEX_BUFFER_MAX = 65536,
	DECODED_VERTEX_BUFFER_SIZE = VERTEX_BUFFER_MAX * 48,
	DECODED_INDEX_BUFFER_SIZE = VERTEX_BUFFER_MAX * 20,
	TRANSFORMED_VERTEX_BUFFER_SIZE = VERTEX_BUFFER_MAX * sizeof(TransformedVertex)
};

#define QUAD_INDICES_MAX 32768

#define VERTEXCACHE_DECIMATION_INTERVAL 17

#ifndef _XBOX
// Check for max first as clamping to max is more common than min when lighting.
inline float clamp(float in, float min, float max) { 
	return in > max ? max : (in < min ? min : in);
}
#else
inline float clamp(float in, float min, float max) { 
   in = __fsel( in - min , in, min );
   return __fsel( in - max, max, in );
}
#endif

TransformDrawEngineDX9::TransformDrawEngineDX9()
	: collectedVerts(0),
	prevPrim_(GE_PRIM_INVALID),
	dec_(0),
	lastVType_(-1),
	shaderManager_(0),
	textureCache_(0),
	framebufferManager_(0),
	numDrawCalls(0),
	vertexCountInDrawCalls(0),
	uvScale(0) {
	decimationCounter_ = VERTEXCACHE_DECIMATION_INTERVAL;
	// Allocate nicely aligned memory. Maybe graphics drivers will
	// appreciate it.
	// All this is a LOT of memory, need to see if we can cut down somehow.
	decoded = (u8 *)AllocateMemoryPages(DECODED_VERTEX_BUFFER_SIZE);
	decIndex = (u16 *)AllocateMemoryPages(DECODED_INDEX_BUFFER_SIZE);
	transformed = (TransformedVertex *)AllocateMemoryPages(TRANSFORMED_VERTEX_BUFFER_SIZE);
	transformedExpanded = (TransformedVertex *)AllocateMemoryPages(3 * TRANSFORMED_VERTEX_BUFFER_SIZE);
	quadIndices_ = new u16[6 * QUAD_INDICES_MAX];

	for (int i = 0; i < QUAD_INDICES_MAX; i++) {
		quadIndices_[i * 6 + 0] = i * 4;
		quadIndices_[i * 6 + 1] = i * 4 + 2;
		quadIndices_[i * 6 + 2] = i * 4 + 1;
		quadIndices_[i * 6 + 3] = i * 4 + 1;
		quadIndices_[i * 6 + 4] = i * 4 + 2;
		quadIndices_[i * 6 + 5] = i * 4 + 3;
	}
	
	if (g_Config.bPrescaleUV) {
		uvScale = new UVScale[MAX_DEFERRED_DRAW_CALLS];
	}
	indexGen.Setup(decIndex);
#ifdef _XBOX
	decJitCache_ = new VertexDecoderJitCache();
#else
	decJitCache_ = NULL;
#endif
	InitDeviceObjects();
}

TransformDrawEngineDX9::~TransformDrawEngineDX9() {
	DestroyDeviceObjects();
	FreeMemoryPages(decoded, DECODED_VERTEX_BUFFER_SIZE);
	FreeMemoryPages(decIndex, DECODED_INDEX_BUFFER_SIZE);
	FreeMemoryPages(transformed, TRANSFORMED_VERTEX_BUFFER_SIZE);
	FreeMemoryPages(transformedExpanded, 3 * TRANSFORMED_VERTEX_BUFFER_SIZE);
	delete [] quadIndices_;
#ifdef _XBOX	
	delete decJitCache_;
#endif
	for (auto iter = decoderMap_.begin(); iter != decoderMap_.end(); iter++) {
		delete iter->second;
	}
	delete [] uvScale;
}

void TransformDrawEngineDX9::InitDeviceObjects() {

}

void TransformDrawEngineDX9::DestroyDeviceObjects() {
	ClearTrackedVertexArrays();
}

namespace {
using namespace DX9;

// Convenient way to do precomputation to save the parts of the lighting calculation
// that's common between the many vertices of a draw call.
class Lighter {
public:
	Lighter();
	void Light(float colorOut0[4], float colorOut1[4], const float colorIn[4], Vec3f pos, Vec3f normal);

private:
	Color4 globalAmbient;
	Color4 materialEmissive;
	Color4 materialAmbient;
	Color4 materialDiffuse;
	Color4 materialSpecular;
	float specCoef_;
	// Vec3f viewer_;
	bool doShadeMapping_;
	int materialUpdate_;
};

Lighter::Lighter() {
	doShadeMapping_ = gstate.getUVGenMode() == GE_TEXMAP_ENVIRONMENT_MAP;
	materialEmissive.GetFromRGB(gstate.materialemissive);
	materialEmissive.a = 0.0f;
	globalAmbient.GetFromRGB(gstate.ambientcolor);
	globalAmbient.GetFromA(gstate.ambientalpha);
	materialAmbient.GetFromRGB(gstate.materialambient);
	materialAmbient.GetFromA(gstate.materialalpha);
	materialDiffuse.GetFromRGB(gstate.materialdiffuse);
	materialDiffuse.a = 1.0f;
	materialSpecular.GetFromRGB(gstate.materialspecular);
	materialSpecular.a = 1.0f;
	specCoef_ = getFloat24(gstate.materialspecularcoef);
	// viewer_ = Vec3f(-gstate.viewMatrix[9], -gstate.viewMatrix[10], -gstate.viewMatrix[11]);
	materialUpdate_ = gstate.materialupdate & 7;
}

void Lighter::Light(float colorOut0[4], float colorOut1[4], const float colorIn[4], Vec3f pos, Vec3f norm)
{
	// Color are in dx format
	Color4 in;

	in.a = colorIn[0];
	in.r = colorIn[1];
	in.g = colorIn[2];
	in.b = colorIn[3];


	const Color4 *ambient;
	if (materialUpdate_ & 1)
		ambient = &in;
	else
		ambient = &materialAmbient;

	const Color4 *diffuse;
	if (materialUpdate_ & 2)
		diffuse = &in;
	else
		diffuse = &materialDiffuse;

	const Color4 *specular;
	if (materialUpdate_ & 4)
		specular = &in;
	else
		specular = &materialSpecular;

	Color4 lightSum0 = globalAmbient * *ambient + materialEmissive;
	Color4 lightSum1(0, 0, 0, 0);

	for (int l = 0; l < 4; l++)
	{
		// can we skip this light?
		if (!gstate.isLightChanEnabled(l))
			continue;

		GELightType type = gstate.getLightType(l);

		Vec3f toLight(0,0,0);
		Vec3f lightDir(0,0,0);

		if (type == GE_LIGHTTYPE_DIRECTIONAL)
			toLight = Vec3f(gstate_c.lightpos[l]);  // lightdir is for spotlights
		else
			toLight = Vec3f(gstate_c.lightpos[l]) - pos;

		bool doSpecular = gstate.isUsingSpecularLight(l);
		bool poweredDiffuse = gstate.isUsingPoweredDiffuseLight(l);

		float distanceToLight = toLight.Length();
		float dot = 0.0f;
		float angle = 0.0f;
		float lightScale = 0.0f;

		if (distanceToLight > 0.0f) {
			toLight /= distanceToLight;
			dot = Dot(toLight, norm);
		}
		// Clamp dot to zero.
		if (dot < 0.0f) dot = 0.0f;

		if (poweredDiffuse)
			dot = powf(dot, specCoef_);

		// Attenuation
		switch (type) {
		case GE_LIGHTTYPE_DIRECTIONAL:
			lightScale = 1.0f;
			break;
		case GE_LIGHTTYPE_POINT:
			lightScale = clamp(1.0f / (gstate_c.lightatt[l][0] + gstate_c.lightatt[l][1]*distanceToLight + gstate_c.lightatt[l][2]*distanceToLight*distanceToLight), 0.0f, 1.0f);
			break;
		case GE_LIGHTTYPE_SPOT:
		case GE_LIGHTTYPE_UNKNOWN:
			lightDir = gstate_c.lightdir[l];
			angle = Dot(toLight.Normalized(), lightDir.Normalized());
			if (angle >= gstate_c.lightangle[l])
				lightScale = clamp(1.0f / (gstate_c.lightatt[l][0] + gstate_c.lightatt[l][1]*distanceToLight + gstate_c.lightatt[l][2]*distanceToLight*distanceToLight), 0.0f, 1.0f) * powf(angle, gstate_c.lightspotCoef[l]);
			break;
		default:
			// ILLEGAL
			break;
		}

		Color4 lightDiff(gstate_c.lightColor[1][l], 0.0f);
		Color4 diff = (lightDiff * *diffuse) * dot;

		// Real PSP specular
		Vec3f toViewer(0,0,1);
		// Better specular
		// Vec3f toViewer = (viewer - pos).Normalized();

		if (doSpecular)
		{
			Vec3f halfVec = (toLight + toViewer);
			halfVec.Normalize();

			dot = Dot(halfVec, norm);
			if (dot > 0.0f)
			{
				Color4 lightSpec(gstate_c.lightColor[2][l], 0.0f);
				lightSum1 += (lightSpec * *specular * (powf(dot, specCoef_) * lightScale));
			}
		}

		if (gstate.isLightChanEnabled(l))
		{
			Color4 lightAmbient(gstate_c.lightColor[0][l], 0.0f);
			lightSum0 += (lightAmbient * *ambient + diff) * lightScale;
		}
	}

	// 4?
	for (int i = 0; i < 4; i++) {
		colorOut0[i] = lightSum0[i] > 1.0f ? 1.0f : lightSum0[i];
		colorOut1[i] = lightSum1[i] > 1.0f ? 1.0f : lightSum1[i];
	}
}

}  // namespace

struct DeclTypeInfo {
	u32 type;
	const char * name;
};

static const DeclTypeInfo VComp[] = {
	{0, "NULL"},						// 	DEC_NONE,
	{D3DDECLTYPE_FLOAT1		,"D3DDECLTYPE_FLOAT1 "},	// 	DEC_FLOAT_1,
	{D3DDECLTYPE_FLOAT2		,"D3DDECLTYPE_FLOAT2 "},	// 	DEC_FLOAT_2,
	{D3DDECLTYPE_FLOAT3		,"D3DDECLTYPE_FLOAT3 "},	// 	DEC_FLOAT_3,
	{D3DDECLTYPE_FLOAT4		,"D3DDECLTYPE_FLOAT4 "},	// 	DEC_FLOAT_4,
#ifdef _XBOX
	{D3DDECLTYPE_BYTE4N		,"D3DDECLTYPE_BYTE4N "},	// 	DEC_S8_3,
#else
	// Not supported in regular DX9 so faking, will cause graphics bugs until worked around
	{D3DDECLTYPE_UBYTE4   ,"D3DDECLTYPE_BYTE4N "},	// 	DEC_S8_3,
#endif

	{D3DDECLTYPE_SHORT4N	,"D3DDECLTYPE_SHORT4N	"},	// 	DEC_S16_3,
	{D3DDECLTYPE_UBYTE4N	,"D3DDECLTYPE_UBYTE4N	"},	// 	DEC_U8_1,
	{D3DDECLTYPE_UBYTE4N	,"D3DDECLTYPE_UBYTE4N	"},	// 	DEC_U8_2,
	{D3DDECLTYPE_UBYTE4N	,"D3DDECLTYPE_UBYTE4N	"},	// 	DEC_U8_3,
	{D3DDECLTYPE_UBYTE4N	,"D3DDECLTYPE_UBYTE4N	"},	// 	DEC_U8_4,
	{D3DDECLTYPE_USHORT4N	,"D3DDECLTYPE_USHORT4N "},	// 	DEC_U16_1,
	{D3DDECLTYPE_USHORT4N	,"D3DDECLTYPE_USHORT4N "},	// 	DEC_U16_2,
	{D3DDECLTYPE_USHORT4N	,"D3DDECLTYPE_USHORT4N "},	// 	DEC_U16_3,
	{D3DDECLTYPE_USHORT4N	,"D3DDECLTYPE_USHORT4N "},	// 	DEC_U16_4,
#ifdef _XBOX
	{D3DDECLTYPE_BYTE4		,"D3DDECLTYPE_BYTE4 "},	// 	DEC_U8A_2,
	{D3DDECLTYPE_USHORT4	,"D3DDECLTYPE_USHORT4 "},	// 	DEC_U16A_2,
#else
	// Not supported in regular DX9 so faking, will cause graphics bugs until worked around
	{D3DDECLTYPE_UBYTE4   ,"D3DDECLTYPE_BYTE4 "},	// 	DEC_U8A_2,
	{D3DDECLTYPE_USHORT4N	,"D3DDECLTYPE_USHORT4 "},	// 	DEC_U16A_2,
#endif
};

static void VertexAttribSetup(D3DVERTEXELEMENT9 * VertexElement, u8 fmt, u8 offset, u8 usage, u8 usage_index = 0) {
	memset(VertexElement, 0, sizeof(D3DVERTEXELEMENT9));
	VertexElement->Offset = offset;
	if (usage == D3DDECLUSAGE_COLOR && fmt == DEC_U8_4) {
		VertexElement->Type = D3DDECLTYPE_D3DCOLOR;	
	} else {
		VertexElement->Type = VComp[fmt].type;	
	}
	VertexElement->Usage = usage;
	VertexElement->UsageIndex = usage_index;
}

static IDirect3DVertexDeclaration9* pHardwareVertexDecl = NULL;
static std::map<u32, IDirect3DVertexDeclaration9 *> vertexDeclMap;
static D3DVERTEXELEMENT9 VertexElements[8];

// TODO: Use VBO and get rid of the vertexData pointers - with that, we will supply only offsets
static void LogDecFmtForDraw(const DecVtxFormat &decFmt) {
	// Vertices Elements orders
	// WEIGHT
	if (decFmt.w0fmt != 0) {
		printf("decFmt.w0fmt -> %s (%d)\n", VComp[decFmt.w0fmt].name, decFmt.w0off);
	}

	if (decFmt.w1fmt != 0) {
		printf("decFmt.w1fmt -> %s (%d)\n", VComp[decFmt.w1fmt].name, decFmt.w1off);
	}

	// TC
	if (decFmt.uvfmt != 0) {
		printf("decFmt.uvfmt -> %s (%d)\n", VComp[decFmt.uvfmt].name, decFmt.uvoff);
	}

	// COLOR
	if (decFmt.c0fmt != 0) {
		printf("decFmt.c0fmt -> %s (%d)\n", VComp[decFmt.c0fmt].name, decFmt.c0off);
	}

	// NORMAL
	if (decFmt.nrmfmt != 0) {
		printf("decFmt.nrmfmt -> %s (%d)\n", VComp[decFmt.nrmfmt].name, decFmt.nrmoff);
	}

	// POSITION
	// Always
	printf("decFmt.posfmt -> %s (%d)\n", VComp[decFmt.posfmt].name, decFmt.posoff);

	printf("decFmt.stride => %d\n", decFmt.stride);

	//pD3Ddevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
}
static void SetupDecFmtForDraw(LinkedShaderDX9 *program, const DecVtxFormat &decFmt, u32 pspFmt) {
	auto vertexDeclCached = vertexDeclMap.find(pspFmt);

	if (vertexDeclCached==vertexDeclMap.end()) {
		D3DVERTEXELEMENT9 * VertexElement = &VertexElements[0];
		int offset = 0;

		// Vertices Elements orders
		// WEIGHT
		if (decFmt.w0fmt != 0) {
			VertexAttribSetup(VertexElement, decFmt.w0fmt, decFmt.w0off, D3DDECLUSAGE_BLENDWEIGHT, 0);
			VertexElement++;
		}

		if (decFmt.w1fmt != 0) {
			VertexAttribSetup(VertexElement, decFmt.w1fmt, decFmt.w1off, D3DDECLUSAGE_BLENDWEIGHT, 1);
			VertexElement++;
		}

		// TC
		if (decFmt.uvfmt != 0) {
			VertexAttribSetup(VertexElement, decFmt.uvfmt, decFmt.uvoff, D3DDECLUSAGE_TEXCOORD);
			VertexElement++;
		}

		// COLOR
		if (decFmt.c0fmt != 0) {
			VertexAttribSetup(VertexElement, decFmt.c0fmt, decFmt.c0off, D3DDECLUSAGE_COLOR, 0);
			VertexElement++;
		}
		// Never used ?
		if (decFmt.c1fmt != 0) {
			VertexAttribSetup(VertexElement, decFmt.c1fmt, decFmt.c1off, D3DDECLUSAGE_COLOR, 1);
			VertexElement++;
		}

		// NORMAL
		if (decFmt.nrmfmt != 0) {
			VertexAttribSetup(VertexElement, decFmt.nrmfmt, decFmt.nrmoff, D3DDECLUSAGE_NORMAL, 0);
			VertexElement++;
		}

		// POSITION
		// Always
		VertexAttribSetup(VertexElement, decFmt.posfmt, decFmt.posoff, D3DDECLUSAGE_POSITION, 0);
		VertexElement++;

		// End
		D3DVERTEXELEMENT9 end = D3DDECL_END();
		memcpy(VertexElement, &end, sizeof(D3DVERTEXELEMENT9));
	
		// Create declaration	
		pD3Ddevice->CreateVertexDeclaration( VertexElements, &pHardwareVertexDecl );

		// Add it to map
		vertexDeclMap[pspFmt] = pHardwareVertexDecl;

		// Log
		//LogDecFmtForDraw(decFmt);
	} else {
		// Set it from map
		pHardwareVertexDecl = vertexDeclCached->second;
	}
}


// The verts are in the order:  BR BL TL TR
static void SwapUVs(TransformedVertex &a, TransformedVertex &b) {
	float tempu = a.u;
	float tempv = a.v;
	a.u = b.u;
	a.v = b.v;
	b.u = tempu;
	b.v = tempv;
}

// 2   3       3   2        0   3          2   1
//        to           to            or
// 1   0       0   1        1   2          3   0


// See comment below where this was called before.
/*
static void RotateUV(TransformedVertex v[4]) {
float x1 = v[2].x;
float x2 = v[0].x;
float y1 = v[2].y;
float y2 = v[0].y;

if ((x1 < x2 && y1 < y2) || (x1 > x2 && y1 > y2))
SwapUVs(v[1], v[3]);
}*/

static void RotateUVThrough(TransformedVertex v[4]) {
	float x1 = v[2].x;
	float x2 = v[0].x;
	float y1 = v[2].y;
	float y2 = v[0].y;

	if ((x1 < x2 && y1 > y2) || (x1 > x2 && y1 < y2))
		SwapUVs(v[1], v[3]);
}


// Clears on the PSP are best done by drawing a series of vertical strips
// in clear mode. This tries to detect that.
bool TransformDrawEngineDX9::IsReallyAClear(int numVerts) const {
	if (transformed[0].x != 0.0f || transformed[0].y != 0.0f)
		return false;

	u32 matchcolor;
	memcpy(&matchcolor, transformed[0].color0, 4);
	float matchz = transformed[0].z;

	int bufW = gstate_c.curRTWidth;
	int bufH = gstate_c.curRTHeight;

	float prevX = 0.0f;
	for (int i = 1; i < numVerts; i++) {
		u32 vcolor;
		memcpy(&vcolor, transformed[i].color0, 4);
		if (vcolor != matchcolor || transformed[i].z != matchz)
			return false;

		if ((i & 1) == 0) {
			// Top left of a rectangle
			if (transformed[i].y != 0)
				return false;
			if (i > 0 && transformed[i].x != transformed[i - 1].x)
				return false;
		} else {
			// Bottom right
			if (transformed[i].y != bufH)
				return false;
			if (transformed[i].x <= transformed[i - 1].x)
				return false;
		}
	}

	// The last vertical strip often extends outside the drawing area.
	if (transformed[numVerts - 1].x < bufW)
		return false;

	return true;
}

// This is the software transform pipeline, which is necessary for supporting RECT
// primitives correctly, and may be easier to use for debugging than the hardware
// transform pipeline.

// There's code here that simply expands transformed RECTANGLES into plain triangles.

// We're gonna have to keep software transforming RECTANGLES, unless we use a geom shader which we can't on OpenGL ES 2.0.
// Usually, though, these primitives don't use lighting etc so it's no biggie performance wise, but it would be nice to get rid of
// this code.

// Actually, if we find the camera-relative right and down vectors, it might even be possible to add the extra points in pre-transformed
// space and thus make decent use of hardware transform.

// Actually again, single quads could be drawn more efficiently using GL_TRIANGLE_STRIP, no need to duplicate verts as for
// GL_TRIANGLES. Still need to sw transform to compute the extra two corners though.
void TransformDrawEngineDX9::SoftwareTransformAndDraw(
	int prim, u8 *decoded, LinkedShaderDX9 *program, int vertexCount, u32 vertType, void *inds, int indexType, const DecVtxFormat &decVtxFormat, int maxIndex) {
		
		bool throughmode = (vertType & GE_VTYPE_THROUGH_MASK) != 0;
		bool lmode = gstate.isUsingSecondaryColor() && gstate.isLightingEnabled();

		// TODO: Split up into multiple draw calls for GLES 2.0 where you can't guarantee support for more than 0x10000 verts.
		float uscale = 1.0f;
		float vscale = 1.0f;
		if (throughmode) {
			uscale /= gstate_c.curTextureWidth;
			vscale /= gstate_c.curTextureHeight;
		}

		int w = gstate.getTextureWidth(0);
		int h = gstate.getTextureHeight(0);
		float widthFactor = (float) w / (float) gstate_c.curTextureWidth;
		float heightFactor = (float) h / (float) gstate_c.curTextureHeight;

		Lighter lighter;
		float fog_end = getFloat24(gstate.fog1);
		float fog_slope = getFloat24(gstate.fog2);

		VertexReader reader(decoded, decVtxFormat, vertType);
		for (int index = 0; index < maxIndex; index++) {
			reader.Goto(index);

			float v[3] = {0, 0, 0};
			float c0[4] = {1, 1, 1, 1};
			float c1[4] = {0, 0, 0, 0};
			float uv[3] = {0, 0, 1};
			float fogCoef = 1.0f;

			if (throughmode) {
				// Do not touch the coordinates or the colors. No lighting.
				reader.ReadPos(v);
				if (reader.hasColor0()) {
					reader.ReadColor0(c0);
					for (int j = 0; j < 4; j++) {
						c1[j] = 0.0f;
					}
				} else {
					c0[0] = gstate.getMaterialAmbientA() / 255.f;
					c0[1] = gstate.getMaterialAmbientR() / 255.f;
					c0[2] = gstate.getMaterialAmbientG() / 255.f;
					c0[3] = gstate.getMaterialAmbientB() / 255.f;
				}

				if (reader.hasUV()) {
					reader.ReadUV(uv);

					uv[0] *= uscale;
					uv[1] *= vscale;
				}
				fogCoef = 1.0f;
				// Scale UV?
			} else {
				// We do software T&L for now
				float out[3], norm[3];
				float pos[3], nrm[3];
				Vec3f normal(0, 0, 1);
				reader.ReadPos(pos);
				if (reader.hasNormal())
					reader.ReadNrm(nrm);

				if (!vertTypeIsSkinningEnabled(vertType)) {
					Vec3ByMatrix43(out, pos, gstate.worldMatrix);
					if (reader.hasNormal()) {
						Norm3ByMatrix43(norm, nrm, gstate.worldMatrix);
						normal = Vec3f(norm).Normalized();
					}
				} else {
					float weights[8];
					reader.ReadWeights(weights);
					// Skinning
					Vec3f psum(0,0,0);
					Vec3f nsum(0,0,0);
					for (int i = 0; i < vertTypeGetNumBoneWeights(vertType); i++) {
						if (weights[i] != 0.0f) {
							Vec3ByMatrix43(out, pos, gstate.boneMatrix+i*12);
							Vec3f tpos(out);
							psum += tpos * weights[i];
							if (reader.hasNormal()) {
								Norm3ByMatrix43(norm, nrm, gstate.boneMatrix+i*12);
								Vec3f tnorm(norm);
								nsum += tnorm * weights[i];
							}
						}
					}

					// Yes, we really must multiply by the world matrix too.
					Vec3ByMatrix43(out, psum.AsArray(), gstate.worldMatrix);
					if (reader.hasNormal()) {
						Norm3ByMatrix43(norm, nsum.AsArray(), gstate.worldMatrix);
						normal = Vec3f(norm).Normalized();
					}
				}

				// Perform lighting here if enabled. don't need to check through, it's checked above.
				float unlitColor[4] = {1, 1, 1, 1};
				if (reader.hasColor0()) {
					reader.ReadColor0(unlitColor);
				} else {
					unlitColor[0] = gstate.getMaterialAmbientA() / 255.f;
					unlitColor[1] = gstate.getMaterialAmbientR() / 255.f;
					unlitColor[2] = gstate.getMaterialAmbientG() / 255.f;
					unlitColor[3] = gstate.getMaterialAmbientB() / 255.f;
				}
				float litColor0[4];
				float litColor1[4];
				lighter.Light(litColor0, litColor1, unlitColor, out, normal);

				if (gstate.isLightingEnabled()) {
					// Don't ignore gstate.lmode - we should send two colors in that case
					for (int j = 0; j < 4; j++) {
						c0[j] = litColor0[j];
					}
					if (lmode) {
						// Separate colors
						for (int j = 0; j < 4; j++) {
							c1[j] = litColor1[j];
						}
					} else {
						// Summed color into c0
						for (int j = 0; j < 4; j++) {
							c0[j] = ((c0[j] + litColor1[j]) > 1.0f) ? 1.0f : (c0[j] + litColor1[j]);
						}
					}
				} else {
					if (reader.hasColor0()) {
						for (int j = 0; j < 4; j++) {
							c0[j] = unlitColor[j];
						}
					} else {
						c0[0] = gstate.getMaterialAmbientA() / 255.f;
						c0[1] = gstate.getMaterialAmbientR() / 255.f;
						c0[2] = gstate.getMaterialAmbientG() / 255.f;
						c0[3] = gstate.getMaterialAmbientB() / 255.f;
					}
					if (lmode) {
						for (int j = 0; j < 4; j++) {
							c1[j] = 0.0f;
						}
					}
				}

				float ruv[2] = {0.0f, 0.0f};
				if (reader.hasUV())
					reader.ReadUV(ruv);

				// Perform texture coordinate generation after the transform and lighting - one style of UV depends on lights.
			switch (gstate.getUVGenMode()) {
				case GE_TEXMAP_TEXTURE_COORDS:	// UV mapping
				case GE_TEXMAP_UNKNOWN: // Seen in Riviera.  Unsure of meaning, but this works.
					// Texture scale/offset is only performed in this mode.
					uv[0] = uscale * (ruv[0]*gstate_c.uv.uScale + gstate_c.uv.uOff);
					uv[1] = vscale * (ruv[1]*gstate_c.uv.vScale + gstate_c.uv.vOff);
					uv[2] = 1.0f;
					break;
				
					case GE_TEXMAP_TEXTURE_MATRIX:
					{
						// Projection mapping
						Vec3f source;
					switch (gstate.getUVProjMode())	{
						case GE_PROJMAP_POSITION: // Use model space XYZ as source
							source = pos;
							break;
						
						case GE_PROJMAP_UV: // Use unscaled UV as source
							source = Vec3f(ruv[0], ruv[1], 0.0f);
							break;
						
						case GE_PROJMAP_NORMALIZED_NORMAL: // Use normalized normal as source
							if (reader.hasNormal()) {
								source = Vec3f(norm).Normalized();
							} else {
								ERROR_LOG_REPORT(G3D, "Normal projection mapping without normal?");
								source = Vec3f(0.0f, 0.0f, 1.0f);
							}
							break;
						
						case GE_PROJMAP_NORMAL: // Use non-normalized normal as source!
							if (reader.hasNormal()) {
								source = Vec3f(norm);
							} else {
								ERROR_LOG_REPORT(G3D, "Normal projection mapping without normal?");
								source = Vec3f(0.0f, 0.0f, 1.0f);
							}
							break;
						}

						float uvw[3];
						Vec3ByMatrix43(uvw, &source.x, gstate.tgenMatrix);
						uv[0] = uvw[0];
						uv[1] = uvw[1];
						uv[2] = uvw[2];
					}
					break;
				
				case GE_TEXMAP_ENVIRONMENT_MAP:
					// Shade mapping - use two light sources to generate U and V.
					{
						Vec3f lightpos0 = Vec3f(gstate_c.lightpos[gstate.getUVLS0()]).Normalized();
						Vec3f lightpos1 = Vec3f(gstate_c.lightpos[gstate.getUVLS1()]).Normalized();

						uv[0] = (1.0f + Dot(lightpos0, normal))/2.0f;
						uv[1] = (1.0f - Dot(lightpos1, normal))/2.0f;
						uv[2] = 1.0f;
					}
					break;
				
				default:
					// Illegal
				ERROR_LOG_REPORT(G3D, "Impossible UV gen mode? %d", gstate.getUVGenMode());
					break;
				}

				uv[0] = uv[0] * widthFactor;
				uv[1] = uv[1] * heightFactor;

				// Transform the coord by the view matrix.
				Vec3ByMatrix43(v, out, gstate.viewMatrix);
				fogCoef = (v[2] + fog_end) * fog_slope;
			}

			// TODO: Write to a flexible buffer, we don't always need all four components.
			memcpy(&transformed[index].x, v, 3 * sizeof(float));
			transformed[index].fog = fogCoef;
			memcpy(&transformed[index].u, uv, 3 * sizeof(float));
			if (gstate_c.flipTexture) {
				transformed[index].v = 1.0f - transformed[index].v;
			}
			for (int i = 0; i < 4; i++) {
				transformed[index].color0[i] = c0[i] * 255.0f;
			}
			for (int i = 0; i < 3; i++) {
				transformed[index].color1[i] = c1[i] * 255.0f;
			}
		}

		// Step 2: expand rectangles.
		const TransformedVertex *drawBuffer = transformed;
		int numTrans = 0;

		bool drawIndexed = false;

		if (prim != GE_PRIM_RECTANGLES) {
			// We can simply draw the unexpanded buffer.
			numTrans = vertexCount;
			drawIndexed = true;
		} else {
			numTrans = 0;
			drawBuffer = transformedExpanded;
			TransformedVertex *trans = &transformedExpanded[0];
			TransformedVertex saved;
			u32 stencilValue;
			for (int i = 0; i < vertexCount; i += 2) {
				int index = ((const u16*)inds)[i];
				saved = transformed[index];
				int index2 = ((const u16*)inds)[i + 1];
				TransformedVertex &transVtx = transformed[index2];
				if (i == 0)
					stencilValue = transVtx.color0[3];
				// We have to turn the rectangle into two triangles, so 6 points. Sigh.

				// bottom right
				trans[0] = transVtx;

				// bottom left
				trans[1] = transVtx;
				trans[1].y = saved.y;
				trans[1].v = saved.v;

				// top left
				trans[2] = transVtx;
				trans[2].x = saved.x;
				trans[2].y = saved.y;
				trans[2].u = saved.u;
				trans[2].v = saved.v;

				// top right
				trans[3] = transVtx;
				trans[3].x = saved.x;
				trans[3].u = saved.u;

				// That's the four corners. Now process UV rotation.
				if (throughmode)
					RotateUVThrough(trans);

				// Apparently, non-through RotateUV just breaks things.
				// If we find a game where it helps, we'll just have to figure out how they differ.
				// Possibly, it has something to do with flipped viewport Y axis, which a few games use.
				// One game might be one of the Metal Gear ones, can't find the issue right now though.
				// else
				//	RotateUV(trans);

				// bottom right
				trans[4] = trans[0];

				// top left
				trans[5] = trans[2];
				trans += 6;

				numTrans += 6;
			}

			// We don't know the color until here, so we have to do it now, instead of in StateMapping.
			// Might want to reconsider the order of things later...
			if (gstate.isModeClear() && gstate.isClearModeAlphaMask()) {
				dxstate.stencilFunc.set(D3DCMP_ALWAYS, stencilValue, 255);
			}
		}


		// TODO: Add a post-transform cache here for multi-RECTANGLES only.
		// Might help for text drawing.

		// these spam the gDebugger log.
		const int vertexSize = sizeof(transformed[0]);

		pD3Ddevice->SetVertexDeclaration( pSoftVertexDecl );

		/// Debug !!
		//pD3Ddevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
#ifdef _XBOX
		if (drawIndexed) {
			pD3Ddevice->DrawIndexedVerticesUP(glprim[prim], 0, vertexCount, numTrans, inds, D3DFMT_INDEX16, drawBuffer, sizeof(TransformedVertex));
		} else {
			pD3Ddevice->DrawVerticesUP(glprim[prim], numTrans, drawBuffer, sizeof(TransformedVertex));
		}
#else
		if (drawIndexed) {
			pD3Ddevice->DrawIndexedPrimitiveUP(glprim[prim], 0, vertexCount, D3DPrimCount(glprim[prim], numTrans), inds, D3DFMT_INDEX16, drawBuffer, sizeof(TransformedVertex));
		} else {
			pD3Ddevice->DrawPrimitiveUP(glprim[prim], D3DPrimCount(glprim[prim], numTrans), drawBuffer, sizeof(TransformedVertex));
		}
#endif
}

VertexDecoderDX9 *TransformDrawEngineDX9::GetVertexDecoder(u32 vtype) {
	auto iter = decoderMap_.find(vtype);
	if (iter != decoderMap_.end())
		return iter->second;
	VertexDecoderDX9 *dec = new VertexDecoderDX9(); 
	dec->SetVertexType(vtype, decJitCache_);
	decoderMap_[vtype] = dec;
	return dec;
}

void TransformDrawEngineDX9::SetupVertexDecoder(u32 vertType) {
	// If vtype has changed, setup the vertex decoder.
	// TODO: Simply cache the setup decoders instead.
	if (vertType != lastVType_) {
		dec_ = GetVertexDecoder(vertType);
		lastVType_ = vertType;
	}
}

int TransformDrawEngineDX9::EstimatePerVertexCost() {
	// TODO: This is transform cost, also account for rasterization cost somehow... although it probably
	// runs in parallel with transform.

	// Also, this is all pure guesswork. If we can find a way to do measurements, that would be great.

	// GTA wants a low value to run smooth, GoW wants a high value (otherwise it thinks things
	// went too fast and starts doing all the work over again).

	int cost = 20;
	if (gstate.isLightingEnabled()) {
		cost += 10;
	}

	for (int i = 0; i < 4; i++) {
		if (gstate.isLightChanEnabled(i))
			cost += 10;
	}
	if (gstate.getUVGenMode() != GE_TEXMAP_TEXTURE_COORDS) {
		cost += 20;
	}
	if (dec_ && dec_->morphcount > 1) {
		cost += 5 * dec_->morphcount;
	}

	return cost;
}

void TransformDrawEngineDX9::SubmitPrim(void *verts, void *inds, GEPrimitiveType prim, int vertexCount, u32 vertType, int forceIndexType, int *bytesRead) {
	if (vertexCount == 0)
		return;  // we ignore zero-sized draw calls.

	if (!indexGen.PrimCompatible(prevPrim_, prim) || numDrawCalls >= MAX_DEFERRED_DRAW_CALLS || vertexCountInDrawCalls + vertexCount > VERTEX_BUFFER_MAX)
		Flush();
		
	// TODO: Is this the right thing to do?
	if (prim == GE_PRIM_KEEP_PREVIOUS) {
		prim = prevPrim_;
	}
	prevPrim_ = prim;
	
	SetupVertexDecoder(vertType);

	dec_->IncrementStat(STAT_VERTSSUBMITTED, vertexCount);

	if (bytesRead)
		*bytesRead = vertexCount * dec_->VertexSize();

	gpuStats.numDrawCalls++;
	gpuStats.numVertsSubmitted += vertexCount;

	DeferredDrawCall &dc = drawCalls[numDrawCalls];
	dc.verts = verts;
	dc.inds = inds;
	dc.vertType = vertType;
	dc.indexType = ((forceIndexType == -1) ? (vertType & GE_VTYPE_IDX_MASK) : forceIndexType) >> GE_VTYPE_IDX_SHIFT;
	dc.prim = prim;
	dc.vertexCount = vertexCount;
	if (inds) {
		GetIndexBounds(inds, vertexCount, vertType, &dc.indexLowerBound, &dc.indexUpperBound);
	} else {
		dc.indexLowerBound = 0;
		dc.indexUpperBound = vertexCount - 1;
	}

	if (uvScale) {
		uvScale[numDrawCalls] = gstate_c.uv;
	}
	numDrawCalls++;
	vertexCountInDrawCalls += vertexCount;
}

void TransformDrawEngineDX9::DecodeVerts() {
	UVScale origUV;
	if (uvScale)
		origUV = gstate_c.uv;
	for (int i = 0; i < numDrawCalls; i++) {
		const DeferredDrawCall &dc = drawCalls[i];

		indexGen.SetIndex(collectedVerts);
		int indexLowerBound = dc.indexLowerBound, indexUpperBound = dc.indexUpperBound;

		u32 indexType = dc.indexType;
		void *inds = dc.inds;
		if (indexType == GE_VTYPE_IDX_NONE >> GE_VTYPE_IDX_SHIFT) {
			// Decode the verts and apply morphing. Simple.
			if (uvScale)
				gstate_c.uv = uvScale[i];
			dec_->DecodeVerts(decoded + collectedVerts * (int)dec_->GetDecVtxFmt().stride,
				dc.verts, indexLowerBound, indexUpperBound);
			collectedVerts += indexUpperBound - indexLowerBound + 1;
			indexGen.AddPrim(dc.prim, dc.vertexCount);
		} else {
			// It's fairly common that games issue long sequences of PRIM calls, with differing
			// inds pointer but the same base vertex pointer. We'd like to reuse vertices between
			// these as much as possible, so we make sure here to combine as many as possible
			// into one nice big drawcall, sharing data.

			// 1. Look ahead to find the max index, only looking as "matching" drawcalls.
			//    Expand the lower and upper bounds as we go.
			int j = i + 1;
			int lastMatch = i;
			while (j < numDrawCalls) {
				if (drawCalls[j].verts != dc.verts)
					break;
				if (uvScale && memcmp(&uvScale[j], &uvScale[i], sizeof(uvScale[0])) != 0)
					break;

				indexLowerBound = std::min(indexLowerBound, (int)drawCalls[j].indexLowerBound);
				indexUpperBound = std::max(indexUpperBound, (int)drawCalls[j].indexUpperBound);
				lastMatch = j;
				j++;
			}

			// 2. Loop through the drawcalls, translating indices as we go.
			for (j = i; j <= lastMatch; j++) {
				switch (indexType) {
				case GE_VTYPE_IDX_8BIT >> GE_VTYPE_IDX_SHIFT:
					indexGen.TranslatePrim(drawCalls[j].prim, drawCalls[j].vertexCount, (const u8 *)drawCalls[j].inds, indexLowerBound);
					break;
				case GE_VTYPE_IDX_16BIT >> GE_VTYPE_IDX_SHIFT:
					indexGen.TranslatePrim(drawCalls[j].prim, drawCalls[j].vertexCount, (const u16 *)drawCalls[j].inds, indexLowerBound);
					break;
				}
			}

			int vertexCount = indexUpperBound - indexLowerBound + 1;
			// 3. Decode that range of vertex data.
			if (uvScale)
				gstate_c.uv = uvScale[i];
			dec_->DecodeVerts(decoded + collectedVerts * (int)dec_->GetDecVtxFmt().stride,
				dc.verts, indexLowerBound, indexUpperBound);
			collectedVerts += vertexCount;

			// 4. Advance indexgen vertex counter.
			indexGen.Advance(vertexCount);
			i = lastMatch;
		}
	}

	// Sanity check
	if (indexGen.Prim() < 0) {
		ERROR_LOG_REPORT(G3D, "DecodeVerts: Failed to deduce prim: %i", indexGen.Prim());
		// Force to points (0)
		indexGen.AddPrim(GE_PRIM_POINTS, 0);
	}
	if (uvScale)
		gstate_c.uv = origUV;
}

u32 TransformDrawEngineDX9::ComputeHash() {
	u32 fullhash = 0;
    int vertexSize = dec_->GetDecVtxFmt().stride;

    // TODO: Add some caps both for numDrawCalls and num verts to check?
    // It is really very expensive to check all the vertex data so often.
    for (int i = 0; i < numDrawCalls; i++) {
        const DeferredDrawCall &dc = drawCalls[i];
        if (!dc.inds) {
            fullhash += XXH32((const char *)dc.verts, vertexSize * dc.vertexCount, 0x1DE8CAC4);
        } else {
            int indexLowerBound = dc.indexLowerBound, indexUpperBound = dc.indexUpperBound;
            int j = i + 1;
            int lastMatch = i;
            while (j < numDrawCalls) {
                    if (drawCalls[j].verts != dc.verts)
                            break;
                    indexLowerBound = std::min(indexLowerBound, (int)dc.indexLowerBound);
                    indexUpperBound = std::max(indexUpperBound, (int)dc.indexUpperBound);
                    lastMatch = j;
                    j++;
            }
            // This could get seriously expensive with sparse indices. Need to combine hashing ranges the same way
            // we do when drawing.
            fullhash += XXH32((const char *)dc.verts + vertexSize * indexLowerBound,
                    vertexSize * (indexUpperBound - indexLowerBound), 0x029F3EE1);
            int indexSize = (dec_->VertexType() & GE_VTYPE_IDX_MASK) == GE_VTYPE_IDX_16BIT ? 2 : 1;
            // Hm, we will miss some indices when combining above, but meh, it should be fine.
            fullhash += XXH32((const char *)dc.inds, indexSize * dc.vertexCount, 0x955FD1CA);
            i = lastMatch;
        }
    }
    if (uvScale) {
            fullhash += XXH32(&uvScale[0], sizeof(uvScale[0]) * numDrawCalls, 0x0123e658);
    }

    return fullhash;
}

u32 TransformDrawEngineDX9::ComputeFastDCID() {
	u32 hash = 0;
	for (int i = 0; i < numDrawCalls; i++) {
		hash ^= (u32)(uintptr_t)drawCalls[i].verts;
		hash = __rotl(hash, 13);
		hash ^= (u32)(uintptr_t)drawCalls[i].inds;
		hash = __rotl(hash, 13);
		hash ^= (u32)drawCalls[i].vertType;
		hash = __rotl(hash, 13);
		hash ^= (u32)drawCalls[i].vertexCount;
		hash = __rotl(hash, 13);
		hash ^= (u32)drawCalls[i].prim;
	}
	return hash;
}

#ifdef _XBOX
enum { VAI_KILL_AGE = 60 };
#else
enum { VAI_KILL_AGE = 120 };
#endif

void TransformDrawEngineDX9::ClearTrackedVertexArrays() {
	for (auto vai = vai_.begin(); vai != vai_.end(); vai++) {
		delete vai->second;
	}
	vai_.clear();
}

void TransformDrawEngineDX9::DecimateTrackedVertexArrays() {
	if (--decimationCounter_ <= 0) {
		decimationCounter_ = VERTEXCACHE_DECIMATION_INTERVAL;
	} else {
		return;
	}

	int threshold = gpuStats.numFlips - VAI_KILL_AGE;
	for (auto iter = vai_.begin(); iter != vai_.end(); ) {
		if (iter->second->lastFrame < threshold) {
			delete iter->second;
			vai_.erase(iter++);
		}
		else
			++iter;
	}

	// Enable if you want to see vertex decoders in the log output. Need a better way.
#if 0
	char buffer[16384];
	for (std::map<u32, VertexDecoder*>::iterator dec = decoderMap_.begin(); dec != decoderMap_.end(); ++dec) {
		char *ptr = buffer;
		ptr += dec->second->ToString(ptr);
		//		*ptr++ = '\n';
		NOTICE_LOG(G3D, buffer);
	}
#endif
}

VertexArrayInfoDX9::~VertexArrayInfoDX9() {
	if (vbo) {
		vbo->Release();
	}
	if (ebo) {
		ebo->Release();
	}
}

void TransformDrawEngineDX9::DoFlush() {
	gpuStats.numFlushes++;

	gpuStats.numTrackedVertexArrays = (int)vai_.size();

	// This is not done on every drawcall, we should collect vertex data
	// until critical state changes. That's when we draw (flush).

	GEPrimitiveType prim = prevPrim_;
	ApplyDrawState(prim);

	LinkedShaderDX9 *program = shaderManager_->ApplyShader(prim, lastVType_);

		if (program->useHWTransform_) {
			LPDIRECT3DVERTEXBUFFER9 vb_ = NULL;
			LPDIRECT3DINDEXBUFFER9 ib_ = NULL;

			int vertexCount = 0;
			int maxIndex = 0;
			bool useElements = true;

			// Cannot cache vertex data with morph enabled.
			if (g_Config.bVertexCache && !(lastVType_ & GE_VTYPE_MORPHCOUNT_MASK)) {
				u32 id = ComputeFastDCID();
				auto iter = vai_.find(id);
				VertexArrayInfoDX9 *vai;
				if (iter != vai_.end()) {
					// We've seen this before. Could have been a cached draw.
					vai = iter->second;
				} else {
					vai = new VertexArrayInfoDX9();
					vai_[id] = vai;
				}

				switch (vai->status) {
				case VertexArrayInfoDX9::VAI_NEW:
					{
						// Haven't seen this one before.
						u32 dataHash = ComputeHash();
						vai->hash = dataHash;
						vai->status = VertexArrayInfoDX9::VAI_HASHING;
						vai->drawsUntilNextFullHash = 0;
						DecodeVerts(); // writes to indexGen
						vai->numVerts = indexGen.VertexCount();
						vai->prim = indexGen.Prim();
						vai->maxIndex = indexGen.MaxIndex();
						goto rotateVBO;
					}

					// Hashing - still gaining confidence about the buffer.
					// But if we get this far it's likely to be worth creating a vertex buffer.
				case VertexArrayInfoDX9::VAI_HASHING:
					{
						vai->numDraws++;
						if (vai->lastFrame != gpuStats.numFlips) {
							vai->numFrames++;
						}
						if (vai->drawsUntilNextFullHash == 0) {
							u32 newHash = ComputeHash();
							if (newHash != vai->hash) {
								vai->status = VertexArrayInfoDX9::VAI_UNRELIABLE;
								if (vai->vbo) {
									vai->vbo->Release();
									vai->vbo = NULL;
								}
								if (vai->ebo) {
									vai->ebo->Release();
									vai->ebo = NULL;
								}
								DecodeVerts();
								goto rotateVBO;
							}
							if (vai->numVerts > 100) {
								// exponential backoff up to 16 draws, then every 24
								vai->drawsUntilNextFullHash = std::min(24, vai->numFrames);
							} else {
								// Lower numbers seem much more likely to change.
								vai->drawsUntilNextFullHash = 0;
							}
							// TODO: tweak
							//if (vai->numFrames > 1000) {
							//	vai->status = VertexArrayInfo::VAI_RELIABLE;
							//}
						} else {
							vai->drawsUntilNextFullHash--;
							// TODO: "mini-hashing" the first 32 bytes of the vertex/index data or something.
						}

						if (vai->vbo == 0) {
							DecodeVerts();
							vai->numVerts = indexGen.VertexCount();
							vai->prim = indexGen.Prim();
							vai->maxIndex = indexGen.MaxIndex();
							useElements = !indexGen.SeenOnlyPurePrims();
							if (!useElements && indexGen.PureCount()) {
								vai->numVerts = indexGen.PureCount();
							}
							// Always
							if (1) {
								void * pVb;
								u32 size = dec_->GetDecVtxFmt().stride * indexGen.MaxIndex();
								pD3Ddevice->CreateVertexBuffer(size, NULL, NULL, D3DPOOL_DEFAULT, &vai->vbo, NULL);
								vai->vbo->Lock(0, size, &pVb, D3DLOCK_NOOVERWRITE );
								memcpy(pVb, decoded, size);
								vai->vbo->Unlock();
							}
							// Ib
							if (useElements) {
								void * pIb;
								u32 size =  sizeof(short) * indexGen.VertexCount();
								pD3Ddevice->CreateIndexBuffer(size, NULL, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &vai->ebo, NULL);
								vai->ebo->Lock(0, size, &pIb, D3DLOCK_NOOVERWRITE );
								memcpy(pIb, decIndex, size);
								vai->ebo->Unlock();
							} else {
								vai->ebo = 0;
							}
						} else {
							gpuStats.numCachedDrawCalls++;
							useElements = vai->ebo ? true : false;
							gpuStats.numCachedVertsDrawn += vai->numVerts;
						}
						vb_ = vai->vbo;
						ib_ = vai->ebo;
						vertexCount = vai->numVerts;
						maxIndex = vai->maxIndex;
						prim = static_cast<GEPrimitiveType>(vai->prim);
						break;
					}

					// Reliable - we don't even bother hashing anymore. Right now we don't go here until after a very long time.
				case VertexArrayInfoDX9::VAI_RELIABLE:
					{
						vai->numDraws++;
						if (vai->lastFrame != gpuStats.numFlips) {
							vai->numFrames++;
						}
						gpuStats.numCachedDrawCalls++;
						gpuStats.numCachedVertsDrawn += vai->numVerts;
						vb_ = vai->vbo;
						ib_ = vai->ebo;

						vertexCount = vai->numVerts;
						
						maxIndex = vai->maxIndex;
						prim = static_cast<GEPrimitiveType>(vai->prim);
						break;
					}

				case VertexArrayInfoDX9::VAI_UNRELIABLE:
					{
						vai->numDraws++;
						if (vai->lastFrame != gpuStats.numFlips) {
							vai->numFrames++;
						}
						DecodeVerts();
						goto rotateVBO;
					}
				}

				vai->lastFrame = gpuStats.numFlips;
			} else {
				DecodeVerts();
rotateVBO:
				gpuStats.numUncachedVertsDrawn += indexGen.VertexCount();
				useElements = !indexGen.SeenOnlyPurePrims();
				vertexCount = indexGen.VertexCount();				
				maxIndex = indexGen.MaxIndex();
				if (!useElements && indexGen.PureCount()) {
					vertexCount = indexGen.PureCount();
				}
				prim = indexGen.Prim();
			}

			DEBUG_LOG(G3D, "Flush prim %i! %i verts in one go", prim, vertexCount);

			SetupDecFmtForDraw(program, dec_->GetDecVtxFmt(), dec_->VertexType());
			pD3Ddevice->SetVertexDeclaration(pHardwareVertexDecl);
#ifdef _XBOX
			if (vb_ == NULL) {
				if (useElements) {
					pD3Ddevice->DrawIndexedVerticesUP(glprim[prim], 0, vertexCount, vertexCount, decIndex, D3DFMT_INDEX16, decoded, dec_->GetDecVtxFmt().stride);
				} else {
					pD3Ddevice->DrawVerticesUP(glprim[prim], vertexCount, decoded, dec_->GetDecVtxFmt().stride);
				}
			} else {
				pD3Ddevice->SetStreamSource(0, vb_, 0, dec_->GetDecVtxFmt().stride);

				if (useElements) {					
					pD3Ddevice->SetIndices(ib_);

					pD3Ddevice->DrawIndexedVertices(glprim[prim], 0, 0, vertexCount);
				} else {
					pD3Ddevice->DrawVertices(glprim[prim], 0, vertexCount);
				}
			}
#else
			if (vb_ == NULL) {
                if (useElements) {
                    pD3Ddevice->DrawIndexedPrimitiveUP(glprim[prim], 0, vertexCount, D3DPrimCount(glprim[prim], vertexCount), decIndex, D3DFMT_INDEX16, decoded, dec_->GetDecVtxFmt().stride);
                } else {
                    pD3Ddevice->DrawPrimitiveUP(glprim[prim], D3DPrimCount(glprim[prim], vertexCount), decoded, dec_->GetDecVtxFmt().stride);
                }
            } else {
                pD3Ddevice->SetStreamSource(0, vb_, 0, dec_->GetDecVtxFmt().stride);

                if (useElements) {                                        
                    pD3Ddevice->SetIndices(ib_);

                    pD3Ddevice->DrawIndexedPrimitive(glprim[prim], 0, 0, 0, 0, D3DPrimCount(glprim[prim], vertexCount));
                } else {
                    pD3Ddevice->DrawPrimitive(glprim[prim], 0, D3DPrimCount(glprim[prim], vertexCount));
                }
            }

#endif
		} else {
			DecodeVerts();
			gpuStats.numUncachedVertsDrawn += indexGen.VertexCount();
			prim = indexGen.Prim();
			// Undo the strip optimization, not supported by the SW code yet.
			if (prim == GE_PRIM_TRIANGLE_STRIP)
				prim = GE_PRIM_TRIANGLES;
			DEBUG_LOG(G3D, "Flush prim %i SW! %i verts in one go", prim, indexGen.VertexCount());

			SoftwareTransformAndDraw(
				prim, decoded, program, indexGen.VertexCount(), 
				dec_->VertexType(), (void *)decIndex, GE_VTYPE_IDX_16BIT, dec_->GetDecVtxFmt(),
				indexGen.MaxIndex());
		}

		indexGen.Reset();
		collectedVerts = 0;
		numDrawCalls = 0;
		vertexCountInDrawCalls = 0;
		prevPrim_ = GE_PRIM_INVALID;

#ifndef _XBOX
		host->GPUNotifyDraw();
#endif
}
bool TransformDrawEngineDX9::TestBoundingBox(void* control_points, int vertexCount, u32 vertType) {
	// Simplify away bones and morph before proceeding

	/*
	SimpleVertex *corners = (SimpleVertex *)(decoded + 65536 * 12);
	u8 *temp_buffer = decoded + 65536 * 24;

	u32 origVertType = vertType;
	vertType = NormalizeVertices((u8 *)corners, temp_buffer, (u8 *)control_points, 0, vertexCount, vertType);

	for (int cube = 0; cube < vertexCount / 8; cube++) {
		// For each cube...
		
		for (int i = 0; i < 8; i++) {
			const SimpleVertex &vert = corners[cube * 8 + i];

			// To world space...
			float worldPos[3];
			Vec3ByMatrix43(worldPos, (float *)&vert.pos.x, gstate.worldMatrix);

			// To view space...
			float viewPos[3];
			Vec3ByMatrix43(viewPos, worldPos, gstate.viewMatrix);

			// And finally to screen space.
			float frustumPos[4];
			Vec3ByMatrix44(frustumPos, viewPos, gstate.projMatrix);

			// Project to 2D
			float x = frustumPos[0] / frustumPos[3];
			float y = frustumPos[1] / frustumPos[3];

			// Rescale 2d position
			// ...
		}
	}
	*/

	
	// Let's think. A better approach might be to take the edges of the drawing region and the projection
	// matrix to build a frustum pyramid, and then clip the cube against those planes. If all vertices fail the same test,
	// the cube is out. Otherwise it's in.
	// TODO....
	
	return true;
}
};
