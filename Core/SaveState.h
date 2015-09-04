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

#include <string>

#include "ChunkFile.h"

namespace SaveState
{
	typedef void (*Callback)(bool status, void *cbUserData);

	// TODO: Better place for this?
	const int REVISION = 4;
	const int SAVESTATESLOTS = 5;

	void Init();

	void SaveSlot(int slot, Callback callback, void *cbUserData = 0);
	void LoadSlot(int slot, Callback callback, void *cbUserData = 0);
	// Checks whether there's an existing save in the specified slot.
	bool HasSaveInSlot(int slot);
	bool HasScreenshotInSlot(int slot);
	// Returns -1 if there's no newest slot.
	int GetNewestSlot();

	std::string GenerateSaveSlotFilename(int slot, const char *extension);

	// Load the specified file into the current state (async.)
	// Warning: callback will be called on a different thread.
	void Load(const std::string &filename, Callback callback = 0, void *cbUserData = 0);

	// Save the current state to the specified file (async.)
	// Warning: callback will be called on a different thread.
	void Save(const std::string &filename, Callback callback = 0, void *cbUserData = 0);

	CChunkFileReader::Error SaveToRam(std::vector<u8> &state);
	CChunkFileReader::Error LoadFromRam(std::vector<u8> &state);

	// For testing / automated tests.  Runs a save state verification pass (async.)
	// Warning: callback will be called on a different thread.
	void Verify(Callback callback = 0, void *cbUserData = 0);

	// To go back to a previous snapshot (only if enabled.)
	// Warning: callback will be called on a different thread.
	void Rewind(Callback callback = 0, void *cbUserData = 0);

	// Returns true if there are rewind snapshots available.
	bool CanRewind();

	// Check if there's any save stating needing to be done.  Normally called once per frame.
	void Process();
};
