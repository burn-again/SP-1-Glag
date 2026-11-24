#!/usr/bin/env python3
"""Estimate the Glag engine's cost on the SP-1's nRF52840 (Cortex-M4F, 64 MHz).

Runs harness.elf (the engine cross-compiled for the real CPU) under the Unicorn emulator and
weighs every executed instruction by its Cortex-M4 cycle count (TRM figures: loads and
stores 2, VMLA/VFMA 3, VDIV/VSQRT 14, branches that leave a block 2-4). It does not model the
flash cache, wait states or interrupts: treat the result as a floor and keep ~25 % margin.

usage: cpu_count.py harness.elf [VOICES ...]    e.g.  0127  0000  5555
       cpu_count.py harness.elf TOOLPREFIX --piece 70 --blocks 15000 --worst 10   (a piece as it plays; what ran in its worst blocks)
       (pieces past 15 need the harness built with -DGL_DEMO_PIECES)
"""
import subprocess, sys, struct, functools
from unicorn import *
from unicorn.arm_const import *
import capstone as cs

ELF = sys.argv[1]
TOOL = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2].endswith('-') else None
# the first four characters are the voices (base 36, 0-n); after them, m<mask> and d<dist>
_vals = {i + 1 for i, x in enumerate(sys.argv) if x in ('--piece', '--blocks', '--worst')}
configs = [a for i, a in enumerate(sys.argv[2:], 2) if i not in _vals and len(a) >= 4 and all(ch in '0123456789abcdefghijklmno' for ch in a[:4]) and all(ch in '0123456789md' for ch in a[4:])] or ['0127']
NM = TOOL + 'nm' if TOOL else 'arm-zephyr-eabi-nm'
OBJCOPY = TOOL + 'objcopy' if TOOL else 'arm-zephyr-eabi-objcopy'

syms = {}
for line in subprocess.check_output([NM, ELF], text=True).splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)
img = subprocess.check_output([OBJCOPY, '-O', 'binary', '-j', '.text', '-j', '.data', ELF, '/dev/stdout'])

md = cs.Cs(cs.CS_ARCH_ARM, cs.CS_MODE_THUMB | cs.CS_MODE_MCLASS)
md.detail = False

def weight(mn, op):
    m = mn.split('.')[0]
    if m in ('vdiv', 'vsqrt'): return 14
    if m in ('sdiv', 'udiv'): return 7
    if m in ('vmla', 'vmls', 'vfma', 'vfms', 'vnmla', 'vnmls', 'vfnma', 'vfnms'): return 3
    if m in ('ldm', 'ldmia', 'ldmdb', 'pop', 'stm', 'stmia', 'stmdb', 'push', 'vpush', 'vpop', 'vldm', 'vstm', 'vldmia', 'vstmia', 'vstmdb'):
        n = op.count(',') + 1
        return 1 + n + (3 if 'pc' in op else 0)
    if m.startswith(('ldr', 'str', 'vldr', 'vstr')): return 2
    if m in ('bl', 'blx'): return 4
    if m in ('bx',): return 3
    return 1

@functools.lru_cache(maxsize=None)
def block_cost(addr, size):
    code = bytes(mem.mem_read(addr, size))
    c = 0
    last = None
    for ins in md.disasm(code, addr):
        c += weight(ins.mnemonic, ins.op_str)
        last = ins.mnemonic
    if last and last.split('.')[0] in ('b', 'beq', 'bne', 'bgt', 'blt', 'bge', 'ble', 'bhi', 'bls', 'bcs', 'bcc', 'bmi', 'bpl', 'cbz', 'cbnz', 'bhs', 'blo', 'bvs', 'bvc'):
        c += 2   # a branch that ends a block is usually taken: pipeline refill
    return c

def run(voices):
    global mem
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    mem = mu
    mu.mem_map(0x0, 1 << 20)
    mu.mem_map(0x20000000, 256 << 10)
    mu.mem_map(0xE0000000, 1 << 20)
    mu.mem_write(0, img[: 1 << 20])
    # cfg_* live in .data: patch their load image in flash, which reset() copies to RAM
    lma = lambda a: syms['_sidata'] + (a - syms['_sdata'])
    dist = None
    voices, opt = voices[:4], voices[4:]
    if 'd' in opt:                         # 0127d241: distortion type 2, amount 4, after the effects
        opt, dd = opt.split('d')
        dist = int(dd[0]) | int(dd[1]) << 4 | int(dd[2]) << 8
    mute = int(opt.split('m')[1]) if 'm' in opt else 0
    mu.mem_write(lma(syms['cfg_voices']), bytes(int(ch, 36) for ch in voices))
    mu.mem_write(lma(syms['cfg_mute']), bytes([mute | 0x80]))
    if dist is not None and 'cfg_dist' in syms:
        mu.mem_write(lma(syms['cfg_dist']), struct.pack('<H', dist))
    if 'cfg_stress' in syms:
        mu.mem_write(lma(syms['cfg_stress']), bytes([0x81 if STRESS else 0x80]))   # nonzero init keeps it in .data
    if PIECE is not None:
        mu.mem_write(lma(syms['cfg_piece']), bytes([PIECE]))
    nblk = BLOCKS or (1500 if STRESS else 300)
    mu.mem_write(lma(syms['cfg_blocks']), struct.pack('<I', nblk))
    # enable the FPU (CPACR: CP10, CP11 full access)
    mu.mem_write(0xE000ED88, struct.pack('<I', 0xF << 20))
    st = {'cyc': 0, 'start': 0, 'blocks': [], 'done': False, 'cur': {}, 'per': []}

    def on_block(uc, addr, size, _):
        c = block_cost(addr, size)
        st['cyc'] += c
        if PROF is not None:
            PROF[addr] = PROF.get(addr, 0) + c
        if WORST:
            st['cur'][addr] = st['cur'].get(addr, 0) + c

    def on_mark(uc, access, addr, size, value, _):
        if value == 1:
            st['start'] = st['cyc']
        elif value == 2:
            st['blocks'].append(st['cyc'] - st['start'])
            if WORST and '--avg-lines' in sys.argv and len(st['blocks']) > 50:
                al = st.setdefault('al', {})          # every block's lines, summed (averaged at the end)
                for a, c in st['cur'].items():
                    al[a] = al.get(a, 0) + c
                st['aln'] = st.get('aln', 0) + 1
            if WORST and '--worst-lines' in sys.argv:
                cost = st['blocks'][-1]
                keep = st.setdefault('wl', [])
                if len(st['blocks']) > 50 and (len(keep) < 3 or cost > keep[-1][0]):
                    keep.append((cost, len(st['blocks']) - 1, dict(st['cur'])))
                    keep.sort(key=lambda x: -x[0])
                    del keep[3:]
            if WORST:
                agg = {}
                for a, c in st['cur'].items():
                    f = func_of(a)
                    agg[f] = agg.get(f, 0) + c
                rd = lambda n: struct.unpack('<I', uc.mem_read(syms[n], 4))[0] if n in syms else 0
                st['per'].append((agg, rd('step_n'), rd('notes_fired'), rd('chord_count')))
                st['cur'] = {}
        elif value == 3:
            st['done'] = True
            uc.emu_stop()

    mu.hook_add(UC_HOOK_BLOCK, on_block)
    mu.hook_add(UC_HOOK_MEM_WRITE, on_mark, begin=syms['mark'], end=syms['mark'] + 3)
    sp, pc = struct.unpack('<II', img[:8])
    mu.reg_write(UC_ARM_REG_SP, sp)
    mu.emu_start(pc | 1, 0xFFFFFFFF, count=0)
    b = st['blocks'][50:]          # skip the first 0.1 s (fade-in, allocation)
    if STRESS:
        b = st['blocks'][500:]     # let every voice fill up first
    budget = 64e6 * 96 / 48000      # cycles per 2 ms block
    avg = sum(b) / len(b) / budget * 100
    worst = max(b) / budget * 100
    if WORST:
        skip = len(st['blocks']) - len(b)
        per = st['per'][skip:]
        mean = {}
        for agg, *_ in per:
            for f, c in agg.items():
                mean[f] = mean.get(f, 0) + c / len(per)
        order = sorted(range(len(b)), key=lambda i: -b[i])[:WORST]
        print(f"  typical block: {sum(b) / len(b) / budget * 100:5.1f} %")
        for rank, i in enumerate(order):
            agg, stp, nf, cc = per[i]
            pstp, pnf, pcc = (per[i - 1][1:] if i > 0 else (stp, nf, cc))
            print(f"  worst #{rank + 1}: {b[i] / budget * 100:5.1f} %  at {(i + skip) * 2 / 1000:6.2f} s  step {stp} (bar {stp // 16}, sixteenth {stp % 16})"
                  f"  notes started {nf - pnf}  chord changed {'yes' if cc != pcc else 'no'}")
            extra = sorted(((f, c - mean.get(f, 0)) for f, c in agg.items()), key=lambda x: -x[1])[:WORST_N]
            for f, d in extra:
                print(f"      {d / budget * 100:+6.1f} pts  {f:32s} ({agg[f] / budget * 100:5.1f} % here, {mean.get(f, 0) / budget * 100:5.1f} % typical)")
    if WORST and '--avg-lines' in sys.argv:
        only = sys.argv[sys.argv.index('--func') + 1] if '--func' in sys.argv else None
        d, n = st.get('al', {}), max(st.get('aln', 1), 1)
        addrs = sorted(a for a in d if only is None or func_of(a) == only)
        lines = []
        for i in range(0, len(addrs), 2000):
            lines += subprocess.check_output([TOOL + 'addr2line', '-e', ELF] + [hex(a) for a in addrs[i:i + 2000]], text=True).splitlines()
        agg = {}
        for a, l in zip(addrs, lines):
            k = l.split('/')[-1].split(' ')[0]
            agg[k] = agg.get(k, 0) + d[a] / n
        print(f"  lines, averaged over {n} blocks:")
        for k, c in sorted(agg.items(), key=lambda x: -x[1])[:30]:
            print(f"      {c / budget * 100:5.2f} pts  {k}")
    if WORST and '--worst-lines' in sys.argv:
        only = sys.argv[sys.argv.index('--func') + 1] if '--func' in sys.argv else None
        for cost, bi, d in st.get('wl', []):
            addrs = sorted(a for a in d if only is None or func_of(a) == only)
            lines = []
            for i in range(0, len(addrs), 2000):
                lines += subprocess.check_output([TOOL + 'addr2line', '-e', ELF] + [hex(a) for a in addrs[i:i + 2000]], text=True).splitlines()
            agg = {}
            for a, l in zip(addrs, lines):
                k = l.split('/')[-1].split(' ')[0]
                agg[k] = agg.get(k, 0) + d[a]
            print(f"  lines of the block at {bi * 2 / 1000:.2f} s ({cost / budget * 100:.1f} %):")
            for k, c in sorted(agg.items(), key=lambda x: -x[1])[:25]:
                print(f"      {c / budget * 100:5.2f} pts  {k}")
    return avg, worst

PROF = {} if '--prof' in sys.argv else None
def _opt(name, conv=int):
    return conv(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else None
PIECE = _opt('--piece')            # a piece as it plays (its own settings and macros), not the voices in piece 0
BLOCKS = _opt('--blocks')          # how many 2 ms blocks (default 300; 1500 under --stress)
WORST = _opt('--worst') or 0       # report the worst N blocks: what ran in them beyond a typical block
WORST_N = 8
_funcs = sorted((a, n) for n, a in syms.items() if a < 0x100000)
_fa = [f[0] for f in _funcs]
_fcache = {}
def func_of(a):
    if a not in _fcache:
        import bisect
        _fcache[a] = _funcs[bisect.bisect_right(_fa, a) - 1][1]
    return _fcache[a]
STRESS = '--stress' in sys.argv
names = ['Pad', 'Bells', 'Bass', 'Piano', 'Marimba', 'Drone', 'Kalimba', 'Flute', 'Arp',
         'Swell', 'EP', 'Gong', 'Pizz', 'Shimmer', 'Tails', 'Pulse', 'Strings', 'Glow', 'Seq', 'Super',
         'Ribbon', 'Funk', 'Harp', 'Harpsichord', 'Kit']
for v in configs:
    avg, worst = run(v)
    vv = v[:4]
    label = f"piece {PIECE}" if PIECE is not None else ' + '.join(names[int(c, 36)] for c in vv)
    print(f"{'' if PIECE is not None else v:8s} {label:40s} avg {avg:5.1f} %   worst block {worst:5.1f} %")

if PROF is not None:
    funcs = sorted((a, n) for n, a in syms.items() if a < 0x100000)
    import bisect
    agg = {}
    for a, c in PROF.items():
        i = bisect.bisect_right([f[0] for f in funcs], a) - 1
        agg[funcs[i][1]] = agg.get(funcs[i][1], 0) + c
    tot = sum(agg.values())
    for n, c in sorted(agg.items(), key=lambda x: -x[1])[:15]:
        print(f"{c/tot*100:5.1f} %  {n}")

if PROF is not None and '--lines' in sys.argv:
    tot = sum(PROF.values())
    top = sorted(PROF.items(), key=lambda x: -x[1])[:25]
    addrs = [hex(a) for a, _ in top]
    out = subprocess.check_output([TOOL + 'addr2line', '-e', ELF] + addrs, text=True).splitlines()
    for (a, c), l in zip(top, out):
        print(f"{c/tot*100:5.1f} %  {a:#x}  {l.split('/')[-1]}")

if PROF is not None and '--dump' in sys.argv:
    # every address's cycles, by source line (needs the harness built with -g): file:line cycles
    out_path = sys.argv[sys.argv.index('--dump') + 1]
    addrs = sorted(PROF)
    lines = []
    for i in range(0, len(addrs), 2000):
        chunk = addrs[i:i + 2000]
        lines += subprocess.check_output([TOOL + 'addr2line', '-e', ELF] + [hex(a) for a in chunk], text=True).splitlines()
    agg = {}
    for a, l in zip(addrs, lines):
        k = l.split('/')[-1].split(' ')[0]
        agg[k] = agg.get(k, 0) + PROF[a]
    with open(out_path, 'w') as f:
        for k, c in sorted(agg.items(), key=lambda x: -x[1]):
            f.write(f"{k} {c}\n")
