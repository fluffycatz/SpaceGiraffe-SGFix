// SGFix — Space Giraffe (PC) refresh-rate fix, as a d3d9.dll proxy.
//
// The game builds its video-mode menu from IDirect3D9::EnumAdapterModes and keeps only the first
// entry it sees for each resolution. Direct3D 9 lists modes ascending by refresh rate, and the
// game skips rates below 50 Hz, so on a TV/monitor that advertises 50 Hz the menu ends up with
// 50 Hz for every resolution. This proxy sits between the game and d3d9.dll and returns the mode
// list with the preferred refresh rate first for each resolution (configurable in sgfix.ini).
// Nothing in the game executable is modified.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <vector>
#include <map>
#include <algorithm>

#define SGFIX_VERSION "1.0.0"

// ---------------------------------------------------------------------------------------------
// config + log
struct Config {
    std::vector<int> prefer;      // refresh rates in order of preference
    int keep_others = 1;          // keep the non-preferred rates after the preferred one
    int min_width = 0, min_height = 0;
    int force_refresh = 0;        // if > 0: overwrite FullScreen_RefreshRateInHz in CreateDevice/Reset
    int present_interval = -1;    // if >= 0: overwrite PresentationInterval (0 = immediate, 1 = one vsync)
    int log = 1;
};
static Config g_cfg;
static wchar_t g_dir[MAX_PATH];
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_cs;
static HMODULE g_real = nullptr;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    EnterCriticalSection(&g_cs);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

static void LoadConfig()
{
    wchar_t ini[MAX_PATH]; _snwprintf(ini, MAX_PATH, L"%ssgfix.ini", g_dir);
    wchar_t s[256]; GetPrivateProfileStringW(L"sgfix", L"refresh", L"60,120,144,100,59,50", s, 256, ini);
    for (wchar_t* p = s; *p;) {
        while (*p == L' ' || *p == L',') p++;
        if (!*p) break;
        int v = _wtoi(p); if (v > 0) g_cfg.prefer.push_back(v);
        while (*p && *p != L',') p++;
    }
    g_cfg.keep_others      = (int)GetPrivateProfileIntW(L"sgfix", L"keep_others", 1, ini);
    g_cfg.min_width        = (int)GetPrivateProfileIntW(L"sgfix", L"min_width", 0, ini);
    g_cfg.min_height       = (int)GetPrivateProfileIntW(L"sgfix", L"min_height", 0, ini);
    g_cfg.force_refresh    = (int)GetPrivateProfileIntW(L"sgfix", L"force_refresh", 0, ini);
    g_cfg.present_interval = (int)GetPrivateProfileIntW(L"sgfix", L"present_interval", -1, ini);
    g_cfg.log              = (int)GetPrivateProfileIntW(L"sgfix", L"log", 1, ini);
}

// ---------------------------------------------------------------------------------------------
// vtable hooking
static void* HookVtable(void* obj, int index, void* hook)
{
    void** vt = *(void***)obj;
    if (vt[index] == hook) return nullptr;
    DWORD old;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) { Log("VirtualProtect failed (%lu)", GetLastError()); return nullptr; }
    void* orig = vt[index]; vt[index] = hook;
    DWORD tmp; VirtualProtect(&vt[index], sizeof(void*), old, &tmp);
    return orig;
}
#define HOOK(obj, idx, name) do { void* o = HookVtable(obj, idx, (void*)&Hook_##name); if (o) orig_##name = (decltype(orig_##name))o; } while (0)

// ---------------------------------------------------------------------------------------------
// mode lists
typedef UINT    (STDMETHODCALLTYPE *PFN_GetAdapterModeCount)(IDirect3D9*, UINT, D3DFORMAT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_EnumAdapterModes)(IDirect3D9*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static PFN_GetAdapterModeCount orig_GetAdapterModeCount; static PFN_EnumAdapterModes orig_EnumAdapterModes;
static PFN_CreateDevice orig_CreateDevice; static PFN_Reset orig_Reset;

struct ModeList { std::vector<D3DDISPLAYMODE> modes; };
static std::map<std::pair<UINT, D3DFORMAT>, ModeList> g_lists;

static int PrefRank(UINT hz)
{
    for (size_t i = 0; i < g_cfg.prefer.size(); i++) if ((UINT)g_cfg.prefer[i] == hz) return (int)i;
    return 1000000 - (int)hz;                    // not in the preference list: after all preferred, highest first
}

static const ModeList& GetList(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt)
{
    auto key = std::make_pair(adapter, fmt);
    auto it = g_lists.find(key);
    if (it != g_lists.end()) return it->second;
    ModeList& L = g_lists[key];
    UINT n = orig_GetAdapterModeCount(d3d, adapter, fmt);
    std::vector<D3DDISPLAYMODE> real;
    for (UINT i = 0; i < n; i++) { D3DDISPLAYMODE m = {}; if (SUCCEEDED(orig_EnumAdapterModes(d3d, adapter, fmt, i, &m))) real.push_back(m); }
    Log("adapter %u format %d: %u modes from Direct3D", adapter, (int)fmt, (unsigned)real.size());
    if (g_cfg.log) { for (const auto& m : real) Log("   real  %ux%u @%u Hz", m.Width, m.Height, m.RefreshRate); }
    // group by resolution in order of first appearance
    std::vector<std::pair<UINT, UINT>> order;
    std::map<std::pair<UINT, UINT>, std::vector<D3DDISPLAYMODE>> groups;
    for (const auto& m : real) {
        if ((int)m.Width < g_cfg.min_width || (int)m.Height < g_cfg.min_height) continue;
        auto r = std::make_pair(m.Width, m.Height);
        if (!groups.count(r)) order.push_back(r);
        groups[r].push_back(m);
    }
    for (const auto& r : order) {
        auto& g = groups[r];
        std::stable_sort(g.begin(), g.end(), [](const D3DDISPLAYMODE& a, const D3DDISPLAYMODE& b) { return PrefRank(a.RefreshRate) < PrefRank(b.RefreshRate); });
        for (size_t i = 0; i < g.size(); i++) { if (i > 0 && !g_cfg.keep_others) break; L.modes.push_back(g[i]); }
    }
    Log("adapter %u format %d: %u modes handed to the game (preferred refresh first per resolution)", adapter, (int)fmt, (unsigned)L.modes.size());
    if (g_cfg.log) { for (const auto& m : L.modes) Log("   game  %ux%u @%u Hz", m.Width, m.Height, m.RefreshRate); }
    return L;
}

static UINT STDMETHODCALLTYPE Hook_GetAdapterModeCount(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt)
{
    EnterCriticalSection(&g_cs); const ModeList& L = GetList(d3d, adapter, fmt); UINT n = (UINT)L.modes.size(); LeaveCriticalSection(&g_cs);
    return n;
}
static HRESULT STDMETHODCALLTYPE Hook_EnumAdapterModes(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt, UINT index, D3DDISPLAYMODE* out)
{
    if (!out) return D3DERR_INVALIDCALL;
    EnterCriticalSection(&g_cs);
    const ModeList& L = GetList(d3d, adapter, fmt);
    HRESULT hr = D3DERR_INVALIDCALL;
    if (index < L.modes.size()) { *out = L.modes[index]; hr = D3D_OK; }
    LeaveCriticalSection(&g_cs);
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
    if (g_cfg.force_refresh > 0 && !pp->Windowed) { Log("   override: refresh %u -> %d Hz", pp->FullScreen_RefreshRateInHz, g_cfg.force_refresh); pp->FullScreen_RefreshRateInHz = g_cfg.force_refresh; }
    if (g_cfg.present_interval >= 0) {
        UINT v = g_cfg.present_interval == 0 ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;
        Log("   override: presentation interval %#x -> %#x", pp->PresentationInterval, v); pp->PresentationInterval = v;
    }
}
static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    LogPP("Reset", pp); ApplyOverrides(pp);
    HRESULT hr = orig_Reset(dev, pp);
    Log("Reset -> %#lx", (unsigned long)hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND hwnd, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    LogPP("CreateDevice", pp); ApplyOverrides(pp);
    HRESULT hr = orig_CreateDevice(d3d, adapter, type, hwnd, flags, pp, out);
    Log("CreateDevice(adapter %u, type %d, flags %#lx) -> %#lx", adapter, (int)type, flags, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) {
        static bool hooked = false;
        if (!hooked) { hooked = true; HOOK(*out, 16, Reset); }
        D3DDISPLAYMODE dm = {}; if (SUCCEEDED((*out)->GetDisplayMode(0, &dm))) Log("   device display mode now %ux%u @%u Hz format %d", dm.Width, dm.Height, dm.RefreshRate, (int)dm.Format);
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------
// exports
#define FORWARDED(X) \
    X(Direct3D9EnableMaximizedWindowedModeShim) X(Direct3DCreate9Ex) X(Direct3DCreate9On12) X(Direct3DCreate9On12Ex) \
    X(Direct3DShaderValidatorCreate9) X(PSGPError) X(PSGPSampleTexture) \
    X(D3DPERF_BeginEvent) X(D3DPERF_EndEvent) X(D3DPERF_GetStatus) X(D3DPERF_QueryRepeatFrame) X(D3DPERF_SetMarker) \
    X(D3DPERF_SetOptions) X(D3DPERF_SetRegion) X(DebugSetLevel) X(DebugSetMute)

extern "C" {
#define DECL_PTR(n) void* p_##n = nullptr;
FORWARDED(DECL_PTR)
#undef DECL_PTR
void WINAPI thunk_missing() { Log("FATAL: a forwarded d3d9 export is missing in the system d3d9.dll"); ExitProcess(0xD9D9); }

typedef IDirect3D9* (WINAPI *PFN_Direct3DCreate9)(UINT);
static PFN_Direct3DCreate9 real_Direct3DCreate9 = nullptr;

IDirect3D9* WINAPI Direct3DCreate9(UINT sdk)
{
    if (!real_Direct3DCreate9) return nullptr;
    IDirect3D9* d3d = real_Direct3DCreate9(sdk);
    Log("Direct3DCreate9(sdk %u) -> %p", sdk, (void*)d3d);
    if (d3d) {
        static bool hooked = false;
        if (!hooked) {
            hooked = true;
            HOOK(d3d, 6, GetAdapterModeCount); HOOK(d3d, 7, EnumAdapterModes); HOOK(d3d, 16, CreateDevice);
            Log("IDirect3D9 vtable hooked");
            D3DDISPLAYMODE dm = {}; if (SUCCEEDED(d3d->GetAdapterDisplayMode(0, &dm))) Log("desktop mode: %ux%u @%u Hz format %d", dm.Width, dm.Height, dm.RefreshRate, (int)dm.Format);
        }
    }
    return d3d;
}
}

static void ResolveForwards()
{
    wchar_t sys[MAX_PATH], path[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    _snwprintf(path, MAX_PATH, L"%s\\d3d9.dll", sys);
    g_real = LoadLibraryW(path);
    if (!g_real) { Log("FATAL: cannot load %ls (err %lu)", path, GetLastError()); return; }
    Log("real d3d9.dll loaded from %ls", path);
#define RESOLVE(n) p_##n = (void*)GetProcAddress(g_real, #n); if (!p_##n) { p_##n = (void*)&thunk_missing; Log("note: system d3d9.dll has no export %s", #n); }
    FORWARDED(RESOLVE)
#undef RESOLVE
    real_Direct3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(g_real, "Direct3DCreate9");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        GetModuleFileNameW(inst, g_dir, MAX_PATH);
        wchar_t* s = wcsrchr(g_dir, L'\\'); if (s) s[1] = 0;
        LoadConfig();
        if (g_cfg.log) {
            wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%ssgfix.log", g_dir);
            g_log = _wfopen(path, L"w");
            if (!g_log) {
                wchar_t base[MAX_PATH];
                if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) { _snwprintf(path, MAX_PATH, L"%s\\SGFix", base); CreateDirectoryW(path, nullptr); _snwprintf(path, MAX_PATH, L"%s\\SGFix\\sgfix.log", base); g_log = _wfopen(path, L"w"); }
            }
        }
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        Log("SGFix %s (d3d9.dll proxy) attached to %ls", SGFIX_VERSION, exe);
        char pref[256] = ""; for (size_t i = 0; i < g_cfg.prefer.size(); i++) { char b[16]; snprintf(b, 16, "%s%d", i ? "," : "", g_cfg.prefer[i]); strncat(pref, b, sizeof pref - strlen(pref) - 1); }
        Log("config: refresh preference [%s] keep_others=%d min %dx%d force_refresh=%d present_interval=%d", pref, g_cfg.keep_others, g_cfg.min_width, g_cfg.min_height, g_cfg.force_refresh, g_cfg.present_interval);
        ResolveForwards();
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("detach");
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
