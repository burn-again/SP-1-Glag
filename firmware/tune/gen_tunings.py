# Writes the Kit's tunings (tunings.py, over params.py's defaults) into glag_kit.h, between its
# "@@KIT_TUNINGS" markers. Run after editing either; tune/build.sh runs it too.
import re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from params import P
from tunings import TUNINGS, GLAG_MACRO, MAP, MAP_GAIN
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
rows = [[float(p[3]) for p in P]] + [parse(t[1]) for t in TUNINGS]
names = ['Glag'] + [t[0] for t in TUNINGS]
def target(ctx):   # 'a1.00|t0.00|...' -> [a, t, l, r, c]; None -> -1s (busier, darker, longer, fuller)
    if not ctx:
        return [-1.0] * 5
    kv = {x[0]: float(x[1:]) for x in ctx.split('|')}
    return [kv['a'], kv['t'], kv['l'], kv['r'], kv['c']]
targets = [target(GLAG_MACRO)] + [target(t[2] if len(t) > 2 else None) for t in TUNINGS]
c = '/* @@KIT_TUNINGS (written by tune/gen_tunings.py from tune/tunings.py: edit those) */\n'
c += '#define KIT_NTUNE %d\n' % len(rows)
c += 'static const char *const KIT_TUNE_NAME[KIT_NTUNE] = {%s};\n' % ', '.join('"%s"' % n for n in names)
c += 'static const float KIT_TUNE[KIT_NTUNE][KT_COUNT] = {\n'
for n, r in zip(names, rows):
    c += '\t{ /* %s */\n\t\t%s},\n' % (n, ', '.join(('%gf' % x) if '.' in ('%g' % x) or 'e' in ('%g' % x) else ('%g.0f' % x) for x in r))
c += '};\n/* where the macro takes the Kit channel at each tuning: activity, tone, length, register,\n * character (-1: busier, darker, longer, fuller than the piece has it) */\n'
c += 'static const float KIT_TUNE_TO[KIT_NTUNE][5] = {%s};\n' % ', '.join('{%s}' % ', '.join('%gf' % x if '.' in '%g' % x else '%g.0f' % x for x in t) for t in targets)
idx = {n: i for i, n in enumerate(names)}
c += '/* the Kit map\'s corners (tunings): (0,0) (1,0) (0,1) (1,1), and each one\'s level */\n'
c += 'static const uint8_t KIT_MAP[4] = {%s};\n' % ', '.join(str(idx[n]) for n in MAP)
c += 'static const float KIT_MAP_GAIN[4] = {%s};\n' % ', '.join('%gf' % g if '.' in '%g' % g else '%g.0f' % g for g in MAP_GAIN)
c += '/* which numbers the map blends (not the evolving, velocity and arc ones), which it switches */\n'
c += 'static const uint8_t KT_MAPPED[KT_COUNT] = {%s};\n' % ', '.join('0' if p[1] in ('Evolve', 'Velocity', 'Arc') else ('2' if p[6] >= 1 else '1') for p in P)
c += '/* @@END_KIT_TUNINGS */\n'
path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../glag/glag_kit.h')
s = open(path).read()
a = s.find('/* @@KIT_TUNINGS'); b = s.find('/* @@END_KIT_TUNINGS */')
if a < 0:
    m = s.index('void gl_kit_tune(int id')
    s = s[:m] + c + s[m:]
else:
    s = s[:a] + c + s[b + len('/* @@END_KIT_TUNINGS */\n'):]
# and the numbers' table itself, at params.py's defaults
kt = '/* ---- the tuning: every number the Kit tuning page reaches (gl_kit_tune), at the firmware\'s\n * defaults (written by tune/gen_tunings.py from tune/params.py: edit that) ---- */\nenum {\n'
kt += ''.join('\tKT_%s,\n' % p[0] for p in P) + '\tKT_COUNT\n};\nstatic float KT[KT_COUNT] = {\n'
kt += ''.join('\t%sf,   /* %s: %s %s */\n' % (repr(float(p[3])), p[0], p[1], p[2]) for p in P) + '};\n'
a = s.index('/* ---- the tuning: every number'); b = s.index('/* @@KIT_TUNINGS')
s = s[:a] + kt + s[b:]
open(path, 'w').write(s)
print('wrote %d tunings' % len(rows))
