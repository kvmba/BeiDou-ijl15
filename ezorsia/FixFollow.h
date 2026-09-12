#pragma once
#include <imm.h>
#include "Client.h"

// IME candidate/composition window follow fix (GMS083 BeiDou client).
//
// Root cause: sub_9E7D77 (the WM_IME_* dispatcher, == CWndMan::TranslateMessage)
// forces the IMM composition/candidate windows to the off-screen point
// (-SM_CXSCREEN, -SM_CYSCREEN) via ImmSetCompositionWindow / ImmSetCandidateWindow.
// That only takes effect on modern IMEs, which then draw nothing at the caret.
// We intercept those two calls and reposition the windows at the focused input
// control instead, so the system IME UI follows the chat/input box.
//
// Verified against BeiDou.exe (GMS083): imagebase 0x400000.

// IMM32 entries the client resolves dynamically at startup (function pointers).
#define IME_FN_SET_CANDIDATE_WINDOW   0x00BF05E8u  // ImmSetCandidateWindow
#define IME_FN_SET_COMPOSITION_WINDOW 0x00BF05F0u  // ImmSetCompositionWindow

// CWndMan singleton: holds the focused control and the game window handle.
#define IME_WNDMAN_PTR         0x00BEC20Cu
#define IME_WNDMAN_FOCUS       0x88u   // m_pFocus (IUIMsgHandler*)
#define IME_WNDMAN_HWND        0xACu   // m_hWnd

// IUIMsgHandler vtable slots on the focused control.
#define IME_IUIMSG_GETABSLEFT  0x2Cu   // GetAbsLeft()
#define IME_IUIMSG_GETABSTOP   0x30u   // GetAbsTop()

// Hook sites inside sub_9E7D77 (each is a 6-byte "call dword ptr [...]").
#define IME_SITE_SET_CANDIDATE_WINDOW   0x009E7F77u
#define IME_RTN_SET_CANDIDATE_WINDOW    0x009E7F7Du
#define IME_SITE_SET_COMPOSITION_WINDOW 0x009E7E4Cu
#define IME_RTN_SET_COMPOSITION_WINDOW  0x009E7E52u

#define IME_CFS_POINT          0x0002u
#define IME_CFS_CANDIDATEPOS   0x0040u

// Vertical offset (render units) from the control top down to the text line.
#define IME_FOLLOW_LINE_OFFSET 20

static int g_imeFollowX = 0;
static int g_imeFollowY = 0;

// Screen-space anchor for the IME windows, derived from the focused control.
static bool ComputeImeFollowPoint(int* pOutX, int* pOutY)
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
	int nAbsLeft = ((int(__thiscall*)(DWORD))*(DWORD*)(vft + IME_IUIMSG_GETABSLEFT))(focus);
	int nAbsTop  = ((int(__thiscall*)(DWORD))*(DWORD*)(vft + IME_IUIMSG_GETABSTOP))(focus);
	POINT ptOrg = { 0, 0 };
	ClientToScreen(hWnd, &ptOrg);
	RECT rcClient = { 0, 0, 0, 0 };
	double dScaleX = 1.0, dScaleY = 1.0;
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
	*pOutX = ptOrg.x + (int)(nAbsLeft * dScaleX + 0.5);
	*pOutY = ptOrg.y + (int)((nAbsTop + IME_FOLLOW_LINE_OFFSET) * dScaleY + 0.5);
	return true;
}

// Mimics the replaced "call ImmSetCandidateWindow(hImc, pCdf)".
// pCdf takes SCREEN coordinates.
static void __cdecl ImeFollowSetCandidateWindow(void* hImc, CANDIDATEFORM* pCdf)
{
	if (pCdf != nullptr && ComputeImeFollowPoint(&g_imeFollowX, &g_imeFollowY))
	{
		pCdf->dwStyle = IME_CFS_CANDIDATEPOS;
		pCdf->ptCurrentPos.x = g_imeFollowX;
		pCdf->ptCurrentPos.y = g_imeFollowY;
	}
	typedef BOOL(WINAPI* Fn)(HIMC, LPCANDIDATEFORM);
	void* pFn = *(void**)IME_FN_SET_CANDIDATE_WINDOW;
	if (pFn != nullptr && hImc != nullptr && pCdf != nullptr)
		((Fn)pFn)((HIMC)hImc, pCdf);
}

// Mimics the replaced "call ImmSetCompositionWindow(hImc, pCff)".
// COMPOSITIONFORM::ptCurrentPos is in CLIENT-area coordinates of the app
// window (the composition window is a child window; the convention is to feed
// it GetCaretPos()-style client points). CANDIDATEFORM, by contrast, is in
// screen coordinates. So the screen point is converted back for this one.
static void __cdecl ImeFollowSetCompositionWindow(void* hImc, COMPOSITIONFORM* pCff)
{
	if (pCff != nullptr && ComputeImeFollowPoint(&g_imeFollowX, &g_imeFollowY))
	{
		HWND hWnd = *(HWND*)(*(DWORD*)IME_WNDMAN_PTR + IME_WNDMAN_HWND);
		if (hWnd != nullptr)
		{
			POINT pt = { g_imeFollowX, g_imeFollowY };
			ScreenToClient(hWnd, &pt);
			pCff->dwStyle = IME_CFS_POINT;
			pCff->ptCurrentPos.x = pt.x;
			pCff->ptCurrentPos.y = pt.y;
		}
	}
	typedef BOOL(WINAPI* Fn)(HIMC, LPCOMPOSITIONFORM);
	void* pFn = *(void**)IME_FN_SET_COMPOSITION_WINDOW;
	if (pFn != nullptr && hImc != nullptr && pCff != nullptr)
		((Fn)pFn)((HIMC)hImc, pCff);
}

// The 6-byte "call dword ptr [ImmSetXxx]" is patched to a jmp to this thunk,
// so on entry [esp] already holds arg1 (hImc) and [esp+4] arg2 (struct) --
// there is no return address. The replaced import is WINAPI (stdcall), so we
// must both drop our own pushed args (cdecl) and emulate its "retn 8".
DWORD imeCandRtnAddr = IME_RTN_SET_CANDIDATE_WINDOW;
__declspec(naked) void imeFollowCandHook()
{
	__asm {
		mov  eax, [esp]        // hImc
		mov  ecx, [esp + 4]    // LPCANDIDATEFORM
		push ecx               // arg2
		push eax               // arg1
		call ImeFollowSetCandidateWindow
		add  esp, 8            // drop cdecl args
		add  esp, 8            // emulate stdcall retn 8
		jmp  imeCandRtnAddr
	}
}

DWORD imeCompRtnAddr = IME_RTN_SET_COMPOSITION_WINDOW;
__declspec(naked) void imeFollowCompHook()
{
	__asm {
		mov  eax, [esp]        // hImc
		mov  ecx, [esp + 4]    // LPCOMPOSITIONFORM
		push ecx               // arg2
		push eax               // arg1
		call ImeFollowSetCompositionWindow
		add  esp, 8            // drop cdecl args
		add  esp, 8            // emulate stdcall retn 8
		jmp  imeCompRtnAddr
	}
}

class FixFollow {
public:
	static void Hook() {
		Memory::CodeCave(imeFollowCandHook, IME_SITE_SET_CANDIDATE_WINDOW, 6);
		Memory::CodeCave(imeFollowCompHook, IME_SITE_SET_COMPOSITION_WINDOW, 6);
		std::cout << "Ime follow hook created" << std::endl;
	}
};
