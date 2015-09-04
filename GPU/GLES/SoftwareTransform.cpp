// Copyright (c) 2013- PPSSPP Project.

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

#include "gfx_es2/gl_state.h"

#include "Core/Config.h"
#include "GPU/GPUState.h"
#include "GPU/Math3D.h"
#include "GPU/GLES/ShaderManager.h"
#include "GPU/GLES/TransformPipeline.h"

// This is the software transform pipeline, which is necessary for supporting RECT
// primitives correctly without geometry shaders, and may be easier to use for
// debugging than the hardware transform pipeline.

// There's code here that simply expands transformed RECTANGLES into plain triangles.

// We're gonna have to keep software transforming RECTANGLES, unless we use a geom shader which we can't on OpenGL ES 2.0.
// Usually, though, these primitives don't use lighting etc so it's no biggie performance wise, but it would be nice to get rid of
// this code.

// Actually, if we find the camera-relative right and down vectors, it might even be possible to add the extra points in pre-transformed
// space and thus make decent use of hardware transform.

// Actually again, single quads could be drawn more efficiently using GL_TRIANGLE_STRIP, no need to duplicate verts as for
// GL_TRIANGLES. Still need to sw transform to compute the extra two corners though.
//

extern const GLuint glprim[8];

// Check for max first as clamping to max is more common than min when lighting.
inline float clamp(float in, float min, float max) {
	return in > max ? max : (in < min ? min : in);
}

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
	Color4 in(colorIn);

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
bool TransformDrawEngine::IsReallyAClear(int numVerts) const {
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


void TransformDrawEngine::SoftwareTransformAndDraw(
		int prim, u8 *decoded, LinkedShader *program, int vertexCount, u32 vertType, void *inds, int indexType, const DecVtxFormat &decVtxFormat, int maxIndex) {

	bool throughmode = (vertType & GE_VTYPE_THROUGH_MASK) != 0;
	bool lmode = gstate.isUsingSecondaryColor() && gstate.isLightingEnabled();

	// TODO: Split up into multiple draw calls for GLES 2.0 where you can't guarantee support for more than 0x10000 verts.

#if defined(USING_GLES2)
	if (vertexCount > 0x10000/3)
		vertexCount = 0x10000/3;
#endif

	float uscale = 1.0f;
	float vscale = 1.0f;
	bool scaleUV = false;
	if (throughmode) {
		uscale /= gstate_c.curTextureWidth;
		vscale /= gstate_c.curTextureHeight;
	} else {
		scaleUV = !g_Config.bPrescaleUV;
	}

	bool skinningEnabled = vertTypeIsSkinningEnabled(vertType);

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
				c0[0] = gstate.getMaterialAmbientR() / 255.f;
				c0[1] = gstate.getMaterialAmbientG() / 255.f;
				c0[2] = gstate.getMaterialAmbientB() / 255.f;
				c0[3] = gstate.getMaterialAmbientA() / 255.f;
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

			if (!skinningEnabled) {
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
				unlitColor[0] = gstate.getMaterialAmbientR() / 255.f;
				unlitColor[1] = gstate.getMaterialAmbientG() / 255.f;
				unlitColor[2] = gstate.getMaterialAmbientB() / 255.f;
				unlitColor[3] = gstate.getMaterialAmbientA() / 255.f;
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
					c0[0] = gstate.getMaterialAmbientR() / 255.f;
					c0[1] = gstate.getMaterialAmbientG() / 255.f;
					c0[2] = gstate.getMaterialAmbientB() / 255.f;
					c0[3] = gstate.getMaterialAmbientA() / 255.f;
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
				if (scaleUV) {
					uv[0] = ruv[0]*gstate_c.uv.uScale + gstate_c.uv.uOff;
					uv[1] = ruv[1]*gstate_c.uv.vScale + gstate_c.uv.vOff;
				} else {
					uv[0] = ruv[0];
					uv[1] = ruv[1];
				}
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

	// Here's the best opportunity to try to detect rectangles used to clear the screen, and
	// replace them with real OpenGL clears. This can provide a speedup on certain mobile chips.
	// Disabled for now - depth does not come out exactly the same.
	//
	// An alternative option is to simply ditch all the verts except the first and last to create a single
	// rectangle out of many. Quite a small optimization though.
	if (false && maxIndex > 1 && gstate.isModeClear() && prim == GE_PRIM_RECTANGLES && IsReallyAClear(maxIndex)) {
		u32 clearColor;
		memcpy(&clearColor, transformed[0].color0, 4);
		float clearDepth = transformed[0].z;
		const float col[4] = {
			((clearColor & 0xFF)) / 255.0f,
			((clearColor & 0xFF00) >> 8) / 255.0f,
			((clearColor & 0xFF0000) >> 16) / 255.0f,
			((clearColor & 0xFF000000) >> 24) / 255.0f,
		};

		bool colorMask = gstate.isClearModeColorMask();
		bool alphaMask = gstate.isClearModeAlphaMask();
		glstate.colorMask.set(colorMask, colorMask, colorMask, alphaMask);
		if (alphaMask) {
			glstate.stencilTest.set(true);
			// Clear stencil
			// TODO: extract the stencilValue properly, see below
			int stencilValue = 0;
			glstate.stencilFunc.set(GL_ALWAYS, stencilValue, 255);
		} else {
			// Don't touch stencil
			glstate.stencilTest.set(false);
		}
		glstate.scissorTest.set(false);
		bool depthMask = gstate.isClearModeDepthMask();

		int target = 0;
		if (colorMask || alphaMask) target |= GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
		if (depthMask) target |= GL_DEPTH_BUFFER_BIT;

		glClearColor(col[0], col[1], col[2], col[3]);
#ifdef USING_GLES2
		glClearDepthf(clearDepth);
#else
		glClearDepth(clearDepth);
#endif
		glClearStencil(0);  // TODO - take from alpha?
		glClear(target);
		return;
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
		u32 stencilValue = 0;
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
			glstate.stencilFunc.set(GL_ALWAYS, stencilValue, 255);
		}
	}

	// TODO: Add a post-transform cache here for multi-RECTANGLES only.
	// Might help for text drawing.

	// these spam the gDebugger log.
	const int vertexSize = sizeof(transformed[0]);

	bool doTextureProjection = gstate.getUVGenMode() == GE_TEXMAP_TEXTURE_MATRIX;
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glVertexAttribPointer(ATTR_POSITION, 4, GL_FLOAT, GL_FALSE, vertexSize, drawBuffer);
	int attrMask = program->attrMask;
	if (attrMask & (1 << ATTR_TEXCOORD)) glVertexAttribPointer(ATTR_TEXCOORD, doTextureProjection ? 3 : 2, GL_FLOAT, GL_FALSE, vertexSize, ((uint8_t*)drawBuffer) + 4 * 4);
	if (attrMask & (1 << ATTR_COLOR0)) glVertexAttribPointer(ATTR_COLOR0, 4, GL_UNSIGNED_BYTE, GL_TRUE, vertexSize, ((uint8_t*)drawBuffer) + 7 * 4);
	if (attrMask & (1 << ATTR_COLOR1)) glVertexAttribPointer(ATTR_COLOR1, 3, GL_UNSIGNED_BYTE, GL_TRUE, vertexSize, ((uint8_t*)drawBuffer) + 8 * 4);
	if (drawIndexed) {
#if 1  // USING_GLES2
		glDrawElements(glprim[prim], numTrans, GL_UNSIGNED_SHORT, inds);
#else
		glDrawRangeElements(glprim[prim], 0, indexGen.MaxIndex(), numTrans, GL_UNSIGNED_SHORT, inds);
#endif
	} else {
		glDrawArrays(glprim[prim], 0, numTrans);
	}
}
