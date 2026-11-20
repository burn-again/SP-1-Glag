#!/bin/sh
# Build the Kit tuning page: the engine to WebAssembly, the Kit's numbers (params.py) and pieces in.
# usage: sh firmware/tune/build.sh   ->  firmware/tune/build/glag-kit-tuner.html
set -e
cd "$(dirname "$0")"
mkdir -p build
# the Kit at each rate it can run at (12, 16, 24 kHz: KIT_KSUB), so the page can switch between them
for k in 12 16 24; do
	clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-builtin -I../web/include -I../glag -Wall -Wextra -DGL_DEMO_PIECES -DKIT_KSUB=$k \
		-Wl,--no-entry -Wl,--export-dynamic -Wl,--strip-all -o build/kit$k.wasm kit_wasm.c ../glag/glag_engine.c
done
python3 -B gen_tunings.py
python3 -B - <<'PY'
import base64, json, re
from params import P
from tunings import TUNINGS
src = open('../glag/glag_engine.c').read()
recs = re.findall(r'\{"([^"]+)", \{(GV_\w+)', src[src.index('static const recipe PIECES'):])
pieces = [[i, n] for i, (n, v) in enumerate(recs) if v == 'GV_KIT']
# the page's table and the firmware's must agree
kit = open('../glag/glag_kit.h').read()
ids = re.findall(r'\tKT_(\w+),', kit[kit.index('enum {\n\tKT_'):kit.index('KT_COUNT')])
assert ids == [p[0] for p in P], 'params.py and glag_kit.h differ'
page = open('tuner.html').read()
page = page.replace('@@WASM@@', json.dumps({k: base64.b64encode(open('build/kit%s.wasm' % k, 'rb').read()).decode() for k in ('12', '16', '24')}))
page = page.replace('@@PARAMS@@', json.dumps([list(p) for p in P]))
page = page.replace('@@PIECES@@', json.dumps(pieces))
page = page.replace('@@TUNINGS@@', json.dumps([['Firmware', 'GK1']] + [[n, c] for n, c in TUNINGS]))
open('build/glag-kit-tuner.html', 'w').write(page)
print('pieces', pieces)
PY
echo "build/glag-kit-tuner.html ($(wc -c < build/glag-kit-tuner.html) bytes)"
