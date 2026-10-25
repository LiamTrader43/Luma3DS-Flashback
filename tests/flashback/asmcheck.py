"""Checks the recorder's assembly on a small interpreter of the ARM
instructions it uses:
  pack     flashback_pack.s (flashbackPackRowAsm / ...Pf) against a Python port
           of the C packer (flashbackPackPixels): every pixel stream must give
           the same bytes, and leave the same prev, run and table;
  capture  flashback_capture.s (flashbackConvertRgba8 / Bgr8 / Rgb565) against
           the C conversion (flashbackPixelToRgb565): the same pixels, nothing
           written past the end, and the pointers left where the C leaves them.

usage: python asmcheck.py pack path/to/flashback_pack.s [count]
       python asmcheck.py capture path/to/flashback_capture.s [count]
"""
import random
import re
import sys

M32 = 0xFFFFFFFF


# ---------------------------------------------------------------- assembler

def strip(line):
    i = line.find('@')
    return (line[:i] if i >= 0 else line).rstrip()


def expand(lines, macros, depth=0):
    """Expands macro invocations (and .if/.endif inside them)."""
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        s = line.strip()
        if s.startswith('.macro'):
            parts = s[len('.macro'):].replace(',', ' ').split()
            name, params = parts[0], parts[1:]
            body = []
            i += 1
            while lines[i].strip() != '.endm':
                body.append(lines[i])
                i += 1
            macros[name] = (params, body)
            i += 1
            continue
        word = s.split(None, 1)
        if word and word[0] in macros:
            params, body = macros[word[0]]
            args = [a.strip() for a in word[1].split(',')] if len(word) > 1 else []
            sub = []
            skip = []
            for b in body:
                t = b
                for p, a in sorted(zip(params, args), key=lambda x: -len(x[0])):
                    t = t.replace('\\' + p, a)
                t = t.replace('\\()', '')
                ts = t.strip()
                if ts.startswith('.if'):
                    skip.append(int(ts[3:].strip(), 0) == 0)
                    continue
                if ts == '.endif':
                    skip.pop()
                    continue
                if any(skip):
                    continue
                sub.append(t)
            out.extend(expand(sub, macros, depth + 1))
            i += 1
            continue
        out.append(line)
        i += 1
    return out


def assemble(path):
    lines = [strip(l) for l in open(path, encoding='utf-8')]
    lines = [l for l in lines if l.strip()]
    lines = expand(lines, {})
    prog, labels = [], {}
    for l in lines:
        s = l.strip()
        while True:
            m = re.match(r'^([\w.]+):\s*(.*)$', s)
            if not m:
                break
            if m.group(1).isdigit():
                labels.setdefault('#' + m.group(1), []).append(len(prog))
            else:
                labels[m.group(1)] = len(prog)
            s = m.group(2).strip()
        if not s or s.startswith('.'):
            continue
        op, _, rest = s.partition(' ')
        prog.append((op, rest.strip(), s))
    return prog, labels


# ---------------------------------------------------------------- machine

REGS = {**{'r%d' % i: i for i in range(13)}, 'sp': 13, 'lr': 14, 'pc': 15,
        'ip': 12, 'fp': 11, 'sl': 10}
CONDS = ['eq', 'ne', 'cs', 'hs', 'cc', 'lo', 'mi', 'pl', 'hi', 'ls', 'ge', 'lt', 'gt', 'le']
BASE = ['push', 'pop', 'ldrh', 'ldr', 'strb', 'str', 'add', 'sub', 'rsb', 'mov', 'lsl', 'lsr', 'asr',
        'cmp', 'orr', 'bic', 'mul', 'pld', 'b', 'and', 'subs', 'ldmia', 'stmia']


def split_op(op):
    for base in sorted(BASE, key=len, reverse=True):
        if op.startswith(base):
            rest = op[len(base):]
            if rest == '' or rest in CONDS:
                return base, rest
    raise ValueError('unknown op ' + op)


class Machine:
    def __init__(self, prog, labels, mem):
        self.prog, self.labels, self.mem = prog, labels, mem
        self.r = [0] * 16
        self.N = self.Z = self.C = self.V = False
        self.steps = 0

    def cond(self, c):
        return {'': True, 'eq': self.Z, 'ne': not self.Z, 'cs': self.C, 'hs': self.C,
                'cc': not self.C, 'lo': not self.C, 'mi': self.N, 'pl': not self.N,
                'hi': self.C and not self.Z, 'ls': (not self.C) or self.Z}[c]

    def reg(self, s):
        return self.r[REGS[s.strip()]]

    def op2(self, parts):
        parts = [p.strip() for p in parts]
        if parts[0].startswith('#'):
            return int(parts[0][1:], 0) & M32
        v = self.reg(parts[0])
        if len(parts) > 1:
            kind, amt = parts[1].split()
            n = int(amt.lstrip('#'), 0)
            v = shift(v, kind, n)
        return v

    def rd8(self, a):
        return self.mem[a]

    def rd16(self, a):
        assert a % 2 == 0, 'unaligned halfword at %x' % a
        return self.mem[a] | (self.mem[a + 1] << 8)

    def rd32(self, a):
        assert a % 4 == 0
        return int.from_bytes(self.mem[a:a + 4], 'little')

    def wr8(self, a, v):
        self.mem[a] = v & 0xFF

    def wr32(self, a, v):
        assert a % 4 == 0
        self.mem[a:a + 4] = (v & M32).to_bytes(4, 'little')

    def run(self, entry, args):
        self.r[0:4] = args
        self.r[13] = 0x3F0000
        self.r[14] = 0xDEAD0000
        pc = self.labels[entry]
        while True:
            self.steps += 1
            op, rest, text = self.prog[pc]
            base, c = split_op(op)
            pc += 1
            if not self.cond(c):
                continue
            ops = [x.strip() for x in re.split(r',(?![^\[{]*[\]}])', rest)] if rest else []
            if base == 'b':
                t = ops[0]
                m = re.match(r'^(\d+)([bf])$', t)
                if m:
                    cands = self.labels['#' + m.group(1)]
                    here = pc - 1
                    pc = max(c for c in cands if c <= here) if m.group(2) == 'b' else min(c for c in cands if c > here)
                else:
                    pc = self.labels[t]
            elif base in ('ldmia', 'stmia'):
                rn = REGS[ops[0].rstrip('!')]
                a = self.r[rn]
                for x in reglist(ops[1]):
                    if base == 'ldmia':
                        self.r[x] = self.rd32(a)
                    else:
                        self.wr32(a, self.r[x])
                    a += 4
                if ops[0].endswith('!'):
                    self.r[rn] = a & M32
            elif base == 'subs':
                a, b = self.reg(ops[1]), self.op2(ops[2:])
                res = (a - b) & M32
                self.r[REGS[ops[0]]] = res
                self.N, self.Z, self.C = bool(res >> 31), res == 0, a >= b
            elif base == 'push':
                regs = reglist(ops[0])
                for x in reversed(regs):
                    self.r[13] -= 4
                    self.wr32(self.r[13], self.r[x])
            elif base == 'pop':
                regs = reglist(ops[0])
                for x in regs:
                    self.r[x] = self.rd32(self.r[13])
                    self.r[13] += 4
                if 15 in regs:
                    return
            elif base == 'pld':
                pass
            elif base in ('ldr', 'ldrh', 'str', 'strb'):
                self.mem_op(base, ops)
            elif base == 'mul':
                self.r[REGS[ops[0]]] = (self.reg(ops[1]) * self.reg(ops[2])) & M32
            elif base == 'cmp':
                a, b = self.reg(ops[0]), self.op2(ops[1:])
                res = (a - b) & M32
                self.N, self.Z, self.C = bool(res >> 31), res == 0, a >= b
            elif base in ('lsl', 'lsr', 'asr'):
                self.r[REGS[ops[0]]] = shift(self.reg(ops[1]), base, int(ops[2].lstrip('#'), 0))
            elif base == 'mov':
                self.r[REGS[ops[0]]] = self.op2(ops[1:])
            else:
                a, b = self.reg(ops[1]), self.op2(ops[2:])
                v = {'add': a + b, 'sub': a - b, 'rsb': b - a, 'orr': a | b, 'bic': a & ~b, 'and': a & b}[base]
                self.r[REGS[ops[0]]] = v & M32
            if self.steps > 50_000_000:
                raise RuntimeError('runaway')

    def mem_op(self, base, ops):
        rt = REGS[ops[0]]
        if base == 'ldr' and ops[1].startswith('='):
            self.r[rt] = int(ops[1][1:], 0) & M32
            return
        addr_s = ops[1]
        post = ops[2:]                  # post-index: [rn], off
        inner = addr_s.strip('[]')
        parts = [p.strip() for p in inner.split(',')]
        rn = REGS[parts[0]]
        if post:
            addr = self.r[rn]
            off = self.op2(post)
            self.r[rn] = (self.r[rn] + off) & M32
        else:
            off = self.op2(parts[1:]) if len(parts) > 1 else 0
            addr = (self.r[rn] + off) & M32
        if base == 'ldr':
            self.r[rt] = self.rd32(addr)
        elif base == 'ldrh':
            self.r[rt] = self.rd16(addr)
        elif base == 'str':
            self.wr32(addr, self.r[rt])
        else:
            self.wr8(addr, self.r[rt])


def reglist(s):
    out = []
    for part in s.strip('{}').split(','):
        part = part.strip()
        if '-' in part:
            a, b = part.split('-')
            out.extend(range(REGS[a], REGS[b] + 1))
        else:
            out.append(REGS[part])
    return out


def shift(v, kind, n):
    v &= M32
    if kind == 'lsl':
        return (v << n) & M32
    if kind == 'lsr':
        return v >> n
    if kind == 'asr':
        s = v - (1 << 32) if v >> 31 else v
        return (s >> n) & M32
    raise ValueError(kind)


# ---------------------------------------------------------------- reference

def wrap(d, bits):
    d &= (1 << bits) - 1
    return d - (1 << bits) if d >= (1 << (bits - 1)) else d


def ref_pack(pixels, index, prev, run, out):
    """flashbackCompress's packing loop (the original), no room check."""
    for px in pixels:
        if px == prev:
            run += 1
            if run == 62:
                out.append(0xC0 | 61)
                run = 0
            continue
        if run:
            out.append(0xC0 | (run - 1))
            run = 0
        h = ((px * 0x9E3779B1) & M32) >> 26
        if index[h] == px:
            out.append(h)
        else:
            index[h] = px
            dr = wrap((px >> 11) - (prev >> 11), 5)
            dg = wrap(((px >> 5) & 0x3F) - ((prev >> 5) & 0x3F), 6)
            db = wrap((px & 0x1F) - (prev & 0x1F), 5)
            tr, tb = dr - (dg >> 1), db - (dg >> 1)
            if -2 <= dr <= 1 and -2 <= dg <= 1 and -2 <= db <= 1:
                out.append(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2))
            elif -8 <= tr <= 7 and -8 <= tb <= 7:
                out.append(0x80 | (dg + 32))
                out.append(((tr + 8) << 4) | (tb + 8))
            else:
                out += [0xFE, px & 0xFF, px >> 8]
        prev = px
    return prev, run


# ---------------------------------------------------------------- tests

ST, IDX, OUT, PIX = 0x1000, 0x2000, 0x10000, 0x100000


def stream(rng, n, kind):
    """A pixel stream that exercises the packer: long runs, table hits, small
    and medium steps, and big jumps, in proportions set by 'kind'."""
    px = rng.randrange(65536)
    seen = [px]
    out = []
    w = {'runs': (60, 15, 15, 7, 3), 'mixed': (30, 30, 20, 15, 5), 'noise': (5, 10, 10, 10, 65),
         'steps': (10, 10, 50, 25, 5), 'longruns': (95, 2, 1, 1, 1)}[kind]
    for _ in range(n):
        k = rng.choices(range(5), w)[0]
        if k == 1:
            px = rng.choice(seen[-80:])
        elif k == 2:        # small step in each channel
            r, g, b = px >> 11, (px >> 5) & 63, px & 31
            px = (((r + rng.randint(-2, 1)) & 31) << 11) | (((g + rng.randint(-2, 1)) & 63) << 5) | ((b + rng.randint(-2, 1)) & 31)
        elif k == 3:        # medium step
            r, g, b = px >> 11, (px >> 5) & 63, px & 31
            dg = rng.randint(-34, 33)
            px = (((r + dg // 2 + rng.randint(-9, 8)) & 31) << 11) | (((g + dg) & 63) << 5) | ((b + dg // 2 + rng.randint(-9, 8)) & 31)
        elif k == 4:
            px = rng.randrange(65536)
        out.append(px)
        seen.append(px)
    return out


def check(prog, labels, rng, n_rows, names=('flashbackPackRowAsm', 'flashbackPackRowAsmPf')):
    bad = 0
    for t in range(n_rows):
        kind = rng.choice(['runs', 'mixed', 'noise', 'steps', 'longruns'])
        step = rng.choice([2, 480, 240, 2, 480])
        n = rng.choice([1, 2, 3, rng.randint(4, 40), rng.randint(40, 400)])
        pf = rng.random() < 0.5
        # state going in: a table of past pixels, a prev and a pending run
        index = [0] * 64
        for _ in range(rng.randint(0, 64)):
            p = rng.randrange(65536)
            index[((p * 0x9E3779B1) & M32) >> 26] = p
        prev = rng.choice([0, rng.randrange(65536)])
        run = rng.choice([0, 0, rng.randint(1, 61), 61])
        pixels = stream(rng, n, kind)
        if rng.random() < 0.3 and pixels:
            pixels[0] = prev        # start inside a run
        # reference
        ri, rout = list(index), []
        rprev, rrun = ref_pack(pixels, ri, prev, run, rout)
        # assembly
        mem = bytearray(0x400000)
        for i, p in enumerate(pixels + [rng.randrange(65536)]):     # (+ the one read past the end)
            a = PIX + i * step
            mem[a] = p & 0xFF
            mem[a + 1] = p >> 8
        for i, v in enumerate(index):
            mem[IDX + 4 * i:IDX + 4 * i + 4] = v.to_bytes(4, 'little')
        for i, v in enumerate([IDX, OUT, prev, run]):
            mem[ST + 4 * i:ST + 4 * i + 4] = v.to_bytes(4, 'little')
        m = Machine(prog, labels, mem)
        m.run(names[1] if pf else names[0], [PIX, PIX + n * step, step, ST])
        aout_end = int.from_bytes(mem[ST + 4:ST + 8], 'little')
        aout = list(mem[OUT:aout_end])
        aprev = int.from_bytes(mem[ST + 8:ST + 12], 'little')
        arun = int.from_bytes(mem[ST + 12:ST + 16], 'little')
        aidx = [int.from_bytes(mem[IDX + 4 * i:IDX + 4 * i + 4], 'little') for i in range(64)]
        saved = [m.r[i] for i in range(4, 12)]
        if aout != rout or aprev != rprev or arun != rrun or aidx != ri:
            bad += 1
            if bad <= 5:
                print(f'DIFFERENT: row {t} ({kind}, {n} px, step {step}, pf {pf}): '
                      f'out {len(aout)}/{len(rout)} bytes, prev {aprev:#x}/{rprev:#x}, run {arun}/{rrun}, '
                      f'table {"same" if aidx == ri else "different"}')
                for k, (x, y) in enumerate(zip(aout, rout)):
                    if x != y:
                        print(f'   first difference at byte {k}: {x:#04x} / {y:#04x}')
                        break
    return bad


def rgb565_ref(fmt, b):
    """flashbackPixelToRgb565 (the C capture) on one pixel's bytes."""
    if fmt == 'rgba8':
        px = int.from_bytes(b[:4], 'little')
        r, g, bl = px >> 24, (px >> 16) & 0xFF, (px >> 8) & 0xFF
    elif fmt == 'bgr8':
        r, g, bl = b[2], b[1], b[0]
    else:
        return b[0] | (b[1] << 8)
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3)


def check_capture(prog, labels, rng, count):
    bad = 0
    for t in range(count):
        fmt, entry, bpp = rng.choice([('rgba8', 'flashbackConvertRgba8', 4), ('bgr8', 'flashbackConvertBgr8', 3),
                                      ('rgb565', 'flashbackConvertRgb565', 2)])
        cols = rng.randint(1, 4)
        skip = rng.choice([0, 0, 240 * bpp])            # 800px mode skips a column
        mem = bytearray(0x400000)
        src, dst = 0x100000, 0x200000
        nbytes = cols * (240 * bpp + skip)
        mem[src:src + nbytes] = bytes(rng.randrange(256) for _ in range(nbytes))
        mem[dst:dst + cols * 480 + 64] = bytes([0xAA]) * (cols * 480 + 64)
        m = Machine(prog, labels, mem)
        m.run(entry, [src, dst, cols, skip])
        want = []
        for c in range(cols):
            base = src + c * (240 * bpp + skip)
            for y in range(240):
                want.append(rgb565_ref(fmt, mem[base + y * bpp:base + y * bpp + 4]))
        got = [mem[dst + 2 * i] | (mem[dst + 2 * i + 1] << 8) for i in range(cols * 240)]
        past = mem[dst + cols * 480:dst + cols * 480 + 64]
        untouched = past == bytes([0xAA]) * 64
        if got != want or not untouched or m.r[0] != src + nbytes or m.r[1] != dst + cols * 480:
            bad += 1
            if bad <= 5:
                k = next((i for i in range(len(want)) if got[i] != want[i]), None)
                print(f'DIFFERENT: {fmt}, {cols} columns, skip {skip}: first wrong pixel {k}, '
                      f'wrote past the end: {not untouched}')
    return bad


if __name__ == '__main__':
    if len(sys.argv) < 3 or sys.argv[1] not in ('pack', 'capture'):
        sys.exit(__doc__)
    mode, path = sys.argv[1], sys.argv[2]
    prog, labels = assemble(path)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 3000
    rng = random.Random(1)
    print(f'{len(prog)} instructions, {len(labels)} labels')
    if mode == 'capture':
        bad = check_capture(prog, labels, rng, count)
        what = 'conversions'
    else:
        bad = check(prog, labels, rng, count)
        what = 'rows'
    print(f'{count} {what}: {bad} different')
    print('all identical' if bad == 0 else 'FAILED')
    sys.exit(1 if bad else 0)
