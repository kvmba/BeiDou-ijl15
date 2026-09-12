#pragma once
#include <imm.h>
#pragma comment(lib, "imm32.lib")
#include <cstdio>
#include <cstdarg>
#include "Client.h"

// IME candidate window follow fix (GMS083 BeiDou client).
//
// Background (verified on BeiDou.exe, imagebase 0x400000):
//  * The client never answers WM_IME_REQUEST / IMR_QUERYCHARPOSITION, the query
//    modern (Win8+) IMEs use to learn the caret position, so their UI falls back
//    to the default spot near the taskbar.
//  * It also never positions the composition window for AT_CARET IMEs (it only
//    does so when IME_PROP_SPECIAL_UI is set), and on WM_INPUTLANGCHANGE it
//    pushes the candidate window off-screen once, before any edit box has focus.
//
// Fix, applied from the subclassed main-window procedure (WindowScaleProc) after
// the game has handled each composition update: answer IMR_QUERYCHARPOSITION,
// and push the IMM composition (client) and candidate (screen) windows onto the
// focused edit control's caret.
//
// NOTE: WM_IME_NOTIFY is deliberately NOT handled -- moving the candidate window
// emits IMN_SETCANDIDATEPOS (a WM_IME_NOTIFY), which would recurse forever.
// ImeFollowForceWindows() is additionally guarded against re-entrancy.

#ifndef IMR_QUERYCHARPOSITION
#define IMR_QUERYCHARPOSITION 0x000Cu
#endif

// CWndMan singleton: focused control + main window handle.
#define IME_WNDMAN_PTR         0x00BEC20Cu
#define IME_WNDMAN_FOCUS       0x88u    // m_pFocus (IUIMsgHandler*)
#define IME_WNDMAN_HWND        0xACu    // m_hWnd

// CCtrlEdit/CCtrlMLEdit: line height (m_nFontHeight).
#define IME_CTRL_FONT_HEIGHT   0x7Cu
#define IME_CTRL_CARET_X       0x58u    // m_nCaretX (caret pixel X)
#define IME_CTRL_VIEWPORT_X    0x60u    // m_nViewportX (horizontal scroll)

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
	FILE* f = fopen("imefollow.log", "a");
	if (!f)
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
	// focus is the IUIMsgHandler subobject (control base + 4); the layout
	// offsets below are relative to the control base.
	DWORD ctrl = focus - 4;

	// GetAbsLeft/GetAbsTop return the control position in render space;
	// map render -> client pixels -> screen (current client size keeps
	// runtime window resizing correct; windowScale is the startup value).
	int nAbsLeft    = ((int(__thiscall*)(DWORD))*(DWORD*)(vft + IME_IUIMSG_GETABSLEFT))(focus);
	int nAbsTop     = ((int(__thiscall*)(DWORD))*(DWORD*)(vft + IME_IUIMSG_GETABSTOP))(focus);
	int nFontHeight = *(int*)(ctrl + IME_CTRL_FONT_HEIGHT);
	int nCaretX     = *(int*)(ctrl + IME_CTRL_CARET_X);
	int nViewportX  = *(int*)(ctrl + IME_CTRL_VIEWPORT_X);
	// caret X follows the insertion point (same formula the client's own
	// CIMECandWnd uses): GetAbsLeft + m_nCaretX - m_nViewportX.
	nAbsLeft += nCaretX - nViewportX;
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
	ImeFollowLog("caret: absL=%d absT=%d caretX=%d viewX=%d fontH=%d | org=(%d,%d) scale=(%.3f,%.3f) | sx=%d sy=%d lh=%d\n",
		nAbsLeft - (nCaretX - nViewportX), nAbsTop, nCaretX, nViewportX, nFontHeight,
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

	if (Client::debug) {
		char buf[128];
		wsprintfA(buf, "[imeFollow] comp=(%d,%d)c cand=(%d,%d)s line=%d\n",
			ptClient.x, ptClient.y, sx, sy, lineH);
		OutputDebugStringA(buf);
	}
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
	if (Client::debug)
		OutputDebugStringA("[imeFollow] IMR_QUERYCHARPOSITION answered\n");
	return true;
}
