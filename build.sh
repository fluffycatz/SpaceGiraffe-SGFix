#!/bin/sh
# Cross-compiles the 32-bit d3d9.dll proxy with MinGW-w64 (apt install g++-mingw-w64-i686-posix).
set -e
CXX=${CXX:-i686-w64-mingw32-g++}; CC=${CC:-i686-w64-mingw32-gcc}; OUT=${OUT:-build}
mkdir -p "$OUT"
$CXX -std=c++17 -O2 -Wall -DUNICODE -D_UNICODE -c src/sgfix.cpp -o "$OUT/sgfix.o"
$CC -c src/thunks.S -o "$OUT/thunks.o"
$CXX -shared -o "$OUT/d3d9.dll" "$OUT/sgfix.o" "$OUT/thunks.o" src/d3d9.def -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup
echo "built $OUT/d3d9.dll"
