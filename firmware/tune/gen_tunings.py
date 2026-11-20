# Writes the Kit's tunings (tunings.py, over params.py's defaults) into glag_kit.h, between its
# "@@KIT_TUNINGS" markers. Run after editing either; tune/build.sh runs it too.
import re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from params import P
from tunings import TUNINGS
ids = [p[0] for p in P]
def parse(code):
    v = [float(p[3]) for p in P]
    for part in code.split(';')[1:]:
        if part.startswith('@') or '=' not in part:
            continue
        k, x = part.split('=')
        if k not in ids:
            raise SystemExit('unknown id %s' % k)
        i = ids.index(k)
        v[i] = min(P[i][5], max(P[i][4], float(x)))
    return v
rows = [[float(p[3]) for p in P]] + [parse(c) for _, c in TUNINGS]
names = ['Firmware'] + [n for n, _ in TUNINGS]
c = '/* @@KIT_TUNINGS (written by tune/gen_tunings.py from tune/tunings.py: edit those) */\n'
c += '#define KIT_NTUNE %d\n' % len(rows)
c += 'static const char *const KIT_TUNE_NAME[KIT_NTUNE] = {%s};\n' % ', '.join('"%s"' % n for n in names)
c += 'static const float KIT_TUNE[KIT_NTUNE][KT_COUNT] = {\n'
for n, r in zip(names, rows):
    c += '\t{ /* %s */\n\t\t%s},\n' % (n, ', '.join(('%gf' % x) if '.' in ('%g' % x) or 'e' in ('%g' % x) else ('%g.0f' % x) for x in r))
c += '};\n/* @@END_KIT_TUNINGS */\n'
path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../glag/glag_kit.h')
s = open(path).read()
a = s.find('/* @@KIT_TUNINGS'); b = s.find('/* @@END_KIT_TUNINGS */')
if a < 0:
    m = s.index('void gl_kit_tune(int id')
    s = s[:m] + c + s[m:]
else:
    s = s[:a] + c + s[b + len('/* @@END_KIT_TUNINGS */\n'):]
open(path, 'w').write(s)
print('wrote %d tunings' % len(rows))
