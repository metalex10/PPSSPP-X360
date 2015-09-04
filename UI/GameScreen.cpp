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

#include "base/colorutil.h"
#include "base/timeutil.h"
#include "gfx_es2/draw_buffer.h"
#include "i18n/i18n.h"
#include "math/curves.h"
#include "ui/ui_context.h"
#include "ui/view.h"
#include "ui/viewgroup.h"
#include "UI/EmuScreen.h"
#include "UI/GameScreen.h"
#include "UI/GameSettingsScreen.h"
#include "UI/GameInfoCache.h"
#include "UI/MiscScreens.h"
#include "UI/MainScreen.h"
#include "Core/Host.h"
#include "Core/Config.h"

void GameScreen::CreateViews() {
	GameInfo *info = g_gameInfoCache.GetInfo(gamePath_, true);

	I18NCategory *d = GetI18NCategory("Dialog");
	I18NCategory *ga = GetI18NCategory("Game");

	// Information in the top left.
	// Back button to the bottom left.
	// Scrolling action menu to the right.
	using namespace UI;

	Margins actionMenuMargins(0, 100, 15, 0);

	root_ = new LinearLayout(ORIENT_HORIZONTAL);

	ViewGroup *leftColumn = new AnchorLayout(new LinearLayoutParams(1.0f));
	root_->Add(leftColumn);

	leftColumn->Add(new Choice(d->T("Back"), "", false, new AnchorLayoutParams(150, WRAP_CONTENT, 10, NONE, NONE, 10)))->OnClick.Handle(this, &GameScreen::OnSwitchBack);
	if (info) {
		texvGameIcon_ = leftColumn->Add(new TextureView(0, IS_DEFAULT, new AnchorLayoutParams(144 * 2, 80 * 2, 10, 10, NONE, NONE)));
		tvTitle_ = leftColumn->Add(new TextView(info->title, ALIGN_LEFT, 1.0f, new AnchorLayoutParams(10, 200, NONE, NONE)));
		tvGameSize_ = leftColumn->Add(new TextView("...", ALIGN_LEFT, 1.0f, new AnchorLayoutParams(10, 250, NONE, NONE)));
		tvSaveDataSize_ = leftColumn->Add(new TextView("...", ALIGN_LEFT, 1.0f, new AnchorLayoutParams(10, 290, NONE, NONE)));
		tvInstallDataSize_ = leftColumn->Add(new TextView("", ALIGN_LEFT, 1.0f, new AnchorLayoutParams(10, 330, NONE, NONE)));
	}

	ViewGroup *rightColumn = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(300, FILL_PARENT, actionMenuMargins));
	root_->Add(rightColumn);
	
	LinearLayout *rightColumnItems = new LinearLayout(ORIENT_VERTICAL);
	rightColumnItems->SetSpacing(0.0f);
	rightColumn->Add(rightColumnItems);
	Choice *play = new Choice(ga->T("Play"));
	rightColumnItems->Add(play)->OnClick.Handle(this, &GameScreen::OnPlay);
	rightColumnItems->Add(new Choice(ga->T("Game Settings")))->OnClick.Handle(this, &GameScreen::OnGameSettings);
	rightColumnItems->Add(new Choice(ga->T("Delete Save Data")))->OnClick.Handle(this, &GameScreen::OnDeleteSaveData); 
	rightColumnItems->Add(new Choice(ga->T("Delete Game")))->OnClick.Handle(this, &GameScreen::OnDeleteGame);
	if (host->CanCreateShortcut()) {
		rightColumnItems->Add(new Choice(ga->T("Create Shortcut")))->OnClick.Handle(this, &GameScreen::OnCreateShortcut);
	}
	if (isRecentGame(gamePath_)) {
		rightColumnItems->Add(new Choice(ga->T("Remove From Recent")))->OnClick.Handle(this, &GameScreen::OnRemoveFromRecent);
	}

	UI::SetFocusedView(play);
}

void DrawBackground(float alpha);

void GameScreen::DrawBackground(UIContext &dc) {
	GameInfo *ginfo = g_gameInfoCache.GetInfo(gamePath_, true);
	dc.Flush();

	dc.RebindTexture();
	::DrawBackground(1.0f);
	dc.Flush();

	if (ginfo && ginfo->pic1Texture) {
		ginfo->pic1Texture->Bind(0);
		uint32_t color = whiteAlpha(ease((time_now_d() - ginfo->timePic1WasLoaded) * 3)) & 0xFFc0c0c0;
		dc.Draw()->DrawTexRect(0,0,dp_xres, dp_yres, 0,0,1,1,color);
		dc.Flush();
		dc.RebindTexture();
	}
	/*
	if (ginfo && ginfo->pic0Texture) {
		ginfo->pic0Texture->Bind(0);
		// Pic0 is drawn in the bottom right corner, overlaying pic1.
		float sizeX = dp_xres / 480 * ginfo->pic0Texture->Width();
		float sizeY = dp_yres / 272 * ginfo->pic0Texture->Height();
		uint32_t color = whiteAlpha(ease((time_now_d() - ginfo->timePic1WasLoaded) * 2)) & 0xFFc0c0c0;
		ui_draw2d.DrawTexRect(dp_xres - sizeX, dp_yres - sizeY, dp_xres, dp_yres, 0,0,1,1,color);
		ui_draw2d.Flush();
		dc.RebindTexture();
	}*/
}

void GameScreen::update(InputState &input) {
	UIScreen::update(input);

	I18NCategory *ga = GetI18NCategory("Game");
	GameInfo *info = g_gameInfoCache.GetInfo(gamePath_, true);

	if (tvTitle_)
		tvTitle_->SetText(info->title + " (" + info->id + ")");
	if (info->iconTexture && texvGameIcon_)	{
		texvGameIcon_->SetTexture(info->iconTexture);
		uint32_t color = whiteAlpha(ease((time_now_d() - info->timeIconWasLoaded) * 3));
		texvGameIcon_->SetColor(color);
	}

	if (info->gameSize) {
		char temp[256];
		sprintf(temp, "%s: %1.1f %s", ga->T("Game"), (float) (info->gameSize) / 1024.f / 1024.f, ga->T("MB"));
		tvGameSize_->SetText(temp);
		sprintf(temp, "%s: %1.2f %s", ga->T("SaveData"), (float) (info->saveDataSize) / 1024.f / 1024.f, ga->T("MB"));
		tvSaveDataSize_->SetText(temp);
		if (info->installDataSize > 0) {
			sprintf(temp, "%s: %1.2f %s", ga->T("InstallData"), (float) (info->installDataSize) / 1024.f / 1024.f, ga->T("MB"));
			tvInstallDataSize_->SetText(temp);
		}
	}
}

UI::EventReturn GameScreen::OnSwitchBack(UI::EventParams &e) {
	screenManager()->finishDialog(this, DR_OK);
	return UI::EVENT_DONE;
}

UI::EventReturn GameScreen::OnPlay(UI::EventParams &e) {
	screenManager()->switchScreen(new EmuScreen(gamePath_));
	return UI::EVENT_DONE;
}

UI::EventReturn GameScreen::OnGameSettings(UI::EventParams &e) {
	GameInfo *info = g_gameInfoCache.GetInfo(gamePath_, true);
	if (info && info->paramSFOLoaded) {
		std::string discID = info->paramSFO.GetValueString("DISC_ID");
		screenManager()->push(new GameSettingsScreen(gamePath_, discID));
	}
	return UI::EVENT_DONE;
}

UI::EventReturn GameScreen::OnDeleteSaveData(UI::EventParams &e) {
	I18NCategory *d = GetI18NCategory("Dialog");
	I18NCategory *ga = GetI18NCategory("Game");
	GameInfo *info = g_gameInfoCache.GetInfo(gamePath_, true);
	if (info) {
		screenManager()->push(
			new PromptScreen(d->T("DeleteConfirmAll", "Do you really want to delete all\nyour save data for this game?"), ga->T("ConfirmDelete"), d->T("Cancel"),
			std::bind(&GameScreen::CallbackDeleteSaveData, this, placeholder::_1)));
	}

	RecreateViews();
	return UI::EVENT_DONE;
}

void GameScreen::CallbackDeleteSaveData(bool yes) {
	GameInfo *info = g_gameInfoCache.GetInfo(gamePath_, false);
	if (yes) {
		info->DeleteAllSaveData();
		info->saveDataSize = 0;
		info->installDataSize = 0;
	}
}

UI::EventReturn GameScreen::OnDeleteGame(UI::EventParams &e) {
	I18NCategory *d = GetI18NCategory("Dialog");
	I18NCategory *ga = GetI18NCategory("Game");
	GameInfo *info = g_gameInfoCache.GetInfo(gamePath_, true);
	if (info) {
		screenManager()->push(
			new PromptScreen(d->T("DeleteConfirmGame", "Do you really want to delete this game\nfrom your device? You can't undo this."), ga->T("ConfirmDelete"), d->T("Cancel"),
			std::bind(&GameScreen::CallbackDeleteGame, this, placeholder::_1)));
	}

	return UI::EVENT_DONE;
}

void GameScreen::CallbackDeleteGame(bool yes) {
	GameInfo *info = g_gameInfoCache.GetInfo(gamePath_, false);
	if (yes) {
		info->DeleteGame();
		g_gameInfoCache.Clear();
		screenManager()->switchScreen(new MainScreen());
	}
}

UI::EventReturn GameScreen::OnCreateShortcut(UI::EventParams &e) {
	GameInfo *info = g_gameInfoCache.GetInfo(gamePath_, false);
	if (info) {
		host->CreateDesktopShortcut(gamePath_, info->title);
	}
	return UI::EVENT_DONE;
}

bool GameScreen::isRecentGame(std::string gamePath) {
	for (auto it = g_Config.recentIsos.begin(); it != g_Config.recentIsos.end(); ++it) {
#ifdef _WIN32
		if (!strcmpIgnore((*it).c_str(), gamePath.c_str(), "\\","/"))
#else
		if (!strcmp((*it).c_str(), gamePath.c_str()))
#endif
			return true;
	}
	return false;
}

UI::EventReturn GameScreen::OnRemoveFromRecent(UI::EventParams &e) {
	for (auto it = g_Config.recentIsos.begin(); it != g_Config.recentIsos.end(); ++it) {
#ifdef _WIN32
		if (!strcmpIgnore((*it).c_str(), gamePath_.c_str(), "\\","/")) {
#else
		if (!strcmp((*it).c_str(), gamePath_.c_str())) {
#endif
			g_Config.recentIsos.erase(it);
			screenManager()->switchScreen(new MainScreen());
			return UI::EVENT_DONE;
		}
	}
	return UI::EVENT_DONE;
}
