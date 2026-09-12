#pragma once
#include <imm.h>
#pragma comment(lib, "imm32.lib")
#include <cstdio>
#include <cstdarg>
#include "Client.h"

// IME candidate window follow fix (GMS083 BeiDou client).
//
// The client never answers WM_IME_REQUEST / IMR_QUERYCHARPOSITION (the caret
// query modern IMEs use) and only positions the IMM candidate window once, on
// WM_INPUTLANGCHANGE, before any edit box has focus -- so the IME UI falls back
// to its default spot near the taskbar.
//
// We answer the query and, after the game handles each composition update,
// force the IMM composition (client coords) and candidate (screen coords)
// windows onto the focused edit control's caret.
//
// NOTE: WM_IME_NOTIFY is deliberately NOT handled (moving the candidate window
// emits IMN_SETCANDIDATEPOS, which would recurse). Re-entrancy is also guarded.

#ifndef IMR_QUERYCHARPOSITION
#define IMR_QUERYCHARPOSITION 0x000Cu
#endif

// CWndMan singleton: focused control + main window handle.
#define IME_WNDMAN_PTR         0x00BEC20Cu
#define IME_WNDMAN_FOCUS       0x88u    // m_pFocus (IUIMsgHandler* = control base + 4)
#define IME_WNDMAN_HWND        0xACu    // m_hWnd

// CCtrlEdit/CCtrlMLEdit fields (relative to the control base).
#define IME_CTRL_FONT_HEIGHT   0x7Cu    // m_nFontHeight
#define IME_CTRL_CARET_X       0x58u    // m_nCaretX (caret pixel X, render space)

// IUIMsgHandler vtable slots on the focused control.
#define IME_IUIMSG_GETABSLEFT  0x2Cu    // GetAbsLeft()
#define IME_IUIMSG_GETABSTOP   0x30u    // GetAbsTop()

// Vertical offset (render units) from the control top down to the text line.
#define IME_FOLLOW_LINE_OFFSET 20

static bool g_imeForceBusy = false;

static void ImeFollowLog(const char* fmt, ...)
{
	if (!Client::debug)
		return;
	FILE* f = nullptr;
	if (fopen_s(&f, "imefollow.log", "a") != 0 || f == nullptr)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}

// Computes the caret anchor of the focused edit control.
//   *pSx/*pSy : caret point in SCREEN pixels
//   *pLineH   : text line height in SCREEN pixels
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
	DWORD ctrl = focus - 4;   // control base

	// GetAbsLeft/GetAbsTop return the control position in render space; the
	// client area is that render resolution scaled by the window scale factor
	// (e.g. 1280x720 -> 1920x1080 at 1.5).
	int nAbsLeft    = ((int(__thiscall*)(DWORD))*(DWORD*)(vft + IME_IUIMSG_GETABSLEFT))(focus);
	int nAbsTop     = ((int(__thiscall*)(DWORD))*(DWORD*)(vft + IME_IUIMSG_GETABSTOP))(focus);
	int nFontHeight = *(int*)(focus + IME_CTRL_FONT_HEIGHT);
	// Follow the caret: adding m_nCaretX (render px) keeps the anchor on the
	// insertion point, so it stays correct as more characters are typed.
	nAbsLeft += *(int*)(ctrl + IME_CTRL_CARET_X);

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
	ImeFollowLog("caret: absL=%d absT=%d caretX=%d fontH=%d | org=(%d,%d) scale=(%.3f,%.3f) | sx=%d sy=%d lh=%d\n",
		nAbsLeft - *(int*)(ctrl + IME_CTRL_CARET_X), nAbsTop, *(int*)(ctrl + IME_CTRL_CARET_X), nFontHeight,
		ptOrg.x, ptOrg.y, dScaleX, dScaleY, *pSx, *pSy, *pLineH);
	return true;
}

// Forcibly moves the IMM composition (client coords) and candidate (screen
// coords) windows to the focused control's caret. Re-entrancy guarded.
static bool ImeFollowForceWindows()
{
	if (g_imeForceBusy)
		return false;

	HWND hWnd = nullptr;
	int sx = 0, sy = 0, lineH = 0;
	if (!ComputeImeCaret(&hWnd, &sx, &sy, &lineH))
		return false;

	HIMC hImc = ImmGetContext(hWnd);
	if (hImc == nullptr)
		return false;

	g_imeForceBusy = true;

	POINT ptClient = { sx, sy };
	ScreenToClient(hWnd, &ptClient);

	COMPOSITIONFORM cff = { 0 };
	cff.dwStyle = CFS_POINT;
	cff.ptCurrentPos.x = ptClient.x;
	cff.ptCurrentPos.y = ptClient.y;
	ImmSetCompositionWindow(hImc, &cff);

	CANDIDATEFORM cdf = { 0 };
	cdf.dwIndex = 0;
	cdf.dwStyle = CFS_CANDIDATEPOS;
	cdf.ptCurrentPos.x = sx;
	cdf.ptCurrentPos.y = sy;
	ImmSetCandidateWindow(hImc, &cdf);

	ImmReleaseContext(hWnd, hImc);
	g_imeForceBusy = false;
	return true;
}

// Answers WM_IME_REQUEST / IMR_QUERYCHARPOSITION. IMECHARPOSITION::pt and
// rcDocument are SCREEN coordinates (the IME is another process).
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
