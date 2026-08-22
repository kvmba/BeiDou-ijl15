#pragma once
// D3D9 post-process upscale filter (sharp-bilinear-2x-prescale).
// The game renders via Gr2D_DX9.dll (D3D9) into a fixed 1280x720 backbuffer,
// and its own Present stretch to the window is nearest-neighbor (blocky).
// We hook the game device's Present: copy its render target into a texture,
// then two passes:
//   Pass 1: POINT-sample the source into a 2x render-target texture
//           (pixel-doubling, keeps the pixel-art crisp)
//   Pass 2: sharp-bilinear pixel shader scales the 2x texture to the window
//           (bilinear in texel interiors, nearest near texel centers → crisp edges)
// Present the additional swap chain and skip the game's own Present. No second
// device, no Reset. Any failure falls back to the original present.

#include <d3d9.h>
#include <cstring>

static bool g_upscaleEnabled = true;

// ---- embedded ps_2_0 sharp-bilinear shader (compiled with fxc) ----
// c0 = texel size of the INPUT texture (1/srcW, 1/srcH).
// c1 = scale (dstW/srcW, dstH/srcH).
// Bilinear near texel edges only; interiors stay flat so edges/text stay
// crisp when upscaled.
//
// Source (compiled: fxc /T ps_2_0 /E main /O3):
/*
sampler2D imageSmp : register(s0);
float4 c0 : register(c0);
float4 c1 : register(c1);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 texel = c0.xy;
    float2 scale = c1.xy;
    float2 coords = uv / texel;
    float2 snapped = floor(coords);
    float2 fr = saturate((coords - snapped - 0.5) / scale + 0.5);
    float2 suv = (snapped + fr) * texel;
    return tex2D(imageSmp, suv);
}
*/
static const BYTE g_ps20_main[] =
{
       0,    2,  255,  255,  254,  255,   47,    0,   67,   84,
      65,   66,   28,    0,    0,    0,  143,    0,    0,    0,
       0,    2,  255,  255,    3,    0,    0,    0,   28,    0,
       0,    0,    0,  129,    0,    0,  136,    0,    0,    0,
      88,    0,    0,    0,    2,    0,    0,    0,    1,    0,
       2,    0,   92,    0,    0,    0,    0,    0,    0,    0,
     108,    0,    0,    0,    2,    0,    1,    0,    1,    0,
       6,    0,   92,    0,    0,    0,    0,    0,    0,    0,
     111,    0,    0,    0,    3,    0,    0,    0,    1,    0,
       2,    0,  120,    0,    0,    0,    0,    0,    0,    0,
      99,   48,    0,  171,    1,    0,    3,    0,    1,    0,
       4,    0,    1,    0,    0,    0,    0,    0,    0,    0,
      99,   49,    0,  105,  109,   97,  103,  101,   83,  109,
     112,    0,    4,    0,   12,    0,    1,    0,    1,    0,
       1,    0,    0,    0,    0,    0,    0,    0,  112,  115,
      95,   50,   95,   48,    0,   77,  105,   99,  114,  111,
     115,  111,  102,  116,   32,   40,   82,   41,   32,   72,
      76,   83,   76,   32,   83,  104,   97,  100,  101,  114,
      32,   67,  111,  109,  112,  105,  108,  101,  114,   32,
      49,   48,   46,   49,    0,  171,   81,    0,    0,    5,
       2,    0,   15,  160,    0,    0,    0,  191,    0,    0,
       0,   63,    0,    0,    0,    0,    0,    0,    0,    0,
      31,    0,    0,    2,    0,    0,    0,  128,    0,    0,
       3,  176,   31,    0,    0,    2,    0,    0,    0,  144,
       0,    8,   15,  160,    6,    0,    0,    2,    0,    0,
       1,  128,    0,    0,    0,  160,    6,    0,    0,    2,
       0,    0,    2,  128,    0,    0,   85,  160,    5,    0,
       0,    3,    0,    0,   12,  128,    0,    0,   27,  128,
       0,    0,   27,  176,   19,    0,    0,    2,    1,    0,
       3,  128,    0,    0,   27,  128,    2,    0,    0,    3,
       0,    0,   12,  128,    0,    0,  228,  128,    1,    0,
      27,  129,    4,    0,    0,    4,    0,    0,    3,  128,
       0,    0,  228,  176,    0,    0,  228,  128,    0,    0,
      27,  129,    2,    0,    0,    3,    0,    0,    3,  128,
       0,    0,  228,  128,    2,    0,    0,  160,    6,    0,
       0,    2,    1,    0,    1,  128,    1,    0,    0,  160,
       6,    0,    0,    2,    1,    0,    2,  128,    1,    0,
      85,  160,    4,    0,    0,    4,    0,    0,   19,  128,
       0,    0,  228,  128,    1,    0,  228,  128,    2,    0,
      85,  160,    2,    0,    0,    3,    0,    0,    3,  128,
       0,    0,  228,  128,    0,    0,   27,  128,    5,    0,
       0,    3,    0,    0,    3,  128,    0,    0,  228,  128,
       0,    0,  228,  160,   66,    0,    0,    3,    0,    0,
      15,  128,    0,    0,  228,  128,    0,    8,  228,  160,
       1,    0,    0,    2,    0,    8,   15,  128,    0,    0,
     228,  128,  255,  255,    0,    0
};

// ---- state ----
static IDirect3DDevice9* g_gameDev = nullptr;
static HWND g_d3d9Window = nullptr;
static IDirect3DSwapChain9* g_swap = nullptr;      // our window-sized additional swap chain
static UINT g_swapW = 0, g_swapH = 0;
static IDirect3DTexture9* g_tex = nullptr;         // game-frame copy (on g_gameDev, RT-usage)
static IDirect3DTexture9* g_tex2x = nullptr;       // 2x render-target intermediate
static IDirect3DPixelShader9* g_shader = nullptr;  // sharp-bilinear pixel shader
static IDirect3DStateBlock9* g_sb = nullptr;       // on g_gameDev
static UINT g_gameW = 0, g_gameH = 0;              // the game's render resolution

// ---- vtable hook types (D3D9: CreateDevice=16; Reset=16, Present=17) ----
typedef IDirect3D9* (__stdcall* D3D9CreateFn)(UINT SDKVersion);
typedef HRESULT (__stdcall* D3D9CreateDeviceFn)(void* This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pParams, IDirect3DDevice9** ppDevice);
typedef HRESULT (__stdcall* D3D9ResetFn)(void* This, D3DPRESENT_PARAMETERS* pParams);
typedef HRESULT (__stdcall* D3D9PresentFn)(void* This, const RECT* pSrc, const RECT* pDst, HWND hOverride, const RGNDATA* pDirty);

static D3D9CreateFn origDirect3DCreate9 = nullptr;
static D3D9CreateDeviceFn origCreateDevice = nullptr;
static D3D9ResetFn origReset = nullptr;
static D3D9PresentFn origPresent = nullptr;

// Drop all our device-owned objects (after a game-initiated Reset they die).
static void ReleaseOurResources() {
    if (g_sb) { g_sb->Release(); g_sb = nullptr; }
    if (g_shader) { g_shader->Release(); g_shader = nullptr; }
    if (g_tex2x) { g_tex2x->Release(); g_tex2x = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    g_swapW = g_swapH = 0;
}

static HRESULT __stdcall HookReset(void* This, D3DPRESENT_PARAMETERS* pParams); // defined below

// Create/recreate our window-sized additional swap chain on the game device.
static bool EnsureSwapChain(UINT winW, UINT winH) {
    if (g_swap && g_swapW == winW && g_swapH == winH)
        return true;
    if (winW == 0 || winH == 0 || g_gameDev == nullptr)
        return false;
    // throttle recreations while the user drags the window border
    static DWORD lastRecreate = 0;
    DWORD now = GetTickCount();
    if (g_swap && now - lastRecreate < 200)
        return false;
    lastRecreate = now;

    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = winW;
    pp.BackBufferHeight = winH;
    pp.hDeviceWindow = g_d3d9Window;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (FAILED(g_gameDev->CreateAdditionalSwapChain(&pp, &g_swap)) || !g_swap) {
        g_swap = nullptr;
        return false;
    }
    g_swapW = winW; g_swapH = winH;
    return true;
}

// Two-pass draw with sharp-bilinear edge-sharpening.
// On entry the render target is our swap chain's backbuffer.
//
// Pass 1 (no shader): POINT-sample source → 2x render-target texture.
// Each source pixel becomes a 2×2 block of identical output pixels.
//
// Pass 2 (sharp-bilinear shader): scale the 2x texture → window.
// The shader applies bilinear inside texel interiors and snapping
// near texel centers, so edges / text stay crisp at any scale.
//
// If g_tex2x is unavailable, both passes collapse to a single pass
// from the source with the shader (the original fallback).
static void RenderQuad(IDirect3DDevice9* dev, IDirect3DTexture9* tex, UINT texW, UINT texH) {
    if (!dev || !tex) return;
    IDirect3DSurface9* finalRt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &finalRt)) || finalRt == nullptr) return;
    D3DSURFACE_DESC d;
    finalRt->GetDesc(&d);
    UINT w = d.Width, h = d.Height;
    if (w == 0 || h == 0) { finalRt->Release(); return; }

    if (!g_sb)
        dev->CreateStateBlock(D3DSBT_ALL, &g_sb);
    if (g_sb)
        g_sb->Capture(); // capture the game's state

    struct V { float x, y, z, rhw; DWORD color; float u, v; };

    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetPixelShader(NULL); // clear any shader the game left bound
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

    // --- Pass 1: nearest-neighbor 2x into the intermediate render target ---
    bool have2x = false;
    if (g_tex2x) {
        IDirect3DSurface9* tex2xSurf = nullptr;
        if (SUCCEEDED(g_tex2x->GetSurfaceLevel(0, &tex2xSurf)) && tex2xSurf != nullptr) {
            if (SUCCEEDED(dev->SetRenderTarget(0, tex2xSurf))) {
                D3DVIEWPORT9 vp2x = { 0, 0, texW * 2, texH * 2, 0.0f, 1.0f };
                dev->SetViewport(&vp2x);
                dev->SetTexture(0, tex);
                dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                V verts2x[4];
                verts2x[0] = { -0.5f, -0.5f, 0.5f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f };
                verts2x[1] = { (float)(texW * 2) - 0.5f, -0.5f, 0.5f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f };
                verts2x[2] = { -0.5f, (float)(texH * 2) - 0.5f, 0.5f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f };
                verts2x[3] = { (float)(texW * 2) - 0.5f, (float)(texH * 2) - 0.5f, 0.5f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f };
                dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts2x, sizeof(V));
                have2x = true;
            }
        }
        if (tex2xSurf) tex2xSurf->Release();
    }

    // --- Pass 2: sharp-bilinear → swap chain backbuffer ---
    dev->SetRenderTarget(0, finalRt);
    D3DVIEWPORT9 vpFinal = { 0, 0, w, h, 0.0f, 1.0f };
    dev->SetViewport(&vpFinal);

    // Choose input: 2x texture if available, otherwise the source directly.
    // Shader constants depend on which texture we feed it.
    IDirect3DTexture9* pass2Tex;
    UINT srcW, srcH;
    if (have2x) {
        pass2Tex = g_tex2x;
        srcW = texW * 2;
        srcH = texH * 2;
    } else {
        pass2Tex = tex;
        srcW = texW;
        srcH = texH;
    }

    dev->SetTexture(0, pass2Tex);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);

    // Apply the sharp-bilinear pixel shader with correct constants.
    // c0 = texel size of the input (1/srcW, 1/srcH)
    // c1 = scale (dstW/srcW, dstH/srcH)
    if (g_shader) {
        float texel[2] = { 1.0f / (float)srcW, 1.0f / (float)srcH };
        float scale[4] = { (float)w / (float)srcW, (float)h / (float)srcH, 0.0f, 0.0f };
        dev->SetPixelShaderConstantF(0, texel, 1);
        dev->SetPixelShaderConstantF(1, scale, 1);
        dev->SetPixelShader(g_shader);
    } else {
        dev->SetPixelShader(NULL);
    }

    V verts[4];
    verts[0] = { -0.5f, -0.5f, 0.5f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f };
    verts[1] = { (float)w - 0.5f, -0.5f, 0.5f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f };
    verts[2] = { -0.5f, (float)h - 0.5f, 0.5f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f };
    verts[3] = { (float)w - 0.5f, (float)h - 0.5f, 0.5f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(V));
    finalRt->Release();

    if (g_sb)
        g_sb->Apply(); // restore the game's state
}

// ---- the game's D3D9 Present hook ----
static HRESULT __stdcall HookPresent(void* This, const RECT* pSrc, const RECT* pDst, HWND hOverride, const RGNDATA* pDirty) {
    // late-bind the DirectInput device hook
    {
        DWORD* pKbd = *(DWORD**)(0x00BEC33C + 8);
        DWORD* pMse = *(DWORD**)(0x00BEC33C + 12);
        if (pKbd != nullptr && *(DWORD*)(*(DWORD**)pKbd + 9) != (DWORD)HookDI8GetDeviceState)
            PatchDI8GetDeviceState((void*)pKbd);
        if (pMse != nullptr && *(DWORD*)(*(DWORD**)pMse + 9) != (DWORD)HookDI8GetDeviceState)
            PatchDI8GetDeviceState((void*)pMse);
    }

    if (!g_upscaleEnabled || This != g_gameDev || pSrc != nullptr || pDst != nullptr || hOverride != nullptr)
        return origPresent(This, pSrc, pDst, hOverride, pDirty);

    RECT rc = {0,0,0,0};
    if (g_d3d9Window) GetClientRect(g_d3d9Window, &rc);
    if (!EnsureSwapChain((UINT)rc.right, (UINT)rc.bottom))
        return origPresent(This, pSrc, pDst, hOverride, pDirty);

    // read the game's render target (current frame) into a system surface
    IDirect3DSurface9* gameRt = nullptr;
    if (FAILED(g_gameDev->GetRenderTarget(0, &gameRt)) || gameRt == nullptr)
        return origPresent(This, pSrc, pDst, hOverride, pDirty);
    D3DSURFACE_DESC dsc;
    gameRt->GetDesc(&dsc);
    if (dsc.Width == 0 || dsc.Height == 0) { gameRt->Release(); return origPresent(This, pSrc, pDst, hOverride, pDirty); }

    // (re)create the frame-copy textures when the game's render size changes.
    // RENDER_TARGET usage so they can be refreshed on the GPU via StretchRect
    // (a plain POOL_DEFAULT texture cannot be written without CPU locks,
    // and its LockRect always fails in D3D9).
    if (!g_tex || g_gameW != dsc.Width || g_gameH != dsc.Height) {
        if (g_tex) { g_tex->Release(); g_tex = nullptr; }
        if (g_tex2x) { g_tex2x->Release(); g_tex2x = nullptr; }
        if (FAILED(g_gameDev->CreateTexture(dsc.Width, dsc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_tex, NULL))) {
            gameRt->Release();
            return origPresent(This, pSrc, pDst, hOverride, pDirty);
        }
        if (FAILED(g_gameDev->CreateTexture(dsc.Width * 2, dsc.Height * 2, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_tex2x, NULL)))
            g_tex2x = nullptr;
        g_gameW = dsc.Width; g_gameH = dsc.Height;
    }

    // GPU copy of the current frame into our texture (no CPU locks involved)
    IDirect3DSurface9* texSurf = nullptr;
    if (FAILED(g_tex->GetSurfaceLevel(0, &texSurf)) || texSurf == nullptr) {
        gameRt->Release();
        return origPresent(This, pSrc, pDst, hOverride, pDirty);
    }
    HRESULT hrSr = g_gameDev->StretchRect(gameRt, NULL, texSurf, NULL, D3DTEXF_NONE);
    texSurf->Release();
    if (FAILED(hrSr)) { gameRt->Release(); return origPresent(This, pSrc, pDst, hOverride, pDirty); }

    // draw into OUR swap chain's backbuffer
    IDirect3DSurface9* swapBb = nullptr;
    if (FAILED(g_swap->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &swapBb)) || swapBb == nullptr) {
        gameRt->Release();
        return origPresent(This, pSrc, pDst, hOverride, pDirty);
    }
    g_gameDev->SetRenderTarget(0, swapBb);
    swapBb->Release();
    RenderQuad(g_gameDev, g_tex, g_gameW, g_gameH);
    HRESULT hr = g_swap->Present(NULL, NULL, NULL, NULL, 0);
    g_gameDev->SetRenderTarget(0, gameRt);
    gameRt->Release();
    if (FAILED(hr)) {
        ReleaseOurResources();
        return origPresent(This, pSrc, pDst, hOverride, pDirty);
    }
    return S_OK;
}

// ---- device creation / reset hooks ----
static HRESULT __stdcall HookReset(void* This, D3DPRESENT_PARAMETERS* pParams) {
    ReleaseOurResources();
    HRESULT hr = origReset(This, pParams);
    if (SUCCEEDED(hr) && This == g_gameDev)
        ((IDirect3DDevice9*)This)->CreatePixelShader((const DWORD*)g_ps20_main, &g_shader);
    return hr;
}

static void HookGameDevice(IDirect3DDevice9* dev) {
    DWORD* vtbl = *(DWORD**)dev;
    DWORD old;
    VirtualProtect(&vtbl[16], sizeof(DWORD) * 2, PAGE_READWRITE, &old);
    if (!origReset) origReset = (D3D9ResetFn)vtbl[16];
    if (!origPresent) origPresent = (D3D9PresentFn)vtbl[17];
    vtbl[16] = (DWORD)HookReset;
    vtbl[17] = (DWORD)HookPresent;
    VirtualProtect(&vtbl[16], sizeof(DWORD) * 2, old, &old);
    if (!g_gameDev) g_gameDev = dev;
}

static HRESULT __stdcall HookCreateDevice(void* This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pParams, IDirect3DDevice9** ppDevice) {
    HRESULT hr = origCreateDevice(This, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pParams, ppDevice);
    if (SUCCEEDED(hr) && ppDevice != nullptr && *ppDevice != nullptr) {
        if (!g_d3d9Window) g_d3d9Window = hFocusWindow;
        HookGameDevice(*ppDevice);
        if (g_gameDev == *ppDevice)
            g_gameDev->CreatePixelShader((const DWORD*)g_ps20_main, &g_shader);
    }
    return hr;
}

static void PatchCreateDevice(IDirect3D9* d3d9) {
    DWORD* vtbl = *(DWORD**)d3d9;
    DWORD old;
    VirtualProtect(&vtbl[16], sizeof(DWORD), PAGE_READWRITE, &old);
    origCreateDevice = (D3D9CreateDeviceFn)vtbl[16];
    vtbl[16] = (DWORD)HookCreateDevice;
    VirtualProtect(&vtbl[16], sizeof(DWORD), old, &old);
}

static IDirect3D9* __stdcall HookDirect3DCreate9(UINT SDKVersion) {
    IDirect3D9* d3d9 = origDirect3DCreate9(SDKVersion);
    if (d3d9 != nullptr)
        PatchCreateDevice(d3d9);
    return d3d9;
}

inline void HookD3D9Upscale(bool bEnable) {
    if (!bEnable) return;
    origDirect3DCreate9 = (D3D9CreateFn)GetProcAddress(LoadLibraryA("d3d9.dll"), "Direct3DCreate9");
    if (!origDirect3DCreate9) return;
    Memory::SetHook(true, (void**)&origDirect3DCreate9, (void*)HookDirect3DCreate9);
}
