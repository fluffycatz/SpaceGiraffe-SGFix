# Changelog

## 1.2.0 - 2026-09-25

* Render-target upscale ('rt_scale', default 4): the game's 512×512 offscreen targets are created
  2048×2048; pre-transformed ('XYZRHW') user-memory draws, scissor and clear rectangles are scaled to
  match while such a target is bound. The picture at 4K is finally sharp.
* Vertex-format tracking ('SetFVF' / 'SetVertexDeclaration' / shaders) in the frame dump.
* Reproducible build (no PE timestamp, fixed image base).

## 1.1.0

* Frame-structure logging: render-target creation, per-frame dumps ('dump_frame', **F9**), stats line
  every 600 frames.
* ReShade chain-loading ('reshade=ReShade32.dll'): a renamed ReShade next to the game is loaded before
  the real 'd3d9.dll'.
* 'keep_others=0' is now the default - the game takes the lowest rate ≥ 50 Hz it is offered, so each
  resolution is handed exactly one refresh rate. This is what actually makes the game run at 60 Hz.
* The game's own debug output ('OutputDebugStringA') is captured into 'sgfix.log'.

## 1.0.4

* Native implementations of the 'Direct3D9*Shim' exports (ordinals 16-23): the Windows AppCompat
  shim engine calls them before 'DllMain'; the call is recorded and replayed into the real 'd3d9.dll'
  once it is loaded. Fixes the start-up crash ('0xc0000005' in 'ntdll.dll') under Steam.

## 1.0.3

* Rewritten in plain C, linked without CRT start-up code or a TLS directory; every export is safe to
  call before 'DllMain'; the first caller into the DLL is logged.

## 1.0.1

* Logging from the first line of 'DllMain' using Win32 only; the real 'd3d9.dll' is loaded lazily;
  'hooks=0' troubleshooting switch.

## 1.0.0

* First version: 'd3d9.dll' proxy putting the preferred refresh rate first for every resolution.
