#pragma once
#include "Common/CommonWindows.h"
#include "Common/CommonTypes.h"
#include "Core/Debugger/DebugInterface.h"

class DumpMemoryWindow
{
	enum Mode { MODE_RAM, MODE_VRAM, MODE_SCRATCHPAD, MODE_CUSTOM };

	HWND parentHwnd;
	DebugInterface* cpu;
	bool filenameChosen;
	Mode selectedMode;

	u32 start;
	u32 size;
	char fileName[MAX_PATH];

	static DumpMemoryWindow* bp;
	void changeMode(HWND hwnd, Mode newMode);
	bool fetchDialogData(HWND hwnd);
public:
	DumpMemoryWindow(HWND parent, DebugInterface* cpu): cpu(cpu)
	{
		parentHwnd = parent;
		filenameChosen = false;
		selectedMode = MODE_RAM;
	};

	static INT_PTR CALLBACK dlgFunc(HWND hWnd, UINT iMsg, WPARAM wParam, LPARAM lParam);
	bool exec();
};
