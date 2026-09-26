# Space Giraffe (PC) - SGFix: 60/120/144 Hz and a sharp picture at 4K

Two problems with Llamasoft's 2009 PC port of Space Giraffe on a modern machine, fixed by one small
'd3d9.dll' proxy that sits between the game and Direct3D 9:

1. **The video-mode menu offers only 50 Hz** for every resolution on many modern displays (4K TVs and
High DPI monitors over HDMI in particular). SGFix hands the game a mode list with your preferred refresh
rate - 60 Hz by default - as the *only* rate for each resolution, so that is what the menu shows and
what the game runs at. The menu may still show @50hz at 4k as the only option after the mod is installed,
but any framerate counter will show you that the game is now running at 60 fps after installing the patch.
2. **The picture is soft, even at 4K.** The game renders its whole scene into small offscreen textures
(512×512 at most, whatever the screen mode) and stretches the result to the screen. SGFix enlarges
those render targets (×4 by default → 2048×2048) and scales the game's screen-space geometry to
match, so the scene is actually drawn at a useful resolution.


It can also chain-load **ReShade** so you can add Adaptive CAS Sharpening and other effects on top, and it writes a
'sgfix.log' that shows exactly what the game asked for and what it got, for debugging purposes.

The game executable is **not** modified - it is wrapped in Steam's 2009-era DRM stub and cannot be
patched on disk anyway - and no game files are redistributed here.

## Install

1. Download the release zip and copy **'d3d9.dll'** and **'sgfix.ini'** into the game folder, next to
'SpaceGiraffePC.exe' ('…\Steam\steamapps\common\Space Giraffe').
2. Start the game from Steam (the DRM stub refuses to start it any other way), open the video
options and pick your resolution - it now comes with the preferred refresh rate (60 Hz by default).
3. Optional: edit 'sgfix.ini' to taste (see below).


Uninstall: delete 'd3d9.dll', 'sgfix.ini' and 'sgfix.log' from the game folder. Steam's *Verify
integrity of game files* leaves the three files alone (they are not part of the depot), so the fix
survives it.

### Refresh rate: use 60 Hz

Space Giraffe is frame-locked: the game logic advances once per displayed frame, so the refresh rate
sets the game speed. 60 Hz is correct; 120 Hz runs the game at double speed and 50 Hz at 5/6 speed (that
is what everybody on a 4K TV has been playing at in most cases). 'refresh=60,…' is therefore the default. If you want
to try 120 or 144 Hz anyway, put that rate first in 'sgfix.ini' ('refresh=120,144,60,…') - it works, it
is just fast.


PLEASE NOTE: The menu label may still say "3840X2160@50Hz" for the selected resolution mode: that text comes from the game's saved
config ('screen_refresh' in its config file), not from the mode it actually uses. The 'Reset:' line in
'sgfix.log' shows the real mode ('refresh=60 Hz'), as does your display's own info panel and any FPS overlay like the Nvidia overlay.

### Sharpness: rt_scale

'rt_scale=4' (the default in this patch) enlarges the game's offscreen render targets four times in each direction.
The game keeps a dozen or so of these targets, so ×4 costs about 250 MB of video memory - nothing for a
current GPU. 'rt_scale=8' (4096×4096, about 1 GB) also works if you want to supersample further.
'rt_scale=1' turns it off. If anything ever looks wrong in the picture with scaling on,
set 'rt_scale=1' and please open an issue with 'sgfix.log' attached.

### ReShade (optional)

ReShade must **NOT** be installed as 'd3d9.dll' (that spot is taken). Instead:

1. Get ReShade from [reshade.me](https://reshade.me/), run the installer against 'SpaceGiraffePC.exe'
with the **DirectX 9** API as usual (choose whatever effects you like - LumaSharpen, CAS, FakeHDR
and the CRT shaders suit this game), then in the game folder **rename the ReShade 'd3d9.dll' it
installed to 'ReShade32.dll'** and put SGFix's 'd3d9.dll' in its place.
(Or open the ReShade setup exe with 7-Zip and take 'ReShade32.dll' out of it directly.)
2. That's it: SGFix loads 'ReShade32.dll' before the real Direct3D library, and ReShade hooks the
game's device just as if it had been injected. 'reshade=' in 'sgfix.ini' names the file
(empty = never load it). Press **Home** in-game for the ReShade overlay.

'extras/ReShadePreset.ini' is the preset used while testing (ReShade 6.8: LumaSharpen + CAS + FakeHDR,
near-default settings).

## sgfix.ini reference

|key|default||
|-|-|-|
|'refresh'|'60,120,144,100,59,50'|Refresh rates in order of preference. Each resolution is handed the first rate in this list that the display supports.|
|'keep_others'|'0'|'1' = also pass the display's other rates after the preferred one. Leave at 0: the game sorts the rates itself and takes the lowest one ≥ 50 Hz, so it must only see one.|
|'min_width', 'min_height'|'0'|Drop modes smaller than this from the list (0 = keep all).|
|'rt_scale'|'4'|Render-target upscale factor (1 = off).|
|'rt_max'|'1024'|Only targets up to this size (per side) are enlarged; the game's are 512 or less.|
|'reshade'|'ReShade32.dll'|Renamed ReShade DLL to chain-load if present in the game folder (empty = never).|
|'log'|'1'|Write 'sgfix.log' next to the DLL: the real and adjusted mode lists, device creation/reset parameters, render-target creation.|
|'capture_debug'|'1'|Copy the game's own debug output ('[game] …' lines: its video-mode decisions) into 'sgfix.log'.|
|'log_textures'|'1'|Log render-target / large texture creation (capped at 400 lines).|
|'dump_frame', 'dump_count'|'0', '2'|Write a full description of 'dump_count' frames (every draw, clear, render-target switch, texture and filter in use) starting at frame 'dump_frame' (0 = never). **F9** in-game dumps the next 'dump_count' frames on demand.|
|'force_refresh'|'0'|Experimental: force the fullscreen refresh rate the game asks for (0 = off).|
|'present_interval'|'-1'|Experimental: '0' = no vsync, '1' = vsync, '-1' = leave the game's choice.|
|'hooks'|'1'|'0' = hook nothing, just forward every call to the real 'd3d9.dll' (for troubleshooting).|

## How it works

Windows loads a 'd3d9.dll' from the game's own folder in preference to the one in 'System32', so
SGFix gets every Direct3D 9 call the game makes. It loads the real 'C:\Windows\System32\d3d9.dll',
forwards everything to it, and replaces a handful of entries in the COM vtables of the 'IDirect3D9' and
'IDirect3DDevice9' objects the real library hands back:


* **Mode list** - 'GetAdapterModeCount' / 'EnumAdapterModes' return a rebuilt list: the real modes,
grouped by resolution in Direct3D's order, with the refresh rates re-ordered by your preference and
(by default) reduced to one per resolution. The game's menu code keeps, for each resolution, the
*lowest* refresh rate ≥ 50 Hz it finds in the list - on a 4K TV that is 50 Hz, whatever order the
rates come in - so the only way to make it use 60 Hz is to make 60 Hz the only rate it sees.
* **Render targets** - 'CreateTexture' for a 'D3DUSAGE_RENDERTARGET' texture ≤ 'rt_max' is created
'rt_scale' times larger. The game never sets viewports or scissors for these targets and samples them
with 0-1 texture coordinates, so most of the pipeline scales for free. The exception is its
screen-space geometry: full-screen quads and sprites drawn with pre-transformed ('XYZRHW')
vertices, whose positions are in target pixels. SGFix scales the x/y of those vertices on the fly
('DrawPrimitiveUP' / 'DrawIndexedPrimitiveUP'), along with scissor and clear rectangles, whenever the
current render target is one it enlarged. The final quad that stretches the last target to the back
buffer is unchanged - it just has 16× the pixels to sample from.
* **ReShade** - the renamed DLL is loaded *before* the real 'd3d9.dll', so ReShade's own hooks catch
the real library's exports when it loads next; the game sees ReShade's device, and SGFix's vtable
hooks sit on top of that.
* **Start-up safety** - Windows' application-compatibility layer calls a few undocumented 'd3d9.dll'
exports ('Direct3D9EnableMaximizedWindowedModeShim' and friends) *before* any DLL's 'DllMain' has run,
at a point where 'LoadLibrary' is not usable yet. SGFix implements those natively (recording the call
and replaying it into the real library later), is linked without the C runtime start-up code or a TLS
directory, and loads the real 'd3d9.dll' lazily on the first normal call. That is what makes the
proxy work inside the DRM-wrapped process - an earlier version that loaded eagerly crashed the game
at start-up with '0xc0000005'.


Details, vtable indices and the reasoning behind each hook: [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Limitations

* Windows only. Under Proton/Wine the proxy would need a 'WINEDLLOVERRIDES="d3d9=n,b"' override and
has not been tested there.
* Pre-transformed geometry drawn from a *vertex buffer* into an enlarged target cannot be scaled (the
data is on the GPU). The game does not do this as far as testing shows; if it ever does, SGFix logs a
warning once and the affected element would appear at the wrong size - 'rt_scale=1' is the fallback.
* The "@50Hz" text in the video menu is cosmetic (see above).
* 120/144 Hz double the game speed - a game limitation, not something the proxy can fix.

## Building

Cross-compiled from Linux with MinGW-w64, 32-bit, win32 thread model:

    apt install gcc-mingw-w64-i686-win32
    ./build.sh            # -> build/d3d9.dll

Plain C99, no C runtime start-up ('-nostartfiles', entry point 'DllMain'), no dependencies beyond
'kernel32'/'user32'. The build is reproducible (no timestamp, fixed image base): release 1.2.0 is
'build/d3d9.dll', 43,169 bytes, SHA-256
'56d72d1d44edfdd14654c56a10a53cef087c565f7b9fc2a80922f2604a60ae8b'.

## Credits

* Reverse engineering and the proxy done using Claude Fable (Anthropic).
* crosire's [ReShade](https://reshade.me/) - and its handling of the app-compat shim exports, which
showed the way out of the start-up crash.

## License

MIT - see [LICENSE](LICENSE). Space Giraffe is © Llamasoft; this project contains no game files or code.

