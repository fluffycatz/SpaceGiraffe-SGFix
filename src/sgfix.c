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

#define SGFIX_VERSION "1.0.3"
#define MAX_PREF 16
#define MAX_LISTS 16
#define MAX_MODES 2048

/* ------------------------------------------------------------------------------------------ */
/* config + log */
static int   g_prefer[MAX_PREF]; static int g_npref = 0;
static int   g_keep_others = 1, g_min_width = 0, g_min_height = 0, g_force_refresh = 0, g_present_interval = -1, g_logon = 1, g_hooks = 1;
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
    g_keep_others      = (int)GetPrivateProfileIntW(L"sgfix", L"keep_others", 1, ini);
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
        if (!hooked) { void* o = HookVtable(*out, 16, (void*)&Hook_Reset); if (o) orig_Reset = (PFN_Reset)o; hooked = 1; }
        memset(&dm, 0, sizeof dm);
        if (SUCCEEDED(IDirect3DDevice9_GetDisplayMode(*out, 0, &dm))) Log("   device display mode now %ux%u @%u Hz format %d", dm.Width, dm.Height, dm.RefreshRate, (int)dm.Format);
    }
    return hr;
}

/* ------------------------------------------------------------------------------------------ */
/* exports */
#define FORWARDED(X) \
    X(Direct3D9EnableMaximizedWindowedModeShim) X(Direct3DCreate9Ex) X(Direct3DCreate9On12) X(Direct3DCreate9On12Ex) \
    X(Direct3DShaderValidatorCreate9) X(PSGPError) X(PSGPSampleTexture) \
    X(D3DPERF_BeginEvent) X(D3DPERF_EndEvent) X(D3DPERF_GetStatus) X(D3DPERF_QueryRepeatFrame) X(D3DPERF_SetMarker) \
    X(D3DPERF_SetOptions) X(D3DPERF_SetRegion) X(DebugSetLevel) X(DebugSetMute)

#define DECL_PTR(n) void* p_##n = NULL;
FORWARDED(DECL_PTR)
#undef DECL_PTR
void WINAPI thunk_missing(void) { Log("FATAL: a forwarded d3d9 export is missing in the system d3d9.dll"); ExitProcess(0xD9D9); }

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

void __cdecl EnsureRealFrom(void* caller)
{
    if (g_real_loaded) return;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_real_loaded) {
        WCHAR sys[MAX_PATH], path[MAX_PATH]; char who[300];
        LoadConfig();
        Log("first call into d3d9.dll from %s (DllMain %s run yet)", ModuleNameOf(caller, who, sizeof who), g_dllmain_ran ? "has" : "has NOT");
        GetSystemDirectoryW(sys, MAX_PATH);
        _snwprintf(path, MAX_PATH, L"%s\\d3d9.dll", sys);
        g_real = LoadLibraryW(path);
        if (!g_real) Log("FATAL: cannot load %ls (err %lu)", path, GetLastError());
        else Log("real d3d9.dll loaded from %ls at %p", path, (void*)g_real);
#define RESOLVE(n) p_##n = g_real ? (void*)GetProcAddress(g_real, #n) : NULL; if (!p_##n) { p_##n = (void*)&thunk_missing; Log("note: system d3d9.dll has no export %s", #n); }
        FORWARDED(RESOLVE)
#undef RESOLVE
        real_Direct3DCreate9 = g_real ? (PFN_Direct3DCreate9)GetProcAddress(g_real, "Direct3DCreate9") : NULL;
        g_real_loaded = 1;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

IDirect3D9* WINAPI Direct3DCreate9(UINT sdk)
{
    IDirect3D9* d3d; static int hooked = 0;
    EnsureRealFrom(__builtin_return_address(0));
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
        Log("config: refresh preference [%s] keep_others=%d min %dx%d force_refresh=%d present_interval=%d hooks=%d", pref, g_keep_others, g_min_width, g_min_height, g_force_refresh, g_present_interval, g_hooks);
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("detach");
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    }
    return TRUE;
}
