#pragma once
#include <imm.h>
#include "Client.h"

// IME candidate window follow fix (GMS083 BeiDou client).
//
// Root cause: the client never answers WM_IME_REQUEST / IMR_QUERYCHARPOSITION,
// the message modern (Win8+) IMEs use to ask the focused window where the
// caret is. With no answer the IME falls back to its default position near the
// taskbar. The client only ever sets the IMM candidate window once, on
// WM_INPUTLANGCHANGE (before any edit box has focus) via ImmSetCandidateWindow.
//
// Fix: answer the query from the subclassed main-window procedure
// (WindowScaleProc in ReplacementFuncs.h) with the focused edit control's caret
// position, so AT_CARET IMEs place their UI at the input box.
//
// Verified against BeiDou.exe (GMS083): imagebase 0x400000.

#ifndef IMR_QUERYCHARPOSITION
#define IMR_QUERYCHARPOSITION 0x000Cu
#endif

// CWndMan singleton: focused control + main window handle.
#define IME_WNDMAN_PTR         0x00BEC20Cu
#define IME_WNDMAN_FOCUS       0x88u    // m_pFocus (IUIMsgHandler*)
#define IME_WNDMAN_HWND        0xACu    // m_hWnd

// CCtrlEdit/CCtrlMLEdit: line height (m_nFontHeight).
#define IME_CTRL_FONT_HEIGHT   0x7Cu

// IUIMsgHandler vtable slots on the focused control.
#define IME_IUIMSG_GETABSLEFT  0x2Cu    // GetAbsLeft()
#define IME_IUIMSG_GETABSTOP   0x30u    // GetAbsTop()

// Vertical offset (render units) from the control top down to the text line.
#define IME_FOLLOW_LINE_OFFSET 20

// Computes the caret anchor of the focused edit control.
//   *pSx/*pSy : caret point in SCREEN pixels
//   *pLineH   : text line height in SCREEN pixels
// Returns false (and leaves outputs untouched) when there is no focused control
// or the feature is disabled.
static bool ComputeImeCaret(HWND* pHWnd, int* pSx, int* pSy, int* pLineH)
{
	if (Client::imeFollow == 0)
		return false;
	DWORD wndMan = *(DWORD*)IME_WNDMAN_PTR;
	if (wndMan == 0)
		return false;
	DWORD focus = *(DWORD*)(wndMan + IME_WNDMAN_FOCUS);
	HWND  hWnd  = *(HWND*)(wndMan + IME_WNDMAN_HWND);
	if (focus == 0 || hWnd == 0)
		return false;
	DWORD vft = *(DWORD*)focus;
	if (vft == 0)
		return false;

	// GetAbsLeft/GetAbsTop return the control position in render space;
	// map render -> client pixels -> screen. Use the *current* client size so
	// runtime window resizing stays correct (windowScale is startup-only).
	int nAbsLeft   = ((int(__thiscall*)(DWORD))*(DWORD*)(vft + IME_IUIMSG_GETABSLEFT))(focus);
	int nAbsTop    = ((int(__thiscall*)(DWORD))*(DWORD*)(vft + IME_IUIMSG_GETABSTOP))(focus);
	int nFontHeight = *(int*)(focus + IME_CTRL_FONT_HEIGHT);
	POINT ptOrg = { 0, 0 };
	ClientToScreen(hWnd, &ptOrg);
	double dScaleX = 1.0, dScaleY = 1.0;
	RECT rcClient = { 0, 0, 0, 0 };
	if (GetClientRect(hWnd, &rcClient)
		&& rcClient.right > 0 && rcClient.bottom > 0
		&& Client::m_nGameWidth > 0 && Client::m_nGameHeight > 0)
	{
		dScaleX = (double)rcClient.right / Client::m_nGameWidth;
		dScaleY = (double)rcClient.bottom / Client::m_nGameHeight;
	}
	else
	{
		dScaleX = dScaleY = Client::windowScale > 0.0 ? Client::windowScale : 1.0;
	}

	*pHWnd = hWnd;
	*pSx = ptOrg.x + (int)(nAbsLeft * dScaleX + 0.5);
	*pSy = ptOrg.y + (int)((nAbsTop + IME_FOLLOW_LINE_OFFSET) * dScaleY + 0.5);
	*pLineH = (int)(nFontHeight * dScaleY + 0.5);
	return true;
}

// Answers WM_IME_REQUEST / IMR_QUERYCHARPOSITION. IMECHARPOSITION::pt and
// rcDocument are expected in SCREEN coordinates (the IME lives in another
// process and positions its own top-level window from them).
static bool ImeFollowQueryCharPosition(LPARAM lParam)
{
	if (lParam == 0)
		return false;
	HWND hWnd = nullptr;
	int sx = 0, sy = 0, lineH = 0;
	if (!ComputeImeCaret(&hWnd, &sx, &sy, &lineH))
		return false;

	IMECHARPOSITION* pIcp = (IMECHARPOSITION*)lParam;
	pIcp->dwSize = sizeof(IMECHARPOSITION);
	pIcp->pt.x = sx;
	pIcp->pt.y = sy;
	pIcp->cLineHeight = (lineH > 0) ? (UINT)lineH : 16;
	RECT rc = { 0, 0, 0, 0 };
	if (GetClientRect(hWnd, &rc))
	{
		POINT tl = { rc.left, rc.top };
		POINT br = { rc.right, rc.bottom };
		ClientToScreen(hWnd, &tl);
		ClientToScreen(hWnd, &br);
		pIcp->rcDocument.left = tl.x;
		pIcp->rcDocument.top = tl.y;
		pIcp->rcDocument.right = br.x;
		pIcp->rcDocument.bottom = br.y;
	}
	return true;
}
