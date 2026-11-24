#!/bin/sh
# Glag Lab: the engine alone (no SP-1 controls) to WebAssembly, for lab.html (the light voices,
# the audition pieces and the performance effects). The page fetches glag_lab.wasm beside it.
# usage: sh firmware/web/build_lab.sh   ->  firmware/web/build/glag_lab.wasm (copy it next to lab.html)
set -e
cd "$(dirname "$0")"
mkdir -p build
clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin -Iinclude -I../glag -Wall -Wextra -DGL_DEMO_PIECES \
	-Wl,--no-entry -Wl,--export-dynamic -Wl,--strip-all -o build/glag_lab.wasm glag_lab.c ../glag/glag_engine.c
echo "build/glag_lab.wasm: $(wc -c < build/glag_lab.wasm) bytes"
