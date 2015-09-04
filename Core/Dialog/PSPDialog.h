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

#include "Globals.h"
#include "Common/Common.h"
#include "Core/Config.h"

class PointerWrap;

#define SCE_UTILITY_DIALOG_RESULT_SUCCESS				0
#define SCE_UTILITY_DIALOG_RESULT_CANCEL				1
#define SCE_UTILITY_DIALOG_RESULT_ABORT					2

const int SCE_ERROR_UTILITY_INVALID_STATUS         = 0x80110001;
const int SCE_ERROR_UTILITY_INVALID_PARAM_SIZE     = 0x80110004;
const int SCE_ERROR_UTILITY_WRONG_TYPE             = 0x80110005;

struct pspUtilityDialogCommon
{
	u32_le size;            /** Size of the structure */
	s32_le language;        /** Language */
	s32_le buttonSwap;      /** Set to 1 for X/O button swap */
	s32_le graphicsThread;  /** Graphics thread priority */
	s32_le accessThread;    /** Access/fileio thread priority (SceJobThread) */
	s32_le fontThread;      /** Font thread priority (ScePafThread) */
	s32_le soundThread;     /** Sound thread priority */
	s32_le result;          /** Result */
	s32_le reserved[4];     /** Set to 0 */
};


class PSPDialog
{
public:
	PSPDialog();
	virtual ~PSPDialog();

	virtual int Update(int animSpeed) = 0;
	virtual int Shutdown(bool force = false);
	virtual void DoState(PointerWrap &p);
	virtual pspUtilityDialogCommon *GetCommonParam();

	enum DialogStatus
	{
		SCE_UTILITY_STATUS_NONE 		= 0,
		SCE_UTILITY_STATUS_INITIALIZE	= 1,
		SCE_UTILITY_STATUS_RUNNING 		= 2,
		SCE_UTILITY_STATUS_FINISHED 	= 3,
		SCE_UTILITY_STATUS_SHUTDOWN 	= 4
	};

	enum DialogStockButton
	{
		DS_BUTTON_NONE   = 0x00,
		DS_BUTTON_OK     = 0x01,
		DS_BUTTON_CANCEL = 0x02,
		DS_BUTTON_BOTH   = 0x03,
	};

	DialogStatus GetStatus();

	void StartDraw();
	void EndDraw();
protected:
	bool IsButtonPressed(int checkButton);
	void DisplayButtons(int flags);

	void StartFade(bool fadeIn_);
	void UpdateFade(int animSpeed);
	u32 CalcFadedColor(u32 inColor);

	DialogStatus status;

	unsigned int lastButtons;
	unsigned int buttons;

	float fadeTimer;
	bool isFading;
	bool fadeIn;
	u32 fadeValue;

	int okButtonImg;
	int cancelButtonImg;
	int okButtonFlag;
	int cancelButtonFlag;
};
