#!/bin/sh
# Build the browser emulator: the engine and controls to WebAssembly, embedded in one page.
# usage: sh firmware/web/build.sh   ->  firmware/web/build/glag-emulator.html
set -e
cd "$(dirname "$0")"
mkdir -p build
clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin -Iinclude -I../glag -Wall -Wextra \
	-Wl,--no-entry -Wl,--export-dynamic -Wl,--strip-all -o build/glag.wasm \
	glag_wasm.c ../glag/glag_engine.c ../glag/glag_ui.c
python3 -c "
import base64, sys
page = open('emulator.html').read()
wasm = base64.b64encode(open('build/glag.wasm', 'rb').read()).decode()
open('build/glag-emulator.html', 'w').write(page.replace('@@WASM@@', wasm))
"
echo "build/glag-emulator.html ($(wc -c < build/glag-emulator.html) bytes)"
