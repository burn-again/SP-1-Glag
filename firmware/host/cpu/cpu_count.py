#!/usr/bin/env python3
"""Estimate the Glag engine's cost on the SP-1's nRF52840 (Cortex-M4F, 64 MHz).

Runs harness.elf (the engine cross-compiled for the real CPU) under the Unicorn emulator and
weighs every executed instruction by its Cortex-M4 cycle count (TRM figures: loads and
stores 2, VMLA/VFMA 3, VDIV/VSQRT 14, branches that leave a block 2-4). It does not model the
flash cache, wait states or interrupts: treat the result as a floor and keep ~25 % margin.

usage: cpu_count.py harness.elf [VOICES ...]    e.g.  0127  0000  5555
"""
import subprocess, sys, struct, functools
from unicorn import *
from unicorn.arm_const import *
import capstone as cs

ELF = sys.argv[1]
TOOL = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2].endswith('-') else None
# the first four characters are the voices (base 36, 0-n); after them, m<mask> and d<dist>
configs = [a for a in sys.argv[2:] if len(a) >= 4 and all(ch in '0123456789abcdefghijklmn' for ch in a[:4]) and all(ch in '0123456789md' for ch in a[4:])] or ['0127']
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
    nblk = 1500 if STRESS else 300
    mu.mem_write(lma(syms['cfg_blocks']), struct.pack('<I', nblk))
    # enable the FPU (CPACR: CP10, CP11 full access)
    mu.mem_write(0xE000ED88, struct.pack('<I', 0xF << 20))
    st = {'cyc': 0, 'start': 0, 'blocks': [], 'done': False}

    def on_block(uc, addr, size, _):
        c = block_cost(addr, size)
        st['cyc'] += c
        if PROF is not None:
            PROF[addr] = PROF.get(addr, 0) + c

    def on_mark(uc, access, addr, size, value, _):
        if value == 1:
            st['start'] = st['cyc']
        elif value == 2:
            st['blocks'].append(st['cyc'] - st['start'])
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
    return avg, worst

PROF = {} if '--prof' in sys.argv else None
STRESS = '--stress' in sys.argv
names = ['Pad', 'Bells', 'Bass', 'Piano', 'Marimba', 'Drone', 'Kalimba', 'Flute', 'Arp',
         'Swell', 'EP', 'Gong', 'Pizz', 'Shimmer', 'Tails', 'Pulse', 'Strings', 'Glow', 'Seq', 'Super',
         'Ribbon', 'Funk', 'Harp', 'Harpsichord']
for v in configs:
    avg, worst = run(v)
    vv = v[:4]
    print(f"{v:8s} {' + '.join(names[int(c, 36)] for c in vv):40s} avg {avg:5.1f} %   worst block {worst:5.1f} %")

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
