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

#include "PSPMsgDialog.h"
#include "../Util/PPGeDraw.h"
#include "../HLE/sceCtrl.h"
#include "../Core/MemMap.h"
#include "Core/Reporting.h"
#include "ChunkFile.h"
#include "i18n/i18n.h"
#include "util/text/utf8.h"

const float FONT_SCALE = 0.65f;

PSPMsgDialog::PSPMsgDialog()
	: PSPDialog()
	, flag(0)
{
}

PSPMsgDialog::~PSPMsgDialog() {
}

int PSPMsgDialog::Init(unsigned int paramAddr)
{
	// Ignore if already running
	if (status != SCE_UTILITY_STATUS_NONE && status != SCE_UTILITY_STATUS_SHUTDOWN)
	{
		return 0;
	}

	messageDialogAddr = paramAddr;
	if (!Memory::IsValidAddress(messageDialogAddr))
	{
		return 0;
	}
	int size = Memory::Read_U32(paramAddr);
	memset(&messageDialog,0,sizeof(messageDialog));
	// Only copy the right size to support different request format
	Memory::Memcpy(&messageDialog,paramAddr,size);

	// debug info
	int optionsNotCoded = ((messageDialog.options | SCE_UTILITY_MSGDIALOG_DEBUG_OPTION_CODED) ^ SCE_UTILITY_MSGDIALOG_DEBUG_OPTION_CODED);
	if(optionsNotCoded)
	{
		ERROR_LOG_REPORT(SCEUTILITY, "PSPMsgDialog options not coded : 0x%08x", optionsNotCoded);
	}

	flag = 0;

	// Check request invalidity
	if(messageDialog.type == 0 && !(messageDialog.errorNum & 0x80000000))
	{
		flag |= DS_ERROR;
		messageDialog.result = SCE_UTILITY_MSGDIALOG_ERROR_ERRORCODEINVALID;
	}
	else if(size == SCE_UTILITY_MSGDIALOG_SIZE_V2 && messageDialog.type == 1)
	{
		unsigned int validOp = SCE_UTILITY_MSGDIALOG_OPTION_TEXT |
				SCE_UTILITY_MSGDIALOG_OPTION_YESNO |
				SCE_UTILITY_MSGDIALOG_OPTION_DEFAULT_NO;
		if (((messageDialog.options | validOp) ^ validOp) != 0)
		{
			flag |= DS_ERROR;
			messageDialog.result = SCE_UTILITY_MSGDIALOG_ERROR_BADOPTION;
		}
	}
	else if(size == SCE_UTILITY_MSGDIALOG_SIZE_V3)
	{
		if((messageDialog.options & SCE_UTILITY_MSGDIALOG_OPTION_DEFAULT_NO) &&
				!(messageDialog.options & SCE_UTILITY_MSGDIALOG_OPTION_YESNO))
		{
			flag |= DS_ERROR;
			messageDialog.result = SCE_UTILITY_MSGDIALOG_ERROR_BADOPTION;
		}
	}

	if(flag == 0)
	{
		yesnoChoice = 1;
		if(messageDialog.type == 1)
			flag |= DS_MSG;
		if(messageDialog.type == 0)
			flag |= DS_ERRORMSG;
		if((messageDialog.options & SCE_UTILITY_MSGDIALOG_OPTION_YESNO) &&
				((size == SCE_UTILITY_MSGDIALOG_SIZE_V3) ||
						(size == SCE_UTILITY_MSGDIALOG_SIZE_V2 && messageDialog.type == 1)))
			flag |= DS_YESNO;
		if(messageDialog.options & SCE_UTILITY_MSGDIALOG_OPTION_DEFAULT_NO)
		{
			yesnoChoice = 0;
			flag |= DS_DEFNO;
		}
		if((messageDialog.options & SCE_UTILITY_MSGDIALOG_OPTION_OK) && (size == SCE_UTILITY_MSGDIALOG_SIZE_V3))
		{
			yesnoChoice = 1;
			flag |= DS_OK;
		}
		if((flag & DS_YESNO) || (flag & DS_OK))
			flag |= DS_VALIDBUTTON;
		if(!((messageDialog.options & SCE_UTILITY_MSGDIALOG_OPTION_NOCANCEL)  && (size == SCE_UTILITY_MSGDIALOG_SIZE_V3)))
			flag |= DS_CANCELBUTTON;
		if(messageDialog.options & SCE_UTILITY_MSGDIALOG_OPTION_NOSOUND)
			flag |= DS_NOSOUND;
	}

	if (flag & DS_ERRORMSG) {
		snprintf(msgText, 512, "Error code: %08x", messageDialog.errorNum);
	} else {
		strncpy(msgText, messageDialog.string, 512);
	}

	status = SCE_UTILITY_STATUS_INITIALIZE;

	lastButtons = __CtrlPeekButtons();
	StartFade(true);
	return 0;
}

void PSPMsgDialog::DisplayMessage(std::string text, bool hasYesNo, bool hasOK)
{
	float WRAP_WIDTH = 300.0f;
	if (UTF8StringNonASCIICount(text.c_str()) > 3)
		WRAP_WIDTH = 372.0f;
	
	float y = 140.0f;
	float h, sy ,ey;
	int n;
	PPGeMeasureText(0, &h, &n, text.c_str(), FONT_SCALE, PPGE_LINE_WRAP_WORD, WRAP_WIDTH);
	float h2 = h * n / 2.0f;
	ey = y + h2 + 20.0f;

	if (hasYesNo)
	{
		I18NCategory *d = GetI18NCategory("Dialog");
		const char *choiceText;
		u32 yesColor, noColor;
		float x, w;
		if (yesnoChoice == 1) {
			choiceText = d->T("Yes");
			x = 204.0f;
			yesColor = 0xFFFFFFFF;
			noColor  = 0xFFFFFFFF;
		}
		else {
			choiceText = d->T("No");
			x = 273.0f;
			yesColor = 0xFFFFFFFF;
			noColor  = 0xFFFFFFFF;
		}
		PPGeMeasureText(&w, &h, 0, choiceText, FONT_SCALE);
		w = 15.0f;
		h = 8.0f;
		float y2 = y + h2 + 8.0f;
		h2 += h + 5.0f;
		y = 135.0f - h;
		PPGeDrawRect(x - w, y2 - h, x + w, y2 + h, CalcFadedColor(0x6DCFCFCF));
		PPGeDrawText(d->T("Yes"), 204.0f, y2 + 1.0f, PPGE_ALIGN_CENTER, FONT_SCALE, CalcFadedColor(0x80000000));
		PPGeDrawText(d->T("Yes"), 203.0f, y2 - 1.0f, PPGE_ALIGN_CENTER, FONT_SCALE, CalcFadedColor(yesColor));
		PPGeDrawText(d->T("No"), 273.0f, y2 + 1.0f, PPGE_ALIGN_CENTER, FONT_SCALE, CalcFadedColor(0x80000000));
		PPGeDrawText(d->T("No"), 272.0f, y2 - 1.0f, PPGE_ALIGN_CENTER, FONT_SCALE, CalcFadedColor(noColor));
		if (IsButtonPressed(CTRL_LEFT) && yesnoChoice == 0) {
			yesnoChoice = 1;
		}
		else if (IsButtonPressed(CTRL_RIGHT) && yesnoChoice == 1) {
			yesnoChoice = 0;
		}
		ey = y2 + 25.0f;
	} 
	
	if (hasOK) {
		I18NCategory *d = GetI18NCategory("Dialog");
		float x, w;
		x = 240.0f;
		w = 15.0f;
		h = 8.0f;
		float y2 = y + h2 + 8.0f;
		h2 += h + 5.0f;
		y = 135.0f - h;
		PPGeDrawRect(x - w, y2 - h, x + w, y2 + h, CalcFadedColor(0x6DCFCFCF));
		PPGeDrawText(d->T("OK"), 240.0f, y2 + 1.0f, PPGE_ALIGN_CENTER, FONT_SCALE, CalcFadedColor(0x80000000));
		PPGeDrawText(d->T("OK"), 239.0f, y2 - 1.0f, PPGE_ALIGN_CENTER, FONT_SCALE, CalcFadedColor(0xFFFFFFFF));
		ey = y2 + 25.0f;
	}

	PPGeDrawTextWrapped(text.c_str(), 241.0f, y+2, WRAP_WIDTH, PPGE_ALIGN_CENTER, FONT_SCALE, CalcFadedColor(0x80000000));
	PPGeDrawTextWrapped(text.c_str(), 240.0f, y, WRAP_WIDTH, PPGE_ALIGN_CENTER, FONT_SCALE, CalcFadedColor(0xFFFFFFFF));
	sy = 125.0f - h2;
	PPGeDrawRect(40.0f, sy, 440.0f, sy + 1.0f, CalcFadedColor(0xFFFFFFFF));
	PPGeDrawRect(40.0f, ey, 440.0f, ey + 1.0f, CalcFadedColor(0xFFFFFFFF));
}

int PSPMsgDialog::Update(int animSpeed)
{
	if (status != SCE_UTILITY_STATUS_RUNNING)
	{
		return 0;
	}

	if ((flag & DS_ERROR))
	{
		status = SCE_UTILITY_STATUS_FINISHED;
	}
	else
	{
		UpdateFade(animSpeed);

		buttons = __CtrlPeekButtons();

		okButtonImg = I_CIRCLE;
		cancelButtonImg = I_CROSS;
		okButtonFlag = CTRL_CIRCLE;
		cancelButtonFlag = CTRL_CROSS;
		if (messageDialog.common.buttonSwap == 1)
		{
			okButtonImg = I_CROSS;
			cancelButtonImg = I_CIRCLE;
			okButtonFlag = CTRL_CROSS;
			cancelButtonFlag = CTRL_CIRCLE;
		}

		StartDraw();
		// white -> RGB(168,173,189), black -> RGB(129,134,150)
		// (255 - a) + (x * a / 255) = 173,  x * a / 255 = 134
		// a = 255 - w + b = 158, x = b * 255 / a = ?
		// but is not drawn using x * a + y * (255 - a) here?
		//PPGeDrawRect(0, 0, 480, 272, CalcFadedColor(0x9EF2D8D0));
		PPGeDrawRect(0, 0, 480, 272, CalcFadedColor(0xC0C8B2AC));

		if ((flag & DS_MSG) || (flag & DS_ERRORMSG))
			DisplayMessage(msgText, (flag & DS_YESNO) != 0, (flag & DS_OK) != 0);

		if (flag & (DS_OK | DS_VALIDBUTTON)) 
			DisplayButtons(DS_BUTTON_OK);

		if (flag & DS_CANCELBUTTON)
			DisplayButtons(DS_BUTTON_CANCEL);

		if (IsButtonPressed(cancelButtonFlag) && (flag & DS_CANCELBUTTON))
		{
			if(messageDialog.common.size == SCE_UTILITY_MSGDIALOG_SIZE_V3 ||
					((messageDialog.common.size == SCE_UTILITY_MSGDIALOG_SIZE_V2) && (flag & DS_YESNO)))
				messageDialog.buttonPressed = 3;
			else
				messageDialog.buttonPressed = 0;
			StartFade(false);
		}
		else if (IsButtonPressed(okButtonFlag) && (flag & DS_VALIDBUTTON))
		{
			if (yesnoChoice == 0)
			{
				messageDialog.buttonPressed = 2;
			}
			else
			{
				messageDialog.buttonPressed = 1;
			}
			StartFade(false);
		}


		EndDraw();

		lastButtons = buttons;
	}

	Memory::Memcpy(messageDialogAddr,&messageDialog,messageDialog.common.size);
	return 0;
}

int PSPMsgDialog::Abort()
{
	// TODO: Probably not exactly the same?
	return PSPDialog::Shutdown();
}

int PSPMsgDialog::Shutdown(bool force)
{
	return PSPDialog::Shutdown();
}

void PSPMsgDialog::DoState(PointerWrap &p)
{
	PSPDialog::DoState(p);

	auto s = p.Section("PSPMsgDialog", 1);
	if (!s)
		return;

	p.Do(flag);
	p.Do(messageDialog);
	p.Do(messageDialogAddr);
	p.DoArray(msgText, sizeof(msgText));
	p.Do(yesnoChoice);
}

pspUtilityDialogCommon *PSPMsgDialog::GetCommonParam()
{
	return &messageDialog.common;
}
