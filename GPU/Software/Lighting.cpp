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

#include "../GPUState.h"

#include "Lighting.h"

namespace Lighting {

void Process(VertexData& vertex)
{
	Vec3<int> mec = Vec3<int>(gstate.getMaterialEmissiveR(), gstate.getMaterialEmissiveG(), gstate.getMaterialEmissiveB());

	Vec3<int> mac = (gstate.materialupdate&1)
						? vertex.color0.rgb()
						: Vec3<int>(gstate.getMaterialAmbientR(), gstate.getMaterialAmbientG(), gstate.getMaterialAmbientB());
	Vec3<int> final_color = mec + mac * Vec3<int>(gstate.getAmbientR(), gstate.getAmbientG(), gstate.getAmbientB()) / 255;
	Vec3<int> specular_color(0, 0, 0);

	for (unsigned int light = 0; light < 4; ++light) {
		// Always calculate texture coords from lighting results if environment mapping is active
		// TODO: specular lighting should affect this, too!
		// TODO: Not sure if this really should be done even if lighting is disabled altogether
		if (gstate.getUVGenMode() == GE_TEXMAP_ENVIRONMENT_MAP) {
			Vec3<float> L = Vec3<float>(getFloat24(gstate.lpos[3*light]&0xFFFFFF), getFloat24(gstate.lpos[3*light+1]&0xFFFFFF),getFloat24(gstate.lpos[3*light+2]&0xFFFFFF));
			float diffuse_factor = Dot(L,vertex.worldnormal) / L.Length() / vertex.worldnormal.Length();

			if (gstate.getUVLS0() == (int)light)
				vertex.texturecoords.s() = (diffuse_factor + 1.f) / 2.f;

			if (gstate.getUVLS1() == (int)light)
				vertex.texturecoords.t() = (diffuse_factor + 1.f) / 2.f;
		}
	}

	if (!gstate.isLightingEnabled())
		return;

	for (unsigned int light = 0; light < 4; ++light) {
		if (!gstate.isLightChanEnabled(light))
			continue;

		// L =  vector from vertex to light source
		// TODO: Should transfer the light positions to world/view space for these calculations
		Vec3<float> L = Vec3<float>(getFloat24(gstate.lpos[3*light]&0xFFFFFF), getFloat24(gstate.lpos[3*light+1]&0xFFFFFF),getFloat24(gstate.lpos[3*light+2]&0xFFFFFF));
		L -= vertex.worldpos;
		float d = L.Length();

		float lka = getFloat24(gstate.latt[3*light]&0xFFFFFF);
		float lkb = getFloat24(gstate.latt[3*light+1]&0xFFFFFF);
		float lkc = getFloat24(gstate.latt[3*light+2]&0xFFFFFF);
		float att = 1.f;
		if (!gstate.isDirectionalLight(light)) {
			att = 1.f / (lka + lkb * d + lkc * d * d);
			if (att > 1.f) att = 1.f;
			if (att < 0.f) att = 0.f;
		}

		float spot = 1.f;
		if (gstate.isSpotLight(light)) {
			Vec3<float> dir = Vec3<float>(getFloat24(gstate.ldir[3*light]&0xFFFFFF), getFloat24(gstate.ldir[3*light+1]&0xFFFFFF),getFloat24(gstate.ldir[3*light+2]&0xFFFFFF));
			float _spot = Dot(-L,dir) / d / dir.Length();
			float cutoff = getFloat24(gstate.lcutoff[light]&0xFFFFFF);
			if (_spot > cutoff) {
				spot = _spot;
				float conv = getFloat24(gstate.lconv[light]&0xFFFFFF);
				spot = pow(_spot, conv);
			} else {
				spot = 0.f;
			}
		}

		// ambient lighting
		Vec3<int> lac = Vec3<int>(gstate.getLightAmbientColorR(light), gstate.getLightAmbientColorG(light), gstate.getLightAmbientColorB(light));
		final_color.r() += (int)(att * spot * lac.r() * mac.r() / 255);
		final_color.g() += (int)(att * spot * lac.g() * mac.g() / 255);
		final_color.b() += (int)(att * spot * lac.b() * mac.b() / 255);

		// diffuse lighting
		Vec3<int> ldc = Vec3<int>(gstate.getDiffuseColorR(light), gstate.getDiffuseColorG(light), gstate.getDiffuseColorB(light));
		Vec3<int> mdc = (gstate.materialupdate&2)
							? vertex.color0.rgb()
							: Vec3<int>(gstate.getMaterialDiffuseR(), gstate.getMaterialDiffuseG(), gstate.getMaterialDiffuseB());

		float diffuse_factor = Dot(L,vertex.worldnormal) / d / vertex.worldnormal.Length();
		if (gstate.isUsingPoweredDiffuseLight(light)) {
			float k = getFloat24(gstate.materialspecularcoef&0xFFFFFF);
			diffuse_factor = pow(diffuse_factor, k);
		}

		if (diffuse_factor > 0.f) {
			final_color.r() += (int)(att * spot * ldc.r() * mdc.r() * diffuse_factor / 255);
			final_color.g() += (int)(att * spot * ldc.g() * mdc.g() * diffuse_factor / 255);
			final_color.b() += (int)(att * spot * ldc.b() * mdc.b() * diffuse_factor / 255);
		}

		if (gstate.isUsingSpecularLight(light)) {
			Vec3<float> E(0.f, 0.f, 1.f);
			Mat3x3<float> view_matrix(gstate.viewMatrix);
			Vec3<float> worldE = view_matrix.Inverse() * (E - Vec3<float>(gstate.viewMatrix[9], gstate.viewMatrix[10], gstate.viewMatrix[11]));
			Vec3<float> H = worldE / worldE.Length() + L / L.Length();

			Vec3<int> lsc = Vec3<int>(gstate.getSpecularColorR(light), gstate.getSpecularColorG(light), gstate.getSpecularColorB(light));
			Vec3<int> msc = (gstate.materialupdate&4)
								? vertex.color0.rgb()
								: Vec3<int>(gstate.getMaterialSpecularR(), gstate.getMaterialSpecularG(), gstate.getMaterialSpecularB());

			float specular_factor = Dot(H,vertex.worldnormal) / H.Length() / vertex.worldnormal.Length();
			float k = getFloat24(gstate.materialspecularcoef&0xFFFFFF);
			specular_factor = pow(specular_factor, k);

			if (specular_factor > 0.f) {
				specular_color.r() += (int)(att * spot * lsc.r() * msc.r() * specular_factor / 255);
				specular_color.g() += (int)(att * spot * lsc.g() * msc.g() * specular_factor / 255);
				specular_color.b() += (int)(att * spot * lsc.b() * msc.b() * specular_factor / 255);
			}
		}
	}

	vertex.color0.r() = final_color.r();
	vertex.color0.g() = final_color.g();
	vertex.color0.b() = final_color.b();

	if (gstate.isUsingSecondaryColor()) {
		vertex.color1 = specular_color.Clamp(0, 255);
	} else {
		vertex.color0.r() += specular_color.r();
		vertex.color0.g() += specular_color.g();
		vertex.color0.b() += specular_color.b();
		vertex.color1 = Vec3<int>(0, 0, 0);
	}

	int maa = (gstate.materialupdate&1) ? vertex.color0.a() : gstate.getMaterialAmbientA();
	vertex.color0.a() = gstate.getAmbientA() * maa / 255;

	vertex.color0 = vertex.color0.Clamp(0, 255);
}

} // namespace
