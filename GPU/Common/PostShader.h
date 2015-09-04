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


// Postprocessing shader manager
// For FXAA, "Natural", bloom, B&W, cross processing and whatnot.

#include <string>
#include <vector>

#include "file/ini_file.h"

struct ShaderInfo {
	std::string iniFile;  // which ini file was this definition in? So we can write settings back later
	std::string section;  // ini file section. This is saved.
	std::string name;     // Fancy display name. TODO: Not using yet.

	std::string fragmentShaderFile;
	std::string vertexShaderFile;

	// Run at output instead of input resolution
	bool outputResolution;

	// TODO: Add support for all kinds of fun options like mapping the depth buffer,
	// SRGB texture reads, multiple shaders chained, etc.

	bool operator == (const std::string &other) {
		return name == other;
	}
};

const ShaderInfo *GetPostShaderInfo(std::string name);

const std::vector<ShaderInfo> &GetAllPostShaderInfo();