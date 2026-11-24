#!/usr/bin/env python3
"""The speech textures' cost, with cpu_count.py's Cortex-M4 cycle weights: speech_cpu.py ELF TOOLPREFIX KIND"""
import subprocess, sys, struct, functools
from unicorn import *
from unicorn.arm_const import *
import capstone as cs
ELF, TOOL, KIND = sys.argv[1], sys.argv[2], int(sys.argv[3])
syms = {}
for line in subprocess.check_output([TOOL + 'nm', ELF], text=True).splitlines():
    p = line.split()
    if len(p) == 3: syms[p[2]] = int(p[0], 16)
img = subprocess.check_output([TOOL + 'objcopy', '-O', 'binary', '-j', '.text', '-j', '.data', ELF, '/dev/stdout'])
src = open(__file__.replace('speech_cpu.py', 'cpu_count.py')).read()
exec(src[src.index('def weight'):src.index('@functools.lru_cache')])   # (the same weights)
md = cs.Cs(cs.CS_ARCH_ARM, cs.CS_MODE_THUMB | cs.CS_MODE_MCLASS)
mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
mu.mem_map(0, 1 << 20); mu.mem_map(0x20000000, 256 << 10); mu.mem_map(0xE0000000, 1 << 20)
mu.mem_write(0, img[:1 << 20])
lma = syms['_sidata'] + (syms['cfg_kind'] - syms['_sdata'])
mu.mem_write(lma, struct.pack('<i', KIND))
@functools.lru_cache(maxsize=None)
def cost(addr, size):
    c, last = 0, None
    for ins in md.disasm(bytes(mu.mem_read(addr, size)), addr):
        c += weight(ins.mnemonic, ins.op_str); last = ins.mnemonic
    if last and last.split('.')[0] in ('b','beq','bne','bgt','blt','bge','ble','bhi','bls','bcs','bcc','bmi','bpl','cbz','cbnz','bhs','blo'): c += 2
    return c
st = {'on': False, 'c': 0, 'per': []}
def on_block(uc, a, s, _):
    if st['on']: st['c'] += cost(a, s)
def on_mark(uc, acc, a, s, v, _):
    if v == 1: st['on'] = True; st['c'] = 0
    elif v == 2: st['on'] = False; st['per'].append(st['c'])
    elif v == 3: uc.emu_stop()
mu.hook_add(UC_HOOK_BLOCK, on_block)
mu.hook_add(UC_HOOK_MEM_WRITE, on_mark, begin=syms['mark'], end=syms['mark'] + 3)
sp, pc = struct.unpack('<II', img[:8])
mu.reg_write(UC_ARM_REG_SP, sp)
mu.emu_start(pc | 1, 0xFFFFFFFF, count=0)
per = st['per']
avg = sum(per) / len(per)
print(f"kind {KIND}: {len(per)} ms, avg {avg:.0f} cycles/ms = {avg / 640:.1f} % of the CPU, worst ms {max(per) / 640:.1f} %")
