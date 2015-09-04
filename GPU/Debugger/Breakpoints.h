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

#pragma once

#include "Common/CommonTypes.h"

namespace GPUBreakpoints {
	void Init();

	bool IsBreakpoint(u32 pc, u32 op);

	bool IsAddressBreakpoint(u32 addr, bool &temp);
	bool IsAddressBreakpoint(u32 addr);
	bool IsCmdBreakpoint(u8 cmd, bool &temp);
	bool IsCmdBreakpoint(u8 cmd);
	bool IsTextureBreakpoint(u32 addr, bool &temp);
	bool IsTextureBreakpoint(u32 addr);

	void AddAddressBreakpoint(u32 addr, bool temp = false);
	void AddCmdBreakpoint(u8 cmd, bool temp = false);
	void AddTextureBreakpoint(u32 addr, bool temp = false);
	void AddTextureChangeTempBreakpoint();

	void RemoveAddressBreakpoint(u32 addr);
	void RemoveCmdBreakpoint(u8 cmd);
	void RemoveTextureBreakpoint(u32 addr);
	void RemoveTextureChangeTempBreakpoint();

	void UpdateLastTexture(u32 addr);

	void ClearAllBreakpoints();
	void ClearTempBreakpoints();

	static inline bool IsOpBreakpoint(u32 op, bool &temp) {
		return IsCmdBreakpoint(op >> 24, temp);
	}

	static inline bool IsOpBreakpoint(u32 op) {
		return IsCmdBreakpoint(op >> 24);
	}
};
