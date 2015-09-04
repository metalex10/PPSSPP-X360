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

#include "native/gfx_es2/gl_state.h"
#include "ext/xxhash.h"

#include "GPU/Math3D.h"
#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"

#include "GPU/Common/SplineCommon.h"
#include "GPU/GLES/StateMapping.h"
#include "GPU/GLES/TextureCache.h"
#include "GPU/GLES/TransformPipeline.h"
#include "GPU/GLES/VertexDecoder.h"
#include "GPU/GLES/ShaderManager.h"
#include "GPU/GLES/GLES_GPU.h"
#include "GPU/Common/SplineCommon.h"

extern const GLuint glprim[8] = {
	GL_POINTS,
	GL_LINES,
	GL_LINE_STRIP,
	GL_TRIANGLES,
	GL_TRIANGLE_STRIP,
	GL_TRIANGLE_FAN,
	GL_TRIANGLES,	 // With OpenGL ES we have to expand sprites into triangles, tripling the data instead of doubling. sigh. OpenGL ES, Y U NO SUPPORT GL_QUADS?
};

enum {
	VERTEX_BUFFER_MAX = 65536,
	DECODED_VERTEX_BUFFER_SIZE = VERTEX_BUFFER_MAX * 48,
	DECODED_INDEX_BUFFER_SIZE = VERTEX_BUFFER_MAX * 20,
	TRANSFORMED_VERTEX_BUFFER_SIZE = VERTEX_BUFFER_MAX * sizeof(TransformedVertex)
};

#define QUAD_INDICES_MAX 32768

#define VERTEXCACHE_DECIMATION_INTERVAL 17

TransformDrawEngine::TransformDrawEngine()
	: collectedVerts(0),
		prevPrim_(GE_PRIM_INVALID),
		dec_(0),
		lastVType_(-1),
		curVbo_(0),
		shaderManager_(0),
		textureCache_(0),
		framebufferManager_(0),
		numDrawCalls(0),
		vertexCountInDrawCalls(0),
		uvScale(0),
		decodeCounter_(0) {
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
	memset(vbo_, 0, sizeof(vbo_));
	memset(ebo_, 0, sizeof(ebo_));
	indexGen.Setup(decIndex);
	decJitCache_ = new VertexDecoderJitCache();

	InitDeviceObjects();
	register_gl_resource_holder(this);
}

TransformDrawEngine::~TransformDrawEngine() {
	DestroyDeviceObjects();
	FreeMemoryPages(decoded, DECODED_VERTEX_BUFFER_SIZE);
	FreeMemoryPages(decIndex, DECODED_INDEX_BUFFER_SIZE);
	FreeMemoryPages(transformed, TRANSFORMED_VERTEX_BUFFER_SIZE);
	FreeMemoryPages(transformedExpanded, 3 * TRANSFORMED_VERTEX_BUFFER_SIZE);
	delete [] quadIndices_;

	unregister_gl_resource_holder(this);
	delete decJitCache_;
	for (auto iter = decoderMap_.begin(); iter != decoderMap_.end(); iter++) {
		delete iter->second;
	}
	delete [] uvScale;
}

void TransformDrawEngine::InitDeviceObjects() {
	if (!vbo_[0]) {
		glGenBuffers(NUM_VBOS, &vbo_[0]);
		glGenBuffers(NUM_VBOS, &ebo_[0]);
	} else {
		ERROR_LOG(G3D, "Device objects already initialized!");
	}
}

void TransformDrawEngine::DestroyDeviceObjects() {
	glDeleteBuffers(NUM_VBOS, &vbo_[0]);
	glDeleteBuffers(NUM_VBOS, &ebo_[0]);
	memset(vbo_, 0, sizeof(vbo_));
	memset(ebo_, 0, sizeof(ebo_));
	ClearTrackedVertexArrays();
}

void TransformDrawEngine::GLLost() {
	ILOG("TransformDrawEngine::GLLost()");
	// The objects have already been deleted.
	memset(vbo_, 0, sizeof(vbo_));
	memset(ebo_, 0, sizeof(ebo_));
	ClearTrackedVertexArrays();
	InitDeviceObjects();
}

struct GlTypeInfo {
	u16 type;
	u8 count;
	u8 normalized;
};

static const GlTypeInfo GLComp[] = {
	{0}, // 	DEC_NONE,
	{GL_FLOAT, 1, GL_FALSE}, // 	DEC_FLOAT_1,
	{GL_FLOAT, 2, GL_FALSE}, // 	DEC_FLOAT_2,
	{GL_FLOAT, 3, GL_FALSE}, // 	DEC_FLOAT_3,
	{GL_FLOAT, 4, GL_FALSE}, // 	DEC_FLOAT_4,
	{GL_BYTE, 4, GL_TRUE}, // 	DEC_S8_3,
	{GL_SHORT, 4, GL_TRUE},// 	DEC_S16_3,
	{GL_UNSIGNED_BYTE, 1, GL_TRUE},// 	DEC_U8_1,
	{GL_UNSIGNED_BYTE, 2, GL_TRUE},// 	DEC_U8_2,
	{GL_UNSIGNED_BYTE, 3, GL_TRUE},// 	DEC_U8_3,
	{GL_UNSIGNED_BYTE, 4, GL_TRUE},// 	DEC_U8_4,
	{GL_UNSIGNED_SHORT, 1, GL_TRUE},// 	DEC_U16_1,
	{GL_UNSIGNED_SHORT, 2, GL_TRUE},// 	DEC_U16_2,
	{GL_UNSIGNED_SHORT, 3, GL_TRUE},// 	DEC_U16_3,
	{GL_UNSIGNED_SHORT, 4, GL_TRUE},// 	DEC_U16_4,
	{GL_UNSIGNED_BYTE,  2, GL_FALSE},// 	DEC_U8A_2,
	{GL_UNSIGNED_SHORT, 2, GL_FALSE},// 	DEC_U16A_2,
};

static inline void VertexAttribSetup(int attrib, int fmt, int stride, u8 *ptr) {
	if (attrib != -1 && fmt) {
		const GlTypeInfo &type = GLComp[fmt];
		glVertexAttribPointer(attrib, type.count, type.type, type.normalized, stride, ptr);
	}
}

// TODO: Use VBO and get rid of the vertexData pointers - with that, we will supply only offsets
static void SetupDecFmtForDraw(LinkedShader *program, const DecVtxFormat &decFmt, u8 *vertexData) {
	VertexAttribSetup(ATTR_W1, decFmt.w0fmt, decFmt.stride, vertexData + decFmt.w0off);
	VertexAttribSetup(ATTR_W2, decFmt.w1fmt, decFmt.stride, vertexData + decFmt.w1off);
	VertexAttribSetup(ATTR_TEXCOORD, decFmt.uvfmt, decFmt.stride, vertexData + decFmt.uvoff);
	VertexAttribSetup(ATTR_COLOR0, decFmt.c0fmt, decFmt.stride, vertexData + decFmt.c0off);
	VertexAttribSetup(ATTR_COLOR1, decFmt.c1fmt, decFmt.stride, vertexData + decFmt.c1off);
	VertexAttribSetup(ATTR_NORMAL, decFmt.nrmfmt, decFmt.stride, vertexData + decFmt.nrmoff);
	VertexAttribSetup(ATTR_POSITION, decFmt.posfmt, decFmt.stride, vertexData + decFmt.posoff);
}

VertexDecoder *TransformDrawEngine::GetVertexDecoder(u32 vtype) {
	auto iter = decoderMap_.find(vtype);
	if (iter != decoderMap_.end())
		return iter->second;
	VertexDecoder *dec = new VertexDecoder();
	dec->SetVertexType(vtype, decJitCache_);
	decoderMap_[vtype] = dec;
	return dec;
}

void TransformDrawEngine::SetupVertexDecoder(u32 vertType) {
	// As the decoder depends on the UVGenMode when we use UV prescale, we simply mash it
	// into the top of the verttype where there are unused bits.
	u32 vertTypeID = (vertType & 0xFFFFFF) | (gstate.getUVGenMode() << 24);

	// If vtype has changed, setup the vertex decoder.
	// TODO: Simply cache the setup decoders instead.
	if (vertTypeID != lastVType_) {
		dec_ = GetVertexDecoder(vertTypeID);
		lastVType_ = vertTypeID;
	}
}

int TransformDrawEngine::EstimatePerVertexCost() {
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

void TransformDrawEngine::SubmitPrim(void *verts, void *inds, GEPrimitiveType prim, int vertexCount, u32 vertType, int *bytesRead) {
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
	dc.indexType = (vertType & GE_VTYPE_IDX_MASK) >> GE_VTYPE_IDX_SHIFT;
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

	if (g_Config.bSoftwareSkinning && (vertType & GE_VTYPE_WEIGHT_MASK)) {
		DecodeVertsStep();
		decodeCounter_++;
	}
}

void TransformDrawEngine::DecodeVerts() {
	UVScale origUV;
	if (uvScale)
		origUV = gstate_c.uv;
	for (; decodeCounter_ < numDrawCalls; decodeCounter_++) {
		if (uvScale)
			gstate_c.uv = uvScale[decodeCounter_];
		DecodeVertsStep();
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

void TransformDrawEngine::DecodeVertsStep() {
	const int i = decodeCounter_;

	const DeferredDrawCall &dc = drawCalls[i];

	indexGen.SetIndex(collectedVerts);
	int indexLowerBound = dc.indexLowerBound, indexUpperBound = dc.indexUpperBound;

	u32 indexType = dc.indexType;
	void *inds = dc.inds;
	if (indexType == GE_VTYPE_IDX_NONE >> GE_VTYPE_IDX_SHIFT) {
		// Decode the verts and apply morphing. Simple.
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
		dec_->DecodeVerts(decoded + collectedVerts * (int)dec_->GetDecVtxFmt().stride,
			dc.verts, indexLowerBound, indexUpperBound);
		collectedVerts += vertexCount;

		// 4. Advance indexgen vertex counter.
		indexGen.Advance(vertexCount);
		decodeCounter_ = lastMatch;
	}
}

u32 TransformDrawEngine::ComputeHash() {
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

u32 TransformDrawEngine::ComputeFastDCID() {
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

enum { VAI_KILL_AGE = 120 };

void TransformDrawEngine::ClearTrackedVertexArrays() {
	for (auto vai = vai_.begin(); vai != vai_.end(); vai++) {
		delete vai->second;
	}
	vai_.clear();
}

void TransformDrawEngine::DecimateTrackedVertexArrays() {
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

VertexArrayInfo::~VertexArrayInfo() {
	if (vbo)
		glDeleteBuffers(1, &vbo);
	if (ebo)
		glDeleteBuffers(1, &ebo);
}

void TransformDrawEngine::DoFlush() {
	gpuStats.numFlushes++;
	
	gpuStats.numTrackedVertexArrays = (int)vai_.size();

	// This is not done on every drawcall, we should collect vertex data
	// until critical state changes. That's when we draw (flush).

	GEPrimitiveType prim = prevPrim_;
	ApplyDrawState(prim);

	LinkedShader *program = shaderManager_->ApplyShader(prim, lastVType_);

	if (program->useHWTransform_) {
		GLuint vbo = 0, ebo = 0;
		int vertexCount = 0;
		int maxIndex = 0;
		bool useElements = true;

		// Cannot cache vertex data with morph enabled.
		bool useCache = g_Config.bVertexCache && !(lastVType_ & GE_VTYPE_MORPHCOUNT_MASK);
		// Also avoid caching when software skinning.
		if (g_Config.bSoftwareSkinning && (lastVType_ & GE_VTYPE_WEIGHT_MASK))
			useCache = false;

		if (useCache) {
			u32 id = ComputeFastDCID();
			auto iter = vai_.find(id);
			VertexArrayInfo *vai;
			if (iter != vai_.end()) {
				// We've seen this before. Could have been a cached draw.
				vai = iter->second;
			} else {
				vai = new VertexArrayInfo();
				vai_[id] = vai;
			}

			switch (vai->status) {
			case VertexArrayInfo::VAI_NEW:
				{
					// Haven't seen this one before.
					u32 dataHash = ComputeHash();
					vai->hash = dataHash;
					vai->status = VertexArrayInfo::VAI_HASHING;
					vai->drawsUntilNextFullHash = 0;
					DecodeVerts(); // writes to indexGen
					vai->numVerts = indexGen.VertexCount();
					vai->prim = indexGen.Prim();
					vai->maxIndex = indexGen.MaxIndex();
					goto rotateVBO;
				}

				// Hashing - still gaining confidence about the buffer.
				// But if we get this far it's likely to be worth creating a vertex buffer.
			case VertexArrayInfo::VAI_HASHING:
				{
					vai->numDraws++;
					if (vai->lastFrame != gpuStats.numFlips) {
						vai->numFrames++;
					}
					if (vai->drawsUntilNextFullHash == 0) {
						u32 newHash = ComputeHash();
						if (newHash != vai->hash) {
							vai->status = VertexArrayInfo::VAI_UNRELIABLE;
							if (vai->vbo) {
								glDeleteBuffers(1, &vai->vbo);
								vai->vbo = 0;
							}
							if (vai->ebo) {
								glDeleteBuffers(1, &vai->ebo);
								vai->ebo = 0;
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
						
						glGenBuffers(1, &vai->vbo);
						glBindBuffer(GL_ARRAY_BUFFER, vai->vbo);
						glBufferData(GL_ARRAY_BUFFER, dec_->GetDecVtxFmt().stride * indexGen.MaxIndex(), decoded, GL_STATIC_DRAW);
						// If there's only been one primitive type, and it's either TRIANGLES, LINES or POINTS,
						// there is no need for the index buffer we built. We can then use glDrawArrays instead
						// for a very minor speed boost.
						if (useElements) {
							glGenBuffers(1, &vai->ebo);
							glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vai->ebo);
							glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(short) * indexGen.VertexCount(), (GLvoid *)decIndex, GL_STATIC_DRAW);
						} else {
							vai->ebo = 0;
							glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
						}
					} else {
						gpuStats.numCachedDrawCalls++;
						glBindBuffer(GL_ARRAY_BUFFER, vai->vbo);
						if (vai->ebo)
							glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vai->ebo);
						useElements = vai->ebo ? true : false;
						gpuStats.numCachedVertsDrawn += vai->numVerts;
					}
					vbo = vai->vbo;
					ebo = vai->ebo;
					vertexCount = vai->numVerts;
					maxIndex = vai->maxIndex;
					prim = static_cast<GEPrimitiveType>(vai->prim);
					break;
				}

				// Reliable - we don't even bother hashing anymore. Right now we don't go here until after a very long time.
			case VertexArrayInfo::VAI_RELIABLE:
				{
					vai->numDraws++;
					if (vai->lastFrame != gpuStats.numFlips) {
						vai->numFrames++;
					}
					gpuStats.numCachedDrawCalls++;
					gpuStats.numCachedVertsDrawn += vai->numVerts;
					vbo = vai->vbo;
					ebo = vai->ebo;
					glBindBuffer(GL_ARRAY_BUFFER, vbo);
					if (ebo)
						glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
					vertexCount = vai->numVerts;
					maxIndex = vai->maxIndex;
					prim = static_cast<GEPrimitiveType>(vai->prim);
					break;
				}

			case VertexArrayInfo::VAI_UNRELIABLE:
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
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

			prim = indexGen.Prim();
		}
		
		VERBOSE_LOG(G3D, "Flush prim %i! %i verts in one go", prim, vertexCount);

		SetupDecFmtForDraw(program, dec_->GetDecVtxFmt(), vbo ? 0 : decoded);
		if (useElements) {
#if 1  // USING_GLES2
			glDrawElements(glprim[prim], vertexCount, GL_UNSIGNED_SHORT, ebo ? 0 : (GLvoid*)decIndex);
#else
			glDrawRangeElements(glprim[prim], 0, maxIndex, vertexCount, GL_UNSIGNED_SHORT, ebo ? 0 : (GLvoid*)decIndex);
#endif
			if (ebo)
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		} else {
			glDrawArrays(glprim[prim], 0, vertexCount);
		}
		if (vbo)
			glBindBuffer(GL_ARRAY_BUFFER, 0);
	} else {
		DecodeVerts();
		gpuStats.numUncachedVertsDrawn += indexGen.VertexCount();
		prim = indexGen.Prim();
		// Undo the strip optimization, not supported by the SW code yet.
		if (prim == GE_PRIM_TRIANGLE_STRIP)
			prim = GE_PRIM_TRIANGLES;
		VERBOSE_LOG(G3D, "Flush prim %i SW! %i verts in one go", prim, indexGen.VertexCount());

		SoftwareTransformAndDraw(
			prim, decoded, program, indexGen.VertexCount(), 
			dec_->VertexType(), (void *)decIndex, GE_VTYPE_IDX_16BIT, dec_->GetDecVtxFmt(),
			indexGen.MaxIndex());
	}

	indexGen.Reset();
	collectedVerts = 0;
	numDrawCalls = 0;
	vertexCountInDrawCalls = 0;
	decodeCounter_ = 0;
	prevPrim_ = GE_PRIM_INVALID;

#ifndef USING_GLES2
	host->GPUNotifyDraw();
#endif
}

struct Plane {
	float x, y, z, w;
	void Set(float _x, float _y, float _z, float _w) { x = _x; y = _y; z = _z; w = _w; }
	float Test(float f[3]) const { return x * f[0] + y * f[1] + z * f[2] + w; }
};

void PlanesFromMatrix(float mtx[16], Plane planes[6]) {
	planes[0].Set(mtx[3]-mtx[0], mtx[7]-mtx[4], mtx[11]-mtx[8], mtx[15]-mtx[12]);  // Right
	planes[1].Set(mtx[3]+mtx[0], mtx[7]+mtx[4], mtx[11]+mtx[8], mtx[15]+mtx[12]);  // Left
	planes[2].Set(mtx[3]+mtx[1], mtx[7]+mtx[5], mtx[11]+mtx[9], mtx[15]+mtx[13]);  // Bottom
	planes[3].Set(mtx[3]-mtx[1], mtx[7]-mtx[5], mtx[11]-mtx[9], mtx[15]-mtx[13]);  // Top
	planes[4].Set(mtx[3]+mtx[2], mtx[7]+mtx[6], mtx[11]+mtx[10], mtx[15]+mtx[14]); // Near
	planes[5].Set(mtx[3]-mtx[2], mtx[7]-mtx[6], mtx[11]-mtx[10], mtx[15]-mtx[14]); // Far
}

static void ConvertMatrix4x3To4x4(float *m4x4, const float *m4x3) {
	m4x4[0] = m4x3[0];
	m4x4[1] = m4x3[1];
	m4x4[2] = m4x3[2];
	m4x4[3] = 0.0f;
	m4x4[4] = m4x3[3];
	m4x4[5] = m4x3[4];
	m4x4[6] = m4x3[5];
	m4x4[7] = 0.0f;
	m4x4[8] = m4x3[6];
	m4x4[9] = m4x3[7];
	m4x4[10] = m4x3[8];
	m4x4[11] = 0.0f;
	m4x4[12] = m4x3[9];
	m4x4[13] = m4x3[10];
	m4x4[14] = m4x3[11];
	m4x4[15] = 1.0f;
}

// This code is HIGHLY unoptimized!
//
// It does the simplest and safest test possible: If all points of a bbox is outside a single of
// our clipping planes, we reject the box.
bool TransformDrawEngine::TestBoundingBox(void* control_points, int vertexCount, u32 vertType) {
	SimpleVertex *corners = (SimpleVertex *)(decoded + 65536 * 12);
	float *verts = (float *)(decoded + 65536 * 18);

	// Try to skip NormalizeVertices if it's pure positions. No need to bother with a vertex decoder
	// and a large vertex format.
	if ((vertType & 0xFFFFFF) == GE_VTYPE_POS_FLOAT) {
		// memcpy(verts, control_points, 12 * vertexCount);
		verts = (float *)control_points;
	} else if ((vertType & 0xFFFFFF) == GE_VTYPE_POS_8BIT) {
		const s8 *vtx = (const s8 *)control_points;
		for (int i = 0; i < vertexCount * 3; i++) {
			verts[i] = vtx[i] * (1.0f / 128.0f);
		}
	} else if ((vertType & 0xFFFFFF) == GE_VTYPE_POS_16BIT) {
		const s16 *vtx = (const s16*)control_points;
		for (int i = 0; i < vertexCount * 3; i++) {
			verts[i] = vtx[i] * (1.0f / 32768.0f);
		}
	} else {
		// Simplify away bones and morph before proceeding
		u8 *temp_buffer = decoded + 65536 * 24;
		NormalizeVertices((u8 *)corners, temp_buffer, (u8 *)control_points, 0, vertexCount, vertType);
		// Special case for float positions only.
		const float *ctrl = (const float *)control_points;
		for (int i = 0; i < vertexCount; i++) {
			verts[i * 3] = corners[i].pos.x;
			verts[i * 3 + 1] = corners[i].pos.y;
			verts[i * 3 + 2] = corners[i].pos.z;
		}
	}

	Plane planes[6];

	float world[16];
	float view[16];
	float worldview[16];
	float worldviewproj[16];
	ConvertMatrix4x3To4x4(world, gstate.worldMatrix);
	ConvertMatrix4x3To4x4(view, gstate.viewMatrix);
	Matrix4ByMatrix4(worldview, world, view);
	Matrix4ByMatrix4(worldviewproj, worldview, gstate.projMatrix);
	PlanesFromMatrix(worldviewproj, planes);
	for (int plane = 0; plane < 6; plane++) {
		int inside = 0;
		int out = 0;
		for (int i = 0; i < vertexCount; i++) {
			// Here we can test against the frustum planes!
			float value = planes[plane].Test(verts + i * 3);
			if (value < 0)
				out++;
			else
				inside++;
		}

		if (inside == 0) {
			// All out
			return false;
		}

		// Any out. For testing that the planes are in the right locations.
		// if (out != 0) return false;
	}

	return true;
}

// TODO: Probably move this to common code (with normalization?)

static inline Vec3f ClipToScreen(const Vec4f& coords)
{
	// TODO: Check for invalid parameters (x2 < x1, etc)
	float vpx1 = getFloat24(gstate.viewportx1);
	float vpx2 = getFloat24(gstate.viewportx2);
	float vpy1 = getFloat24(gstate.viewporty1);
	float vpy2 = getFloat24(gstate.viewporty2);
	float vpz1 = getFloat24(gstate.viewportz1);
	float vpz2 = getFloat24(gstate.viewportz2);

	float retx = coords.x * vpx1 / coords.w + vpx2;
	float rety = coords.y * vpy1 / coords.w + vpy2;
	float retz = coords.z * vpz1 / coords.w + vpz2;

	// 16 = 0xFFFF / 4095.9375
	return Vec3f(retx * 16, rety * 16, retz);
}

static Vec3f ScreenToDrawing(const Vec3f& coords)
{
	Vec3f ret;
	ret.x = coords.x - gstate.getOffsetX16();
	ret.y = coords.y - gstate.getOffsetY16();

	// Convert from 16 point to float.
	ret.x *= 1.0 / 16.0;
	ret.y *= 1.0 / 16.0;
	ret.z = coords.z;
	return ret;
}

// TODO: This probably is not the best interface.
bool TransformDrawEngine::GetCurrentSimpleVertices(int count, std::vector<GPUDebugVertex> &vertices, std::vector<u16> &indices) {
	// This is always for the current vertices.
	u16 indexLowerBound = 0;
	u16 indexUpperBound = count - 1;

	if ((gstate.vertType & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
		const u8 *inds = Memory::GetPointer(gstate_c.indexAddr);
		const u16 *inds16 = (const u16 *)inds;

		if (inds) {
			GetIndexBounds(inds, count, gstate.vertType, &indexLowerBound, &indexUpperBound);
			indices.resize(count);
			switch (gstate.vertType & GE_VTYPE_IDX_MASK) {
			case GE_VTYPE_IDX_16BIT:
				for (int i = 0; i < count; ++i) {
					indices[i] = inds16[i];
				}
				break;
			case GE_VTYPE_IDX_8BIT:
				for (int i = 0; i < count; ++i) {
					indices[i] = inds[i];
				}
				break;
			default:
				return false;
			}
		} else {
			indices.clear();
		}
	} else {
		indices.clear();
	}

	static std::vector<u32> temp_buffer;
	static std::vector<SimpleVertex> simpleVertices;
	temp_buffer.resize(65536 * 24 / sizeof(u32));
	simpleVertices.resize(indexUpperBound + 1);
	NormalizeVertices((u8 *)(&simpleVertices[0]), (u8 *)(&temp_buffer[0]), Memory::GetPointer(gstate_c.vertexAddr), indexLowerBound, indexUpperBound, gstate.vertType);

	float world[16];
	float view[16];
	float worldview[16];
	float worldviewproj[16];
	ConvertMatrix4x3To4x4(world, gstate.worldMatrix);
	ConvertMatrix4x3To4x4(view, gstate.viewMatrix);
	Matrix4ByMatrix4(worldview, world, view);
	Matrix4ByMatrix4(worldviewproj, worldview, gstate.projMatrix);

	vertices.resize(indexUpperBound + 1);
	for (int i = indexLowerBound; i <= indexUpperBound; ++i) {
		const SimpleVertex &vert = simpleVertices[i];

		if (gstate.isModeThrough()) {
			vertices[i].u = vert.uv[0];
			vertices[i].v = vert.uv[1];
			vertices[i].x = vert.pos.x;
			vertices[i].y = vert.pos.y;
			vertices[i].z = vert.pos.z;
		} else {
			float clipPos[4];
			Vec3ByMatrix44(clipPos, vert.pos.AsArray(), worldviewproj);
			Vec3f screenPos = ClipToScreen(clipPos);
			Vec3f drawPos = ScreenToDrawing(screenPos);

			vertices[i].u = vert.uv[0];
			vertices[i].v = vert.uv[1];
			vertices[i].x = drawPos.x;
			vertices[i].y = drawPos.y;
			vertices[i].z = 1.0;
		}
	}

	return true;
}
