#!/bin/sh
# Builds RECraft's winmm.dll with mingw-w64 (i686). Needs MinHook's objects (buffer.o hook.o trampoline.o hde32.o)
# built from https://github.com/TsudaKageyu/minhook and its include folder at ../minhook/include.
set -e
i686-w64-mingw32-windres version.rc -O coff -o version.o
i686-w64-mingw32-g++ -O2 -Wall -Wno-misleading-indentation -shared -static -static-libgcc -static-libstdc++ -I../minhook/include \
  -o winmm.dll recraft.cpp bridge.cpp input.cpp render.cpp collision.cpp combat.cpp blocks.cpp names.cpp items.cpp winmm_stubs.S \
  buffer.o hook.o trampoline.o hde32.o version.o exports.def -ld3d9 -ldxguid -Wl,--enable-stdcall-fixup -Wl,--dynamicbase,--nxcompat
