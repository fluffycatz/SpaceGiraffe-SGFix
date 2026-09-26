#!/bin/sh
# 32-bit d3d9.dll proxy, plain C, MinGW-w64 win32 thread model (apt install gcc-mingw-w64-i686-win32).
# Linked without the MinGW CRT start-up files: the loader calls DllMain directly, there is no TLS
# directory and nothing runs before DllMain except what the exports themselves do.
set -e
CC=${CC:-i686-w64-mingw32-gcc-win32}; OUT=${OUT:-build}
mkdir -p "$OUT"
$CC -std=c99 -O2 -Wall -DUNICODE -D_UNICODE -fno-asynchronous-unwind-tables -c src/sgfix.c -o "$OUT/sgfix.o"
$CC -c src/thunks.S -o "$OUT/thunks.o"
$CC -shared -nostartfiles -o "$OUT/d3d9.dll" "$OUT/sgfix.o" "$OUT/thunks.o" src/d3d9.def \
    -static-libgcc -luser32 -Wl,-e,_DllMain@12 -Wl,--enable-stdcall-fixup -Wl,--exclude-all-symbols -Wl,--disable-runtime-pseudo-reloc -Wl,--disable-auto-import -Wl,--no-insert-timestamp -Wl,--image-base,0x10000000
echo "built $OUT/d3d9.dll"
