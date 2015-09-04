#pragma once

#include <vector>

class Dialog;

class TabControl
{
public:
	TabControl(HWND handle, bool noDisplayArea = false);
	void HandleNotify(LPARAM lParam);
	HWND AddTabWindow(wchar_t* className, wchar_t* title, DWORD style = 0);
	void AddTabDialog(Dialog* dialog, wchar_t* title);
	void AddTab(HWND hwnd, wchar_t* title);
	void ShowTab(int index, bool setControlIndex = true);
	void ShowTab(HWND pageHandle);
	void NextTab(bool cycle);
	void PreviousTab(bool cycle);
	int CurrentTabIndex() { return currentTab; }
	HWND CurrentTabHandle() {
		if (currentTab < 0 || currentTab >= (int)tabs.size()) {
			return NULL;
		}
		return tabs[currentTab].pageHandle;
	}
	void SetShowTabTitles(bool enabled);
	void SetIgnoreBottomMargin(bool enabled) { ignoreBottomMargin = enabled; }
	bool GetShowTabTitles() { return showTabTitles; }
	void SetMinTabWidth(int w);

private:
	static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	void OnResize();
	int AppendPageToControl(wchar_t* title);

	struct TabInfo
	{
		bool hasBorder;
		bool hasClientEdge;
		HWND lastFocus;
		HWND pageHandle;
		wchar_t title[128];
	};

	HWND hwnd;
	WNDPROC oldProc;
	std::vector<TabInfo> tabs;
	bool showTabTitles;
	bool ignoreBottomMargin;
	int currentTab;
	bool hasButtons;
	bool noDisplayArea_;
};