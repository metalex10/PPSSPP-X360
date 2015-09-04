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

#pragma once

#include "Core/Dialog/PSPDialog.h"
#include "Core/System.h"

struct SceUtilityNetconfData {
	char groupName[8];
	int timeout;
};

struct SceUtilityNetconfParam {
	pspUtilityDialogCommon common;
	int netAction;
	PSPPointer<SceUtilityNetconfData> NetconfData;
	int netHotspot;
	int netHotspotConnected;
	int netWifiSpot;   
};

class PSPNetconfDialog: public PSPDialog {
public:
	PSPNetconfDialog();
	virtual ~PSPNetconfDialog();

	virtual int Init(u32 paramAddr);
	virtual int Update(int animSpeed);
	virtual int Shutdown(bool force = false);
	virtual void DoState(PointerWrap &p);

private:
	SceUtilityNetconfParam request;
};
