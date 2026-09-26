/* SGFix - Space Giraffe (PC) refresh-rate fix, as a d3d9.dll proxy.
 *
 * The game builds its video-mode menu from IDirect3D9::EnumAdapterModes and keeps only the first
 * entry it sees for each resolution. Direct3D 9 lists modes ascending by refresh rate and the game
 * skips rates below 50 Hz, so on a display that advertises 50 Hz the menu ends up with 50 Hz for
 * every resolution. This proxy sits between the game and d3d9.dll and returns the mode list with
 * the preferred refresh rate first for each resolution (configurable in sgfix.ini).
 * Nothing in the game executable is modified.
 *
 * Deliberately plain C with nothing but kernel32 + a few msvcrt string functions, no CRT start-up
 * code and no TLS directory, and every export is safe to call before DllMain has run (overlays and
 * other injected DLLs do that during process start-up).
 */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SGFIX_VERSION "1.2.0"
/* {85C31227-3DE5-4f00-9B3A-F11AC38C18B5} = IID_IDirect3DTexture9 (defined here so no uuid library is needed) */
static const GUID kIID_IDirect3DTexture9 = { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } };
#define MAX_PREF 16
#define MAX_LISTS 16
#define MAX_MODES 2048

/* ------------------------------------------------------------------------------------------ */
/* config + log */
static int   g_prefer[MAX_PREF]; static int g_npref = 0;
static int   g_keep_others = 0, g_min_width = 0, g_min_height = 0, g_force_refresh = 0, g_present_interval = -1, g_logon = 1, g_hooks = 1, g_capture_debug = 1;
static int   g_dump_frame = 300, g_dump_count = 2, g_log_textures = 1;
static int   g_rt_scale = 4, g_rt_max = 1024;      /* render-target upscale factor for the game's small offscreen targets */
static WCHAR g_reshade[64];
static WCHAR g_dir[MAX_PATH];
static HANDLE g_log = INVALID_HANDLE_VALUE;
static SRWLOCK g_lock = SRWLOCK_INIT;      /* zero-initialised: usable before DllMain */
static HINSTANCE g_inst = NULL;
static int g_dllmain_ran = 0, g_cfg_loaded = 0, g_log_tried = 0;
static HMODULE g_real = NULL;
int g_real_loaded = 0;               /* read by the asm thunks */

static void EnsureDir(void)
{
    HMODULE h = NULL; WCHAR* s;
    if (g_dir[0]) return;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&EnsureDir, &h)) return;
    GetModuleFileNameW(h, g_dir, MAX_PATH);
    s = wcsrchr(g_dir, L'\\'); if (s) s[1] = 0;
}
static void LogOpen(void)
{
    WCHAR path[MAX_PATH], tmp[MAX_PATH];
    if (g_log_tried) return;
    g_log_tried = 1;
    EnsureDir();
    _snwprintf(path, MAX_PATH, L"%ssgfix.log", g_dir);
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE && GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH)) {
        _snwprintf(path, MAX_PATH, L"%s\\sgfix.log", tmp);
        g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
}

static void Log(const char* fmt, ...)
{
    char buf[2048], stamp[32]; SYSTEMTIME t; DWORD w; int n, m; va_list ap;
    va_start(ap, fmt); n = _vsnprintf(buf, sizeof buf - 3, fmt, ap); va_end(ap);
    if (n < 0) n = (int)sizeof buf - 3;
    buf[n++] = '\r'; buf[n++] = '\n'; buf[n] = 0;
    OutputDebugStringA(buf);
    if (!g_log_tried) LogOpen();
    if (g_log == INVALID_HANDLE_VALUE) return;
    GetLocalTime(&t);
    m = _snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    WriteFile(g_log, stamp, (DWORD)m, &w, NULL); WriteFile(g_log, buf, (DWORD)n, &w, NULL);
    FlushFileBuffers(g_log);
}

static void LoadConfig(void)
{
    WCHAR ini[MAX_PATH], s[256]; WCHAR* p;
    if (g_cfg_loaded) return;
    g_cfg_loaded = 1;
    EnsureDir();
    _snwprintf(ini, MAX_PATH, L"%ssgfix.ini", g_dir);
    GetPrivateProfileStringW(L"sgfix", L"refresh", L"60,120,144,100,59,50", s, 256, ini);
    for (p = s; *p && g_npref < MAX_PREF;) {
        int v;
        while (*p == L' ' || *p == L',') p++;
        if (!*p) break;
        v = _wtoi(p); if (v > 0) g_prefer[g_npref++] = v;
        while (*p && *p != L',') p++;
    }
    g_keep_others      = (int)GetPrivateProfileIntW(L"sgfix", L"keep_others", 0, ini);
    g_capture_debug    = (int)GetPrivateProfileIntW(L"sgfix", L"capture_debug", 1, ini);
    g_dump_frame       = (int)GetPrivateProfileIntW(L"sgfix", L"dump_frame", 300, ini);
    g_dump_count       = (int)GetPrivateProfileIntW(L"sgfix", L"dump_count", 2, ini);
    g_log_textures     = (int)GetPrivateProfileIntW(L"sgfix", L"log_textures", 1, ini);
    g_rt_scale         = (int)GetPrivateProfileIntW(L"sgfix", L"rt_scale", 4, ini);
    g_rt_max           = (int)GetPrivateProfileIntW(L"sgfix", L"rt_max", 1024, ini);
    if (g_rt_scale < 1) g_rt_scale = 1; if (g_rt_scale > 8) g_rt_scale = 8;
    GetPrivateProfileStringW(L"sgfix", L"reshade", L"ReShade32.dll", g_reshade, 64, ini);
    g_min_width        = (int)GetPrivateProfileIntW(L"sgfix", L"min_width", 0, ini);
    g_min_height       = (int)GetPrivateProfileIntW(L"sgfix", L"min_height", 0, ini);
    g_force_refresh    = (int)GetPrivateProfileIntW(L"sgfix", L"force_refresh", 0, ini);
    g_present_interval = (int)GetPrivateProfileIntW(L"sgfix", L"present_interval", -1, ini);
    g_logon            = (int)GetPrivateProfileIntW(L"sgfix", L"log", 1, ini);
    g_hooks            = (int)GetPrivateProfileIntW(L"sgfix", L"hooks", 1, ini);
}

/* ------------------------------------------------------------------------------------------ */
/* vtable hooking */
static void* HookVtable(void* obj, int index, void* hook)
{
    void** vt = *(void***)obj; void* orig; DWORD old, tmp;
    if (vt[index] == hook) return NULL;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) { Log("VirtualProtect failed (%lu)", GetLastError()); return NULL; }
    orig = vt[index]; vt[index] = hook;
    VirtualProtect(&vt[index], sizeof(void*), old, &tmp);
    return orig;
}

/* ------------------------------------------------------------------------------------------ */
/* mode lists */
typedef UINT    (STDMETHODCALLTYPE *PFN_GetAdapterModeCount)(IDirect3D9*, UINT, D3DFORMAT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_EnumAdapterModes)(IDirect3D9*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static PFN_GetAdapterModeCount orig_GetAdapterModeCount; static PFN_EnumAdapterModes orig_EnumAdapterModes;
static PFN_CreateDevice orig_CreateDevice; static PFN_Reset orig_Reset;

typedef struct { UINT adapter; D3DFORMAT fmt; UINT n; D3DDISPLAYMODE* modes; } ModeList;
static ModeList g_lists[MAX_LISTS]; static int g_nlists = 0;

static int PrefRank(UINT hz)
{
    int i;
    for (i = 0; i < g_npref; i++) if ((UINT)g_prefer[i] == hz) return i;
    return 1000000 - (int)hz;         /* not in the preference list: after all preferred ones, highest first */
}
typedef struct { D3DDISPLAYMODE m; int order; } SortItem;
static int __cdecl CmpMode(const void* a, const void* b)
{
    const SortItem* x = (const SortItem*)a; const SortItem* y = (const SortItem*)b;
    int ra, rb;
    /* group by resolution in the order Direct3D listed them (ascending width, height) */
    if (x->m.Width != y->m.Width) return x->m.Width < y->m.Width ? -1 : 1;
    if (x->m.Height != y->m.Height) return x->m.Height < y->m.Height ? -1 : 1;
    ra = PrefRank(x->m.RefreshRate); rb = PrefRank(y->m.RefreshRate);
    if (ra != rb) return ra < rb ? -1 : 1;
    return x->order < y->order ? -1 : (x->order > y->order ? 1 : 0);
}

static ModeList* GetList(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt)
{
    int i; UINT n, k, kept; SortItem* items; ModeList* L;
    for (i = 0; i < g_nlists; i++) if (g_lists[i].adapter == adapter && g_lists[i].fmt == fmt) return &g_lists[i];
    if (g_nlists >= MAX_LISTS) return NULL;
    L = &g_lists[g_nlists++]; L->adapter = adapter; L->fmt = fmt; L->n = 0;
    n = orig_GetAdapterModeCount(d3d, adapter, fmt); if (n > MAX_MODES) n = MAX_MODES;
    items = (SortItem*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SortItem) * (n ? n : 1));
    L->modes = (D3DDISPLAYMODE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(D3DDISPLAYMODE) * (n ? n : 1));
    if (!items || !L->modes) { L->n = 0; return L; }
    k = 0;
    for (i = 0; i < (int)n; i++) {
        D3DDISPLAYMODE m; memset(&m, 0, sizeof m);
        if (FAILED(orig_EnumAdapterModes(d3d, adapter, fmt, (UINT)i, &m))) continue;
        if (g_logon) Log("   real  %ux%u @%u Hz", m.Width, m.Height, m.RefreshRate);
        if ((int)m.Width < g_min_width || (int)m.Height < g_min_height) continue;
        items[k].m = m; items[k].order = i; k++;
    }
    Log("adapter %u format %d: %u modes from Direct3D, %u after size filter", adapter, (int)fmt, n, k);
    qsort(items, k, sizeof(SortItem), CmpMode);
    kept = 0;
    for (i = 0; i < (int)k; i++) {
        if (!g_keep_others && i > 0 && items[i].m.Width == items[i - 1].m.Width && items[i].m.Height == items[i - 1].m.Height) continue;
        L->modes[kept++] = items[i].m;
    }
    L->n = kept;
    HeapFree(GetProcessHeap(), 0, items);
    Log("adapter %u format %d: %u modes handed to the game (preferred refresh first per resolution)", adapter, (int)fmt, kept);
    if (g_logon) for (i = 0; i < (int)kept; i++) Log("   game  %ux%u @%u Hz", L->modes[i].Width, L->modes[i].Height, L->modes[i].RefreshRate);
    return L;
}

static UINT STDMETHODCALLTYPE Hook_GetAdapterModeCount(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt)
{
    UINT n; ModeList* L;
    AcquireSRWLockExclusive(&g_lock); L = GetList(d3d, adapter, fmt); n = L ? L->n : 0; ReleaseSRWLockExclusive(&g_lock);
    return n;
}
static HRESULT STDMETHODCALLTYPE Hook_EnumAdapterModes(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt, UINT index, D3DDISPLAYMODE* out)
{
    HRESULT hr = D3DERR_INVALIDCALL; ModeList* L;
    if (!out) return D3DERR_INVALIDCALL;
    AcquireSRWLockExclusive(&g_lock);
    L = GetList(d3d, adapter, fmt);
    if (L && index < L->n) { *out = L->modes[index]; hr = D3D_OK; }
    ReleaseSRWLockExclusive(&g_lock);
    return hr;
}

static void LogPP(const char* what, const D3DPRESENT_PARAMETERS* pp)
{
    if (!pp) { Log("%s: no presentation parameters", what); return; }
    Log("%s: %ux%u format %d, windowed=%d, refresh=%u Hz, interval=%#x, swap=%d, buffers=%u, ms=%d, flags=%#lx, hwnd=%p",
        what, pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->BackBufferFormat, pp->Windowed, pp->FullScreen_RefreshRateInHz,
        pp->PresentationInterval, (int)pp->SwapEffect, pp->BackBufferCount, (int)pp->MultiSampleType, pp->Flags, (void*)pp->hDeviceWindow);
}
static void ApplyOverrides(D3DPRESENT_PARAMETERS* pp)
{
    if (!pp) return;
    if (g_force_refresh > 0 && !pp->Windowed) { Log("   override: refresh %u -> %d Hz", pp->FullScreen_RefreshRateInHz, g_force_refresh); pp->FullScreen_RefreshRateInHz = (UINT)g_force_refresh; }
    if (g_present_interval >= 0) {
        UINT v = g_present_interval == 0 ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;
        Log("   override: presentation interval %#x -> %#x", pp->PresentationInterval, v); pp->PresentationInterval = v;
    }
}
static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp);
/* ---- render-target / frame-structure logging (to find out at what resolution the game draws) */
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateTexture)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateRenderTarget)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDepthStencilSurface)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_StretchRect)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetTexture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetSamplerState)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawIndexedPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DrawIndexedPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Clear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetFVF)(IDirect3DDevice9*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetVertexDeclaration)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetVertexShader)(IDirect3DDevice9*, IDirect3DVertexShader9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetPixelShader)(IDirect3DDevice9*, IDirect3DPixelShader9*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetScissorRect)(IDirect3DDevice9*, const RECT*);
static PFN_CreateTexture orig_CreateTexture; static PFN_CreateRenderTarget orig_CreateRenderTarget; static PFN_CreateDepthStencilSurface orig_CreateDepthStencilSurface;
static PFN_StretchRect orig_StretchRect; static PFN_SetRenderTarget orig_SetRenderTarget; static PFN_SetViewport orig_SetViewport; static PFN_SetTexture orig_SetTexture;
static PFN_SetSamplerState orig_SetSamplerState; static PFN_DrawPrimitive orig_DrawPrimitive; static PFN_DrawIndexedPrimitive orig_DrawIndexedPrimitive;
static PFN_DrawPrimitiveUP orig_DrawPrimitiveUP; static PFN_DrawIndexedPrimitiveUP orig_DrawIndexedPrimitiveUP; static PFN_Present orig_Present; static PFN_Clear orig_Clear;
static PFN_SetFVF orig_SetFVF; static PFN_SetVertexDeclaration orig_SetVertexDeclaration; static PFN_SetVertexShader orig_SetVertexShader; static PFN_SetPixelShader orig_SetPixelShader; static PFN_SetScissorRect orig_SetScissorRect;

static UINT g_frame = 0; static int g_draws = 0, g_rtsets = 0, g_stretch = 0, g_clears = 0; static int g_dumping = 0, g_dump_left = 0, g_dump_req = 0; static int g_tex_logged = 0;
static IDirect3DBaseTexture9* g_tex0 = NULL; static DWORD g_mag0 = 0xffff, g_min0 = 0xffff; static D3DVIEWPORT9 g_vp; static IDirect3DSurface9* g_rt0 = NULL;
static DWORD g_fvf = 0; static IDirect3DVertexDeclaration9* g_vdecl = NULL; static IDirect3DVertexShader9* g_vs = NULL; static IDirect3DPixelShader9* g_ps = NULL;
static int g_rt0_scaled = 0;                     /* current render target is one we enlarged */
static int g_scaled_rts = 0;

static const char* FmtName(D3DFORMAT f)
{
    switch (f) {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8"; case D3DFMT_X8R8G8B8: return "X8R8G8B8"; case D3DFMT_R5G6B5: return "R5G6B5"; case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DFMT_A32B32G32R32F: return "A32B32G32R32F"; case D3DFMT_R32F: return "R32F"; case D3DFMT_D24S8: return "D24S8"; case D3DFMT_D24X8: return "D24X8"; case D3DFMT_D16: return "D16";
    case D3DFMT_A8: return "A8"; case D3DFMT_L8: return "L8"; case D3DFMT_DXT1: return "DXT1"; case D3DFMT_DXT3: return "DXT3"; case D3DFMT_DXT5: return "DXT5"; case D3DFMT_A2R10G10B10: return "A2R10G10B10";
    case D3DFMT_G16R16F: return "G16R16F"; case D3DFMT_A8B8G8R8: return "A8B8G8R8"; case D3DFMT_X8B8G8R8: return "X8B8G8R8"; case D3DFMT_A1R5G5B5: return "A1R5G5B5";
    default: { static char b[4][16]; static int i; char* s = b[i++ & 3]; _snprintf(s, 16, "fmt%d", (int)f); return s; }
    }
}
static const char* SurfDesc(IDirect3DSurface9* s, char* buf, int n)
{
    D3DSURFACE_DESC d;
    if (!s) { _snprintf(buf, n, "null"); return buf; }
    if (FAILED(IDirect3DSurface9_GetDesc(s, &d))) { _snprintf(buf, n, "surface@%p", (void*)s); return buf; }
    _snprintf(buf, n, "%ux%u %s%s%s", d.Width, d.Height, FmtName(d.Format), (d.Usage & D3DUSAGE_RENDERTARGET) ? " RT" : "", d.MultiSampleType ? " MSAA" : "");
    return buf;
}
static const char* TexDesc(IDirect3DBaseTexture9* t, char* buf, int n)
{
    IDirect3DTexture9* tex = NULL; D3DSURFACE_DESC d;
    if (!t) { _snprintf(buf, n, "none"); return buf; }
    if (SUCCEEDED(IDirect3DBaseTexture9_QueryInterface(t, &kIID_IDirect3DTexture9, (void**)&tex)) && tex) {
        if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc(tex, 0, &d))) _snprintf(buf, n, "tex %ux%u %s%s", d.Width, d.Height, FmtName(d.Format), (d.Usage & D3DUSAGE_RENDERTARGET) ? " RT" : "");
        else _snprintf(buf, n, "tex@%p", (void*)t);
        IDirect3DTexture9_Release(tex);
    } else _snprintf(buf, n, "non-2D tex@%p", (void*)t);
    return buf;
}
static const char* FilterName(DWORD f) { switch (f) { case D3DTEXF_NONE: return "none"; case D3DTEXF_POINT: return "point"; case D3DTEXF_LINEAR: return "linear"; case D3DTEXF_ANISOTROPIC: return "aniso"; default: return "?"; } }

static HRESULT STDMETHODCALLTYPE Hook_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* sh)
{
    HRESULT hr; UINT ow = w, oh = h; int scaled = 0;
    if (g_rt_scale > 1 && (usage & D3DUSAGE_RENDERTARGET) && pool == D3DPOOL_DEFAULT && (int)w <= g_rt_max && (int)h <= g_rt_max && w >= 64 && h >= 64) {
        w *= g_rt_scale; h *= g_rt_scale; scaled = 1;
    }
    hr = orig_CreateTexture(dev, w, h, levels, usage, fmt, pool, out, sh);
    if (scaled && FAILED(hr)) { Log("CreateTexture %ux%u (scaled from %ux%u) failed %#lx - retrying at the original size", w, h, ow, oh, (unsigned long)hr); w = ow; h = oh; scaled = 0; hr = orig_CreateTexture(dev, w, h, levels, usage, fmt, pool, out, sh); }
    if (scaled && SUCCEEDED(hr)) { g_scaled_rts++; if (g_scaled_rts <= 60) Log("render target %ux%u enlarged to %ux%u (x%d)", ow, oh, w, h, g_rt_scale); }
    if (g_log_textures && ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DYNAMIC)) || w >= 512 || h >= 512) && g_tex_logged < 400) {
        g_tex_logged++;
        Log("CreateTexture %ux%u levels=%u usage=%#lx%s%s %s pool=%d -> %#lx (%p)", w, h, levels, usage, (usage & D3DUSAGE_RENDERTARGET) ? " RENDERTARGET" : "", (usage & D3DUSAGE_DYNAMIC) ? " DYNAMIC" : "", FmtName(fmt), (int)pool, (unsigned long)hr, out ? (void*)*out : NULL);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CreateRenderTarget(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD msq, BOOL lockable, IDirect3DSurface9** out, HANDLE* sh)
{
    HRESULT hr = orig_CreateRenderTarget(dev, w, h, fmt, ms, msq, lockable, out, sh);
    Log("CreateRenderTarget %ux%u %s ms=%d -> %#lx (%p)", w, h, FmtName(fmt), (int)ms, (unsigned long)hr, out ? (void*)*out : NULL);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CreateDepthStencilSurface(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD msq, BOOL discard, IDirect3DSurface9** out, HANDLE* sh)
{
    HRESULT hr = orig_CreateDepthStencilSurface(dev, w, h, fmt, ms, msq, discard, out, sh);
    Log("CreateDepthStencilSurface %ux%u %s ms=%d -> %#lx", w, h, FmtName(fmt), (int)ms, (unsigned long)hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_StretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE filter)
{
    g_stretch++;
    if (g_dumping) { char a[64], b[64]; Log("   StretchRect %s%s -> %s%s filter=%s", SurfDesc(src, a, 64), sr ? " (rect)" : "", SurfDesc(dst, b, 64), dr ? " (rect)" : "", FilterName(filter)); }
    return orig_StretchRect(dev, src, sr, dst, dr, filter);
}
static HRESULT STDMETHODCALLTYPE Hook_SetRenderTarget(IDirect3DDevice9* dev, DWORD idx, IDirect3DSurface9* surf)
{
    if (idx == 0) {
        D3DSURFACE_DESC d;
        g_rt0 = surf; g_rt0_scaled = 0;
        if (surf && g_rt_scale > 1 && SUCCEEDED(IDirect3DSurface9_GetDesc(surf, &d)) && (d.Usage & D3DUSAGE_RENDERTARGET) && d.Pool == D3DPOOL_DEFAULT && d.Format == D3DFMT_A8R8G8B8
            && (int)d.Width <= g_rt_max * g_rt_scale && (int)d.Height <= g_rt_max * g_rt_scale && (d.Width % g_rt_scale) == 0 && (int)d.Width / g_rt_scale >= 64) g_rt0_scaled = 1;
    }
    g_rtsets++;
    if (g_dumping) { char a[64]; Log("   RT%lu <- %s", idx, SurfDesc(surf, a, 64)); }
    return orig_SetRenderTarget(dev, idx, surf);
}
static HRESULT STDMETHODCALLTYPE Hook_SetViewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp)
{
    if (vp) g_vp = *vp;
    if (g_dumping && vp) Log("   viewport <- %lux%lu @%lu,%lu", vp->Width, vp->Height, vp->X, vp->Y);
    return orig_SetViewport(dev, vp);
}
static HRESULT STDMETHODCALLTYPE Hook_SetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* t) { if (stage == 0) g_tex0 = t; return orig_SetTexture(dev, stage, t); }
static HRESULT STDMETHODCALLTYPE Hook_SetFVF(IDirect3DDevice9* dev, DWORD fvf) { g_fvf = fvf; g_vdecl = NULL; return orig_SetFVF(dev, fvf); }
static HRESULT STDMETHODCALLTYPE Hook_SetVertexDeclaration(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* d) { g_vdecl = d; if (d) g_fvf = 0; return orig_SetVertexDeclaration(dev, d); }
static HRESULT STDMETHODCALLTYPE Hook_SetVertexShader(IDirect3DDevice9* dev, IDirect3DVertexShader9* v) { g_vs = v; return orig_SetVertexShader(dev, v); }
static HRESULT STDMETHODCALLTYPE Hook_SetPixelShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* p) { g_ps = p; return orig_SetPixelShader(dev, p); }
static HRESULT STDMETHODCALLTYPE Hook_SetScissorRect(IDirect3DDevice9* dev, const RECT* r)
{
    RECT sr;
    if (g_dumping && r) Log("   scissor <- (%ld,%ld)-(%ld,%ld)%s", r->left, r->top, r->right, r->bottom, g_rt0_scaled ? " [scaled]" : "");
    if (r && g_rt0_scaled) { sr.left = r->left * g_rt_scale; sr.top = r->top * g_rt_scale; sr.right = r->right * g_rt_scale; sr.bottom = r->bottom * g_rt_scale; return orig_SetScissorRect(dev, &sr); }
    return orig_SetScissorRect(dev, r);
}
/* Vertex data of pre-transformed (XYZRHW) draws is in render-target pixels: scale it for enlarged targets. */
static int PreTransformed(void) { return g_vdecl == NULL && (g_fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW; }
static void* ScaleUP(const void* data, UINT nverts, UINT stride, void* buf, UINT bufsize)
{
    UINT i; char* d; float f = (float)g_rt_scale;
    if (!data || !stride || nverts * stride > bufsize) return NULL;
    memcpy(buf, data, nverts * stride); d = (char*)buf;
    for (i = 0; i < nverts; i++) { float* v = (float*)(d + i * stride); v[0] *= f; v[1] *= f; }
    return buf;
}
static UINT VertsForPrims(D3DPRIMITIVETYPE pt, UINT prims)
{
    switch (pt) { case D3DPT_POINTLIST: return prims; case D3DPT_LINELIST: return prims * 2; case D3DPT_LINESTRIP: return prims + 1; case D3DPT_TRIANGLELIST: return prims * 3; case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return prims + 2; default: return 0; }
}
static HRESULT STDMETHODCALLTYPE Hook_SetSamplerState(IDirect3DDevice9* dev, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD v)
{
    if (sampler == 0) { if (type == D3DSAMP_MAGFILTER) g_mag0 = v; else if (type == D3DSAMP_MINFILTER) g_min0 = v; }
    return orig_SetSamplerState(dev, sampler, type, v);
}
static HRESULT STDMETHODCALLTYPE Hook_Clear(IDirect3DDevice9* dev, DWORD n, const D3DRECT* r, DWORD flags, D3DCOLOR c, float z, DWORD st)
{
    D3DRECT sr[8]; DWORD i;
    g_clears++;
    if (g_dumping) { char a[64]; Log("   clear flags=%#lx color=%#lx rects=%lu on %s%s", flags, (unsigned long)c, n, SurfDesc(g_rt0, a, 64), g_rt0_scaled ? " [scaled]" : ""); }
    if (n && r && g_rt0_scaled && n <= 8) {
        for (i = 0; i < n; i++) { sr[i].x1 = r[i].x1 * g_rt_scale; sr[i].y1 = r[i].y1 * g_rt_scale; sr[i].x2 = r[i].x2 * g_rt_scale; sr[i].y2 = r[i].y2 * g_rt_scale; }
        return orig_Clear(dev, n, sr, flags, c, z, st);
    }
    return orig_Clear(dev, n, r, flags, c, z, st);
}
static void LogDraw(const char* kind, D3DPRIMITIVETYPE pt, UINT count)
{
    char a[64], b[64];
    g_draws++;
    if (!g_dumping) return;
    Log("   d%-3d %s type=%d prims=%u | RT=%s%s | %s vs=%s ps=%s | tex0=%s mag=%s min=%s", g_draws, kind, (int)pt, count, SurfDesc(g_rt0, a, 64), g_rt0_scaled ? " [scaled]" : "",
        g_vdecl ? "vdecl" : (PreTransformed() ? "XYZRHW" : "fvf"), g_vs ? "yes" : "no", g_ps ? "yes" : "no", TexDesc(g_tex0, b, 64), FilterName(g_mag0), FilterName(g_min0));
}
static int g_warned_vb = 0;
static HRESULT STDMETHODCALLTYPE Hook_DrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE pt, UINT start, UINT count)
{
    LogDraw("DrawPrimitive", pt, count);
    if (g_rt0_scaled && PreTransformed() && !g_warned_vb) { g_warned_vb = 1; Log("WARNING: pre-transformed vertex-buffer draw into an enlarged render target (cannot be scaled) - set rt_scale=1 if the picture is wrong"); }
    return orig_DrawPrimitive(dev, pt, start, count);
}
static HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE pt, INT bv, UINT mi, UINT nv, UINT si, UINT count) { LogDraw("DrawIndexedPrimitive", pt, count); return orig_DrawIndexedPrimitive(dev, pt, bv, mi, nv, si, count); }
static char g_upbuf[65536];
static HRESULT STDMETHODCALLTYPE Hook_DrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE pt, UINT count, const void* d, UINT stride)
{
    LogDraw("DrawPrimitiveUP", pt, count);
    if (g_rt0_scaled && PreTransformed()) { void* sd = ScaleUP(d, VertsForPrims(pt, count), stride, g_upbuf, sizeof g_upbuf); if (sd) return orig_DrawPrimitiveUP(dev, pt, count, sd, stride); }
    return orig_DrawPrimitiveUP(dev, pt, count, d, stride);
}
static HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE pt, UINT mi, UINT nv, UINT count, const void* idx, D3DFORMAT ifmt, const void* vd, UINT stride)
{
    LogDraw("DrawIndexedPrimitiveUP", pt, count);
    if (g_rt0_scaled && PreTransformed()) { void* sd = ScaleUP(vd, mi + nv, stride, g_upbuf, sizeof g_upbuf); if (sd) return orig_DrawIndexedPrimitiveUP(dev, pt, mi, nv, count, idx, ifmt, sd, stride); }
    return orig_DrawIndexedPrimitiveUP(dev, pt, mi, nv, count, idx, ifmt, vd, stride);
}
static HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* dev, const RECT* sr, const RECT* dr, HWND hwnd, const RGNDATA* dirty)
{
    static int k9 = 0; int n9; HRESULT hr;
    n9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0; if (n9 && !k9) g_dump_req = 1; k9 = n9;
    if (g_dumping) Log("---- end of frame %u: %d draws, %d clears, %d RT sets, %d StretchRects", g_frame, g_draws, g_clears, g_rtsets, g_stretch);
    hr = orig_Present(dev, sr, dr, hwnd, dirty);
    if (g_frame % 600 == 599) Log("stats: frame %u, last frame had %d draws, %d clears, %d RT sets, %d StretchRects", g_frame, g_draws, g_clears, g_rtsets, g_stretch);
    if (g_dumping && --g_dump_left <= 0) g_dumping = 0;
    g_frame++;
    if (!g_dumping && (g_dump_req || (g_dump_frame > 0 && (int)g_frame == g_dump_frame))) { g_dump_req = 0; g_dumping = 1; g_dump_left = g_dump_count > 0 ? g_dump_count : 1; Log("==== dumping %d frame(s) starting with frame %u ====", g_dump_left, g_frame); }
    g_draws = g_clears = g_rtsets = g_stretch = 0;
    return hr;
}
static void HookDevice(IDirect3DDevice9* dev)
{
    void* o;
#define HOOKD(idx, name) o = HookVtable(dev, idx, (void*)&Hook_##name); if (o) orig_##name = (PFN_##name)o;
    HOOKD(16, Reset) HOOKD(17, Present) HOOKD(23, CreateTexture) HOOKD(28, CreateRenderTarget) HOOKD(29, CreateDepthStencilSurface)
    HOOKD(34, StretchRect) HOOKD(37, SetRenderTarget) HOOKD(43, Clear) HOOKD(47, SetViewport) HOOKD(65, SetTexture) HOOKD(69, SetSamplerState)
    HOOKD(81, DrawPrimitive) HOOKD(82, DrawIndexedPrimitive) HOOKD(83, DrawPrimitiveUP) HOOKD(84, DrawIndexedPrimitiveUP)
    HOOKD(75, SetScissorRect) HOOKD(87, SetVertexDeclaration) HOOKD(89, SetFVF) HOOKD(92, SetVertexShader) HOOKD(107, SetPixelShader)
#undef HOOKD
    Log("IDirect3DDevice9 vtable hooked (render logging on; F9 dumps the next %d frames; rt_scale=%d)", g_dump_count, g_rt_scale);
}

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    HRESULT hr;
    LogPP("Reset", pp); ApplyOverrides(pp);
    hr = orig_Reset(dev, pp);
    Log("Reset -> %#lx", (unsigned long)hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND hwnd, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    HRESULT hr; static int hooked = 0;
    LogPP("CreateDevice", pp); ApplyOverrides(pp);
    hr = orig_CreateDevice(d3d, adapter, type, hwnd, flags, pp, out);
    Log("CreateDevice(adapter %u, type %d, flags %#lx) -> %#lx", adapter, (int)type, flags, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) {
        D3DDISPLAYMODE dm;
        if (!hooked) { hooked = 1; HookDevice(*out); }
        memset(&dm, 0, sizeof dm);
        if (SUCCEEDED(IDirect3DDevice9_GetDisplayMode(*out, 0, &dm))) Log("   device display mode now %ux%u @%u Hz format %d", dm.Width, dm.Height, dm.RefreshRate, (int)dm.Format);
    }
    return hr;
}

/* ------------------------------------------------------------------------------------------ */
/* exports */
#define FORWARDED(X) \
    X(Direct3DCreate9Ex) X(Direct3DCreate9On12) X(Direct3DCreate9On12Ex) \
    X(Direct3DShaderValidatorCreate9) X(PSGPError) X(PSGPSampleTexture) \
    X(D3DPERF_BeginEvent) X(D3DPERF_EndEvent) X(D3DPERF_GetStatus) X(D3DPERF_QueryRepeatFrame) X(D3DPERF_SetMarker) \
    X(D3DPERF_SetOptions) X(D3DPERF_SetRegion) X(DebugSetLevel) X(DebugSetMute)

#define DECL_PTR(n) void* p_##n = NULL;
FORWARDED(DECL_PTR)
#undef DECL_PTR
void WINAPI thunk_missing(void) { Log("FATAL: a forwarded d3d9 export was called but the real d3d9.dll is not available"); ExitProcess(0xD9D9); }

typedef IDirect3D9* (WINAPI *PFN_Direct3DCreate9)(UINT);
static PFN_Direct3DCreate9 real_Direct3DCreate9 = NULL;

static const char* ModuleNameOf(void* addr, char* buf, int n)
{
    HMODULE h = NULL; char path[MAX_PATH]; const char* p;
    if (!addr || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &h)) { _snprintf(buf, n, "unknown module"); return buf; }
    GetModuleFileNameA(h, path, MAX_PATH);
    p = strrchr(path, '\\'); p = p ? p + 1 : path;
    _snprintf(buf, n, "%s+%#lx", p, (unsigned long)((char*)addr - (char*)h));
    return buf;
}

/* The Windows AppCompat shim engine (AcLayers.dll) calls the functions below during process
   start-up, before any DllMain has run and while LoadLibrary is not yet usable. They are
   therefore implemented natively: the call is recorded and replayed into the real d3d9.dll once
   it has been loaded. (ReShade handles the same exports the same way.) */
static int g_shim_pending = 0; static int g_shim_unknown = 0; static UINT g_shim_mode = 0;
static void ForwardOrdinal(int ordinal, int a, UINT b, int nargs)
{
    FARPROC p;
    if (!g_real_loaded || !g_real) return;
    p = GetProcAddress(g_real, (LPCSTR)(UINT_PTR)ordinal);
    if (!p) return;
    if (nargs == 2) ((void (WINAPI*)(int, UINT))p)(a, b); else ((void (WINAPI*)(int))p)(a);
}
void WINAPI Direct3D9ForceHybridEnumeration(UINT mode)               { ForwardOrdinal(16, (int)mode, 0, 1); }
void WINAPI Direct3D9SetMaximizedWindowedModeShim(int unknown, UINT mode)
{
    if (!g_real_loaded) { g_shim_pending = 1; g_shim_unknown = unknown; g_shim_mode = mode; return; }
    ForwardOrdinal(17, unknown, mode, 2);
}
void WINAPI Direct3D9SetSwapEffectUpgradeShim(int unknown)           { ForwardOrdinal(18, unknown, 0, 1); }
void WINAPI Direct3D9Force9on12(int unknown)                         { ForwardOrdinal(19, unknown, 0, 1); }
void WINAPI Direct3D9SetMaximizedWindowHwndOverride(int unknown)     { ForwardOrdinal(22, unknown, 0, 1); }
void WINAPI Direct3D9SetVendorIDLieFor9on12(int unknown)             { ForwardOrdinal(23, unknown, 0, 1); }
void WINAPI Direct3D9EnableMaximizedWindowedModeShim(int unknown)    { Direct3D9SetMaximizedWindowedModeShim(unknown, 1); }

void __cdecl EnsureRealFrom(void* caller)
{
    if (g_real_loaded) return;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_real_loaded) {
        WCHAR sys[MAX_PATH], path[MAX_PATH]; char who[300];
        LoadConfig();
        Log("call into d3d9.dll from %s (DllMain %s run yet)", ModuleNameOf(caller, who, sizeof who), g_dllmain_ran ? "has" : "has NOT");
        if (g_reshade[0]) {                              /* optional ReShade (renamed, e.g. ReShade32.dll) next to the game */
            HMODULE rs; _snwprintf(path, MAX_PATH, L"%s%s", g_dir, g_reshade);
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
                rs = LoadLibraryW(path);
                Log("ReShade: %ls %s", path, rs ? "loaded (it hooks the real d3d9.dll loaded next)" : "FAILED to load");
            }
        }
        GetSystemDirectoryW(sys, MAX_PATH);
        _snwprintf(path, MAX_PATH, L"%s\\d3d9.dll", sys);
        g_real = LoadLibraryW(path);
        if (!g_real) Log("cannot load %ls (err %lu)%s", path, GetLastError(), g_dllmain_ran ? "" : " - process still initialising, will retry on the next call");
        else {
            Log("real d3d9.dll loaded from %ls at %p", path, (void*)g_real);
#define RESOLVE(n) p_##n = (void*)GetProcAddress(g_real, #n); if (!p_##n) { p_##n = (void*)&thunk_missing; Log("note: system d3d9.dll has no export %s", #n); }
            FORWARDED(RESOLVE)
#undef RESOLVE
            real_Direct3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(g_real, "Direct3DCreate9");
            g_real_loaded = 1;
            if (g_shim_pending) { g_shim_pending = 0; Log("replaying Direct3D9SetMaximizedWindowedModeShim(%d, %u) recorded during start-up", g_shim_unknown, g_shim_mode); ForwardOrdinal(17, g_shim_unknown, g_shim_mode, 2); }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/* The game reports its video-mode decisions with OutputDebugStringA ("Mode : w %d h %d ref %d",
   "Current video mode : ... SKIP %d" ...). Redirect the exe's import of it into our log. */
typedef void (WINAPI *PFN_OutputDebugStringA)(LPCSTR);
static PFN_OutputDebugStringA real_OutputDebugStringA = NULL;
static void WINAPI Hook_OutputDebugStringA(LPCSTR str)
{
    if (str) {
        char buf[1024]; size_t n = strlen(str); if (n >= sizeof buf) n = sizeof buf - 1;
        memcpy(buf, str, n); buf[n] = 0;
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        Log("[game] %s", buf);
    }
    if (real_OutputDebugStringA) real_OutputDebugStringA(str);
}
static void HookGameDebugOutput(void)
{
    static int done = 0;
    HMODULE exe; IMAGE_DOS_HEADER* dos; IMAGE_NT_HEADERS* nt; IMAGE_DATA_DIRECTORY* dir; IMAGE_IMPORT_DESCRIPTOR* imp;
    if (done || !g_capture_debug) return;
    done = 1;
    exe = GetModuleHandleW(NULL); dos = (IMAGE_DOS_HEADER*)exe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    nt = (IMAGE_NT_HEADERS*)((char*)exe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR*)((char*)exe + dir->VirtualAddress); imp->Name; imp++) {
        const char* dll = (const char*)exe + imp->Name;
        IMAGE_THUNK_DATA* names; IMAGE_THUNK_DATA* iat; int i;
        if (_stricmp(dll, "kernel32.dll") != 0 || !imp->OriginalFirstThunk) continue;
        names = (IMAGE_THUNK_DATA*)((char*)exe + imp->OriginalFirstThunk);
        iat = (IMAGE_THUNK_DATA*)((char*)exe + imp->FirstThunk);
        for (i = 0; names[i].u1.AddressOfData; i++) {
            IMAGE_IMPORT_BY_NAME* ibn;
            if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
            ibn = (IMAGE_IMPORT_BY_NAME*)((char*)exe + names[i].u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, "OutputDebugStringA") == 0) {
                DWORD old, tmp;
                if (VirtualProtect(&iat[i].u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                    if (!real_OutputDebugStringA) real_OutputDebugStringA = (PFN_OutputDebugStringA)(UINT_PTR)iat[i].u1.Function;
                    iat[i].u1.Function = (UINT_PTR)&Hook_OutputDebugStringA;
                    VirtualProtect(&iat[i].u1.Function, sizeof(void*), old, &tmp);
                    Log("game debug output captured (OutputDebugStringA import redirected)");
                }
            }
        }
    }
}

IDirect3D9* WINAPI Direct3DCreate9(UINT sdk)
{
    IDirect3D9* d3d; static int hooked = 0;
    EnsureRealFrom(__builtin_return_address(0));
    HookGameDebugOutput();
    if (!real_Direct3DCreate9) { Log("Direct3DCreate9: real function unavailable"); return NULL; }
    d3d = real_Direct3DCreate9(sdk);
    Log("Direct3DCreate9(sdk %u) -> %p", sdk, (void*)d3d);
    if (d3d && g_hooks) {
        if (!hooked) {
            void* o; D3DDISPLAYMODE dm;
            hooked = 1;
            o = HookVtable(d3d, 6, (void*)&Hook_GetAdapterModeCount); if (o) orig_GetAdapterModeCount = (PFN_GetAdapterModeCount)o;
            o = HookVtable(d3d, 7, (void*)&Hook_EnumAdapterModes);    if (o) orig_EnumAdapterModes = (PFN_EnumAdapterModes)o;
            o = HookVtable(d3d, 16, (void*)&Hook_CreateDevice);       if (o) orig_CreateDevice = (PFN_CreateDevice)o;
            Log("IDirect3D9 vtable hooked");
            memset(&dm, 0, sizeof dm);
            if (SUCCEEDED(IDirect3D9_GetAdapterDisplayMode(d3d, 0, &dm))) Log("desktop mode: %ux%u @%u Hz format %d", dm.Width, dm.Height, dm.RefreshRate, (int)dm.Format);
        }
    } else if (d3d) Log("hooks=0: forwarding only");
    return d3d;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        WCHAR exe[MAX_PATH]; char pref[256] = ""; int i;
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        g_dllmain_ran = 1;
        LoadConfig();
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        Log("SGFix %s (d3d9.dll proxy) DllMain: attached to %ls, dll at %p%s", SGFIX_VERSION, exe, (void*)inst, g_real_loaded ? " (real d3d9 was already loaded by an earlier call)" : "");
        for (i = 0; i < g_npref; i++) { char b[16]; _snprintf(b, 16, "%s%d", i ? "," : "", g_prefer[i]); strncat(pref, b, sizeof pref - strlen(pref) - 1); }
        Log("config: refresh preference [%s] keep_others=%d min %dx%d force_refresh=%d present_interval=%d hooks=%d capture_debug=%d", pref, g_keep_others, g_min_width, g_min_height, g_force_refresh, g_present_interval, g_hooks, g_capture_debug);
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("detach");
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    }
    return TRUE;
}
