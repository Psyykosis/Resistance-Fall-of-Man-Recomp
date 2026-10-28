#!/usr/bin/env python3
"""v163: route provably-ordinary guest memory accesses of the lifted PPU code
to unchecked inline accessors (ppu_recomp.h vm_*_fast / vm_write*_stk).

  - R_1 (stack pointer) relative loads/stores: the PPU stack is ordinary guest
    memory -- never the null page, MMIO, the gcm ref-poll word or past the end
    of the VM. Stores still honour SPU reservations (the coherence bitmap).
  - R_2 (TOC) relative loads.
  - R_30 relative loads in functions where every assignment of R_30 is a TOC
    load (R_30 = vm_read32(R_2 + c)): the per-file constant pool.

Each of these used the full checked path (span / poll / store-ok / coherence,
plus an out-of-line call that forced register spills around it).
"""
import re, sys

FUNC_RE = re.compile(r'^void (func_[0-9A-F]+)\(ppu_context\* ctx\) \{$')
ACC_RE = re.compile(r'\bvm_(read|write)(8|16|32|64)\((R_(\d+)) \+ ')

def xform_func(lines):
    # R_30 analysis
    r30_ok = True; first_toc = None; first_use = None; first_label = None
    for idx, l in enumerate(lines):
        t = l.strip()
        if first_label is None and re.match(r'^loc_[0-9A-F]+:$', t): first_label = idx
        for m in re.finditer(r'R_30 (=|\+=|-=|\|=|&=) ([^;]*);', l):
            rhs = m.group(2)
            if m.group(1) == '=' and re.match(r'^vm_read32\(R_2 \+ -?0x[0-9A-F]+\)$', rhs):
                if first_toc is None: first_toc = idx
            elif m.group(1) == '=' and rhs in ('ctx->gpr[30]', '_cs_30'):
                pass
            else:
                r30_ok = False
        if first_use is None and re.search(r'\bvm_(read|write)(8|16|32|64)\(R_30 \+ ', l): first_use = idx
    if first_toc is None: r30_ok = False
    if r30_ok and first_use is not None and first_use <= first_toc: r30_ok = False
    if r30_ok and first_label is not None and first_label < first_toc: r30_ok = False
    # R_2 analysis
    r2_ok = True
    for l in lines:
        for m in re.finditer(r'R_2 (=|\+=|-=) ([^;]*);', l):
            rhs = m.group(2)
            if m.group(1) != '=' or not (rhs in ('ctx->gpr[2]', '0x006BD648ULL') or
                                         re.match(r'^vm_read32\(R_\d+ \+ 0x4\)$', rhs)):
                r2_ok = False
    n = [0, 0, 0]
    out = []
    r30_state = False      # R_30 currently holds the TOC-derived base
    for idx, l in enumerate(lines):
        t = l.strip()
        if r30_ok:
            if idx == first_toc: pass
            if re.match(r'^loc_[0-9A-F]+:$', t) and idx > first_toc: r30_state = True
        def rep(m):
            rw, w, reg, rn = m.group(1), m.group(2), m.group(3), int(m.group(4))
            if rn == 1:
                n[0] += 1
                return ('vm_read%s_fast(%s + ' % (w, reg)) if rw == 'read' else ('vm_write%s_stk(%s + ' % (w, reg))
            if rw == 'read' and rn == 2 and r2_ok:
                n[1] += 1; return 'vm_read%s_fast(R_2 + ' % w
            if rw == 'read' and rn == 30 and r30_ok and r30_state:
                n[2] += 1; return 'vm_read%s_fast(R_30 + ' % w
            return m.group(0)
        # an assignment on this line takes effect after the line's reads
        l2 = ACC_RE.sub(rep, l)
        if r30_ok:
            for m in re.finditer(r'R_30 = ([^;]*);', l):
                rhs = m.group(1)
                r30_state = bool(re.match(r'^vm_read32\(R_2 \+ -?0x[0-9A-F]+\)$', rhs)) or (rhs == 'ctx->gpr[30]' and r30_state)
        out.append(l2)
    return out, n

def process(src):
    lines = src.split('\n')
    out = []; tot = [0, 0, 0]; i = 0
    while i < len(lines):
        if not FUNC_RE.match(lines[i]):
            out.append(lines[i]); i += 1; continue
        j = i + 1
        while j < len(lines) and lines[j] != '}': j += 1
        body, n = xform_func(lines[i + 1:j])
        for k in range(3): tot[k] += n[k]
        out.append(lines[i]); out.extend(body)
        if j < len(lines): out.append(lines[j])
        i = j + 1
    return '\n'.join(out), tot

if __name__ == '__main__':
    src = open(sys.argv[1], encoding='utf-8', errors='surrogateescape').read()
    if 'v163 fastmem' in src:
        print(sys.argv[1], 'already processed'); sys.exit(0)
    res, tot = process(src)
    res = res.replace('\nextern "C" uint8_t* vm_base;\n', '\n#ifndef PPU_FIXED_BOUNDS   /* v163: ppu_recomp.h declares it const */\nextern "C" uint8_t* vm_base;\n#endif\n', 1)
    res = res.replace('#include "ppu_recomp.h"', '#include "ppu_recomp.h"\n/* v163 fastmem: stack / TOC accesses use the unchecked accessors */', 1)
    open(sys.argv[2], 'w', encoding='utf-8', errors='surrogateescape').write(res)
    print('%s: stack %d, toc %d, r30-pool %d' % (sys.argv[1], tot[0], tot[1], tot[2]))
