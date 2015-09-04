#ifndef CTRLDISASMVIEW_H
#define CTRLDISASMVIEW_H

#include <QWidget>
#include "Core/Debugger/DebugInterface.h"

class Debugger_Disasm;

class CtrlDisAsmView : public QWidget
{
	Q_OBJECT
public:
	explicit CtrlDisAsmView(QWidget *parent = 0);

	void redraw();
	void setAlign(int l)
	{
		align=l;
	}

	void setParentWindow(Debugger_Disasm* win)
	{
		parentWindow = win;
	}

	void setDebugger(DebugInterface *deb)
	{
		debugger=deb;
		curAddress=debugger->getPC();
		align=debugger->getInstructionSize(0);
	}
	DebugInterface *getDebugger()
	{
		return debugger;
	}
	void gotoAddr(unsigned int addr)
	{
		curAddress=addr&(~(align-1));
		redraw();
	}
	void gotoPC()
	{
		curAddress=debugger->getPC()&(~(align-1));
		redraw();
	}
	unsigned int getSelection()
	{
		return curAddress;
	}

	void setShowMode(bool s)
	{
		showHex=s;
	}

	void toggleBreakpoint()
	{
		debugger->toggleBreakpoint(curAddress);
		redraw();
	}

	void contextMenu(const QPoint& pos);
protected:
	void paintEvent(QPaintEvent *);
	void mousePressEvent(QMouseEvent *e);
	void keyPressEvent(QKeyEvent *);
	void wheelEvent(QWheelEvent *e);
	
public slots:
	void CopyAddress();
	void CopyInstrDisAsm();
	void SetNextStatement();
	void FollowBranch();
	void CopyInstrHex();
	void RunToHere();
	void ToggleBreakpoint();
	void GoToMemoryView();
	void RenameFunction();

private:
	int yToAddress(int y);

	int curAddress;
	int align;

	int rowHeight;
	int selection;
	int marker;
	int oldSelection;
	bool selectionChanged;
	bool selecting;
	bool hasFocus;
	bool showHex;

	DebugInterface *debugger;
	Debugger_Disasm* parentWindow;
	
};

#endif // CTRLDISASMVIEW_H
