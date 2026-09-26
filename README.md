# Space Giraffe (PC) refresh-rate fix — SGFix

Space Giraffe's video-mode menu offers only 50 Hz for every resolution on many modern displays
(4K TVs and monitors over HDMI in particular). The game builds that menu from Direct3D 9's mode
list and keeps only the **first** entry it sees for each resolution; Direct3D lists refresh rates
in ascending order and the game skips rates below 50 Hz, so 50 Hz wins everywhere. Your display's
capabilities are read fine — the game just never looks past the first candidate.

SGFix is a small `d3d9.dll` proxy that sits between the game and Direct3D and hands the game the
same mode list with your preferred refresh rate first for each resolution. The game executable
is not modified (it is wrapped in Steam's 2009 DRM, so it cannot be patched on disk anyway).

## Install

1. Copy `d3d9.dll` and `sgfix.ini` into the game folder, next to `SpaceGiraffePC.exe`
   (`Steam\steamapps\common\Space Giraffe\`).
2. Start the game, open the video options and pick your resolution — it now shows the
   preferred refresh rate (60 Hz by default).
3. To use 120 or 144 Hz, edit `sgfix.ini` and put that rate first: `refresh=120,144,60,100,50`.

Uninstall: delete `d3d9.dll`, `sgfix.ini` and `sgfix.log` from the game folder.

`sgfix.log` records the mode list Direct3D reported, the list handed to the game, and every
device creation / reset with the mode the game asked for — attach it if something looks wrong.

## Building

32-bit MinGW-w64 cross build (`apt install g++-mingw-w64-i686-posix`), then `./build.sh`.

## License

MIT. Space Giraffe is © Llamasoft; nothing of it is included here.
