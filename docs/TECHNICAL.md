# SGFix — technical notes

Target: `SpaceGiraffePC.exe`, Steam depot, 32-bit PE, Direct3D 9 (`d3d9.dll`, `D3DX9_40`), XACT audio,
window title/log `SGPCLogFile.txt`. The executable is wrapped in Steam's old DRM stub (a `.bind`
section that unpacks the real code at run time with material it gets from the Steam client): it will
not start outside Steam ("Application load error 5:0000065434") and its code is not present in
readable form on disk. So, unlike Tempest 4000 and Moose Life, there is nothing to patch statically —
every change has to be made from outside the process image, at run time. A `d3d9.dll` proxy in the game
folder is the least invasive way to do that: the game loads it instead of the system library through
normal DLL search order, and no memory of the game is ever written.

## Problem 1: the 50 Hz menu

The game builds its video-mode menu from `IDirect3D9::GetAdapterModeCount` / `EnumAdapterModes`
(format `D3DFMT_X8R8G8B8`, 22). Direct3D returns modes grouped by resolution, and within a resolution
in ascending refresh-rate order — on an LG G3 over HDMI 2.1 with an RTX 5090, 14 rates per resolution:

```
real  3840x2160 @24 Hz … @30 @48 @50 @59 @60 @100 @119 @120 @144 Hz
```

The game's own debug output (`OutputDebugStringA`, captured into `sgfix.log` as `[game] …` lines:
`Mode : w %d h %d ref %d`, `Current video mode : … SKIP %d`) shows what it does with them: it skips
anything below 50 Hz and keeps, per resolution, the **lowest** rate that is left. Re-ordering the list
therefore changes nothing — the first version of SGFix put the preferred rate first per resolution and
the game still picked 50 Hz. The list has to be *reduced*: with `keep_others=0` (default) each
resolution is handed exactly one entry, the highest-priority rate from `refresh=` that the display
supports, and that is what the game uses. `Reset:` in the log confirms it
(`Reset: 3840x2160 format 22, windowed=0, refresh=60 Hz, …`).

The menu text ("@50Hz") is not derived from the mode list at all but from `screen_refresh` in the
game's config file, hence it can lag behind the real mode; it is cosmetic.

Why 60 and not 120: the game is frame-locked (one logic step per presented frame, no delta time), so
120 Hz runs it at double speed and 50 Hz at 5/6 speed. Nothing in the proxy can change that without
patching the game code in memory, which this project deliberately does not do.

## Problem 2: the soft picture

The per-frame dump (`dump_frame` / **F9**) shows the frame structure. At 3840×2160 the game runs, per
frame, about 39 draws, 27 clears and 52 render-target switches, and *all* of the scene rendering goes
into offscreen `A8R8G8B8` textures of 512×512 pixels (`CreateTexture … usage=0x1 RENDERTARGET`,
around 14 of them — feedback/bloom chains). Each target is cleared, drawn into with the previous target
bound as texture 0 (`mag=linear min=linear`), and so on; at the end a single pre-transformed
`D3DPT_TRIANGLEFAN` quad (`DrawPrimitiveUP`, `XYZRHW` FVF) stretches the last 512×512 texture over the
back buffer. The screen mode only ever affects that final stretch — the game is a 512-pixel game
bilinearly blown up to 4K, which is exactly how it looks.

Observations that make an external fix possible:

* the dumps show no `SetViewport` calls and no scissor use around these targets — the game relies on
  the viewport that D3D9 resets to the full target on every `SetRenderTarget`, so enlarging the target
  enlarges the viewport with it;
* every texture read uses 0–1 texture coordinates, so a larger source texture just gets sampled more
  finely;
* the only thing expressed in target pixels is the pre-transformed geometry: quads and sprites drawn
  with `D3DFVF_XYZRHW` vertices through `DrawPrimitiveUP` / `DrawIndexedPrimitiveUP` (user-memory
  vertices, no vertex buffers), plus the occasional clear rectangle.

So SGFix creates any render-target texture ≤ `rt_max` per side `rt_scale` times larger
(`Hook_CreateTexture`; falls back to the original size if the driver refuses), tracks whether the
current colour target is one of those (`Hook_SetRenderTarget` — recognised by its `D3DSURFACE_DESC`:
render-target usage, default pool, `A8R8G8B8`, size a multiple of `rt_scale` of an original of at least
64), and while it is:

* `DrawPrimitiveUP` / `DrawIndexedPrimitiveUP` with a pre-transformed FVF copy the vertex data into a
  64 KB scratch buffer and multiply x and y (the first two floats of every vertex) by `rt_scale`;
* `SetScissorRect` and the rectangles of `Clear` are multiplied the same way.

The final stretch to the back buffer draws into the (unscaled) back buffer, so it is left alone — it
now samples a 2048×2048 texture instead of 512×512. `DrawPrimitive` (vertex-buffer) draws with a
pre-transformed FVF into a scaled target cannot be fixed this way; the game does not issue any, and
SGFix logs a one-time warning if it ever does.

## The proxy

### Exports

`src/d3d9.def` exports everything the system `d3d9.dll` exports that any program might import:

* `Direct3DCreate9` — implemented here (loads the real library, calls its `Direct3DCreate9`, hooks the
  returned `IDirect3D9`).
* `Direct3DCreate9Ex`, `Direct3DCreate9On12`, `Direct3DCreate9On12Ex`, `Direct3DShaderValidatorCreate9`,
  `PSGPError`, `PSGPSampleTexture`, `D3DPERF_*`, `DebugSetLevel`, `DebugSetMute` — assembly thunks
  (`src/thunks.S`): `jmp *p_<name>` through a pointer resolved from the real library, or into
  `thunk_missing` if it is called before the real library is available.
* The application-compatibility shim entry points, by name *and* by ordinal (16–23, `NONAME`):
  `Direct3D9ForceHybridEnumeration` (16), `Direct3D9SetMaximizedWindowedModeShim` (17),
  `Direct3D9SetSwapEffectUpgradeShim` (18), `Direct3D9Force9on12` (19),
  `Direct3D9SetMaximizedWindowHwndOverride` (22), `Direct3D9SetVendorIDLieFor9on12` (23) and
  `Direct3D9EnableMaximizedWindowedModeShim`; `Direct3DCreate9On12` / `…Ex` keep their ordinals 20/21.
  The shim functions are implemented natively, see below.

### Start-up: the AppCompat shim engine

This was the hard part, and the reason 1.0.0–1.0.2 crashed the game at start-up with `0xc0000005`
(Event Viewer: faulting module `ntdll.dll`, or "unknown module, offset 0").

Windows applies compatibility shims to many old games (this one included). During process
initialisation, before *any* DLL's `DllMain` has run, the shim engine (`AcLayers.dll`) loads `d3d9.dll`
from the process directory — i.e. the proxy — and calls its `Direct3D9SetMaximizedWindowedModeShim`
export (and possibly the others above) directly. At that moment:

* the proxy's `DllMain` has not run, so nothing can be initialised there;
* `LoadLibrary` fails with error 1168 (`ERROR_NOT_FOUND`) — the loader is not ready for nested loads;
* anything the compiler's runtime does before `DllMain` (TLS callbacks, CRT initialisation, C++
  constructors, pthread-based TLS in the MinGW *posix* toolchain) runs in this hostile context or, worse,
  is skipped, leaving the export to run on uninitialised runtime state.

Forwarding those exports through a thunk to a not-yet-loaded library therefore jumped to address 0.
The fix, following what ReShade does: implement the shim exports natively. They record the call
(`g_shim_pending`, `g_shim_unknown`, `g_shim_mode`) and return; the first *normal* call
(`Direct3DCreate9`, or any thunk) loads the real library and replays the recorded call into its
ordinal 17. Everything else the proxy does before `DllMain` is limited to `kernel32` calls on
zero-initialised statics (`SRWLOCK_INIT`, lazy log file, lazy config). To make that hold, the DLL is
built:

* in plain C (no C++ runtime, no static constructors);
* with the MinGW-w64 **win32** thread-model toolchain (the *posix* one drags in winpthread and a TLS
  directory);
* with `-nostartfiles -Wl,-e,_DllMain@12`: no CRT start-up code at all, the loader calls `DllMain`
  directly; `-Wl,--disable-runtime-pseudo-reloc --disable-auto-import` so nothing needs fix-ups at
  start-up.

The first call's origin is logged (`call into d3d9.dll from SpaceGiraffePC.exe+0x2efb (DllMain has run
yet)` in a normal Steam launch — the shim engine's early call does not happen on every system, so the
log line says which case you got).

### Loading order with ReShade

`EnsureRealFrom()` (first normal call, under `g_lock`): load `<game dir>\<reshade>` if the file exists,
then `System32\d3d9.dll` by full path (never by bare name — that would resolve to the proxy itself),
resolve the thunk pointers, replay the shim call. ReShade installed as `d3d9.dll` hooks the real
library's exports when that library is loaded *after* it ("Installing delayed hooks for
'C:\Windows\system32\d3d9.dll'"), which is what happens here, so the game's device is a ReShade device
and SGFix's vtable hooks sit on top of it. `ReShade.log` shows it: `Registering hooks for
'C:\Windows\system32\d3d9.dll' … Redirecting Direct3DCreate9(SDKVersion = 0x20)`.

### Hooks

COM vtable entries are replaced in place (`VirtualProtect` + pointer swap) on the first object of each
type, which affects every object sharing that vtable — i.e. all of them.

`IDirect3D9`: 6 `GetAdapterModeCount`, 7 `EnumAdapterModes`, 16 `CreateDevice`.

`IDirect3DDevice9`: 16 `Reset`, 17 `Present`, 23 `CreateTexture`, 28 `CreateRenderTarget`,
29 `CreateDepthStencilSurface`, 34 `StretchRect`, 37 `SetRenderTarget`, 43 `Clear`, 47 `SetViewport`,
65 `SetTexture`, 69 `SetSamplerState`, 75 `SetScissorRect`, 81 `DrawPrimitive`,
82 `DrawIndexedPrimitive`, 83 `DrawPrimitiveUP`, 84 `DrawIndexedPrimitiveUP`, 87 `SetVertexDeclaration`,
89 `SetFVF`, 92 `SetVertexShader`, 107 `SetPixelShader`.

Mode lists are built once per (adapter, format) and cached (`GetList`): every real mode is enumerated
and logged, size-filtered (`min_width`/`min_height`), sorted by (width, height, preference rank of the
refresh rate, original order) and, unless `keep_others=1`, reduced to the first entry per resolution.
Rates not in `refresh=` rank after all listed ones, highest first.

`CreateDevice` / `Reset` log the presentation parameters (that is where the real refresh rate shows)
and apply the experimental `force_refresh` / `present_interval` overrides.

### Game debug output

The exe imports `OutputDebugStringA` from `kernel32.dll` and reports its video-mode decisions through
it. `HookGameDebugOutput` walks the exe's import directory and redirects every IAT slot for that name
to a function that appends the text to `sgfix.log` (`[game] …`) and calls the original. This is a
patch of the exe's *import table in memory*, not of its code, and is off with `capture_debug=0`.

## The log

`sgfix.log` (next to the DLL, overwritten each run, `log=1`):

```
SGFix 1.2.0 (d3d9.dll proxy) DllMain: attached to F:\...\SpaceGiraffePC.exe, dll at 70670000
config: refresh preference [60,120,144,100,59,50] keep_others=0 min 0x0 force_refresh=0 present_interval=-1 hooks=1 capture_debug=1
call into d3d9.dll from SpaceGiraffePC.exe+0x2efb (DllMain has run yet)
ReShade: F:\...\ReShade32.dll loaded (it hooks the real d3d9.dll loaded next)
real d3d9.dll loaded from C:\Windows\system32\d3d9.dll at 6C710000
replaying Direct3D9SetMaximizedWindowedModeShim(0, 0) recorded during start-up
game debug output captured (OutputDebugStringA import redirected)
Direct3DCreate9(sdk 32) -> 028280A8
IDirect3D9 vtable hooked
desktop mode: 3840x2160 @60 Hz format 22
   real  640x480 @24 Hz
   …
adapter 0 format 22: N modes from Direct3D, N after size filter
adapter 0 format 22: M modes handed to the game (preferred refresh first per resolution)
   game  640x480 @60 Hz
   …
CreateDevice: 640x480 format 22, windowed=1, refresh=0 Hz, …        (the game always starts windowed)
IDirect3DDevice9 vtable hooked (render logging on; F9 dumps the next 2 frames; rt_scale=4)
CreateTexture 2048x2048 levels=1 usage=0x1 RENDERTARGET A8R8G8B8 pool=0 -> 0 (…)
Reset: 3840x2160 format 22, windowed=0, refresh=60 Hz, …           (the mode you picked)
stats: frame 214199, last frame had 39 draws, 27 clears, 52 RT sets, 0 StretchRects
```

A frame dump lists every draw with the render target, vertex format (`XYZRHW` / `fvf` / `vdecl`),
shaders in use, texture 0 and its filters, plus every clear, viewport, scissor and render-target switch.

## Testing without the game

The proxy was exercised under 32-bit Wine (`WINEDLLOVERRIDES="d3d9=n,b"`, Xvfb + llvmpipe) with small
test programs that reproduce what matters: mode enumeration through the proxy, the assembly thunks, the
shim exports being called before `DllMain`, the IAT redirection, and a D3D9 program that renders through
512×512 render targets with `XYZRHW` quads the way the game does, run with and without scaling. Real-hardware validation: RTX 5090 + LG G3, 3840×2160 @ 60 Hz, ReShade 6.8.
