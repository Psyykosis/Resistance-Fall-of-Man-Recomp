#!/usr/bin/env python3
"""Resistance: Fall of Man port -- hand edits to the lifted PPU code, kept as
data so the code can be regenerated from the player's own copy of the game.

  extract <edited_dir> <fresh_dir> <hooks.json>   record the edits
  apply   <fresh_dir> <hooks.json>                re-apply them in place

Edits are recorded per function (func_XXXXXXXX), as small hunks: the lines
the port removed/added plus one line of context. Chunk boundaries do not
matter. Lines added at the top of a chunk (extern declarations) are applied to
every chunk. ppu_recomp.h is handled as one file.
"""
import difflib, glob, json, os, re, sys

FR = re.compile(r'^void (func_[0-9A-F]+)\(ppu_context\* ctx\) \{$')
TAIL = re.compile(r'^(#undef _RCX_RL|#define _RCX_RL .*|\s*)$')

def rd(p):
    return open(p, encoding='utf-8', errors='surrogateescape').read().split('\n')

def split_chunk(lines):
    """-> preamble lines, [(name, body lines)] ; trailing blank/_RCX_RL lines of a
    body are moved out so chunk layout differences do not register as edits."""
    pre, funcs, cur, buf = [], [], None, []
    for l in lines:
        m = FR.match(l)
        if m:
            if cur: funcs.append((cur, buf))
            cur, buf = m.group(1), [l]
        elif cur: buf.append(l)
        else: pre.append(l)
    if cur: funcs.append((cur, buf))
    out = []
    for n, b in funcs:
        k = len(b)
        while k > 1 and TAIL.match(b[k - 1]): k -= 1
        # cut at the function table, if a chunk carries it
        for i in range(k):
            if b[i].startswith('/* Function table */') or b[i].startswith('const func_entry function_table'):
                k = i
                while k > 1 and TAIL.match(b[k - 1]): k -= 1
                break
        out.append((n, b[:k]))
    return pre, out

def load(d):
    funcs, pres = {}, []
    for f in sorted(glob.glob(os.path.join(d, 'ppu_recomp_0*.cpp'))):
        pre, fs = split_chunk(rd(f))
        pres.append(pre)
        for n, b in fs: funcs[n] = b
    return funcs, pres

def hunks(a, b):
    """edits turning a into b: [{ctx, rm, add}] with one line of leading context"""
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    out = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal': continue
        out.append({'at': i1, 'ctx': a[i1 - 1] if i1 > 0 else None, 'rm': a[i1:i2], 'add': b[j1:j2]})
    return out

def apply_hunks(lines, hs, where):
    """bottom-up by recorded position (positions refer to the fresh lift);
    each hunk is checked against its removed lines + context, and searched
    for nearby if the fresh lift shifted a little."""
    for h in sorted(hs, key=lambda h: h['at'], reverse=True):
        rm, ctx, at = h['rm'], h['ctx'], h['at']
        def ok(i):
            return 0 <= i <= len(lines) and lines[i:i + len(rm)] == rm and (ctx is None or (i > 0 and lines[i - 1] == ctx))
        hit = at if ok(at) else None
        if hit is None:
            for d in range(1, 400):
                if ok(at - d): hit = at - d; break
                if ok(at + d): hit = at + d; break
        if hit is None:
            raise SystemExit('hook does not apply in %s near line %d (%r)' % (where, at, ctx))
        lines[hit:hit + len(rm)] = h['add']
    return lines

def extract(ed, fr, outp):
    A, preA = load(ed); B, preB = load(fr)
    res = {'funcs': {}, 'preamble': [], 'header': None}
    for n, b in A.items():
        if n not in B: raise SystemExit('function %s missing from fresh lift' % n)
        if b != B[n]: res['funcs'][n] = hunks(B[n], b)
    fresh_pre = set(l for p in preB for l in p)
    seen = set()
    for p in preA:
        for l in p:
            if l not in fresh_pre and l not in seen: seen.add(l); res['preamble'].append(l)
    ha, hb = os.path.join(ed, 'ppu_recomp.h'), os.path.join(fr, 'ppu_recomp.h')
    if os.path.exists(ha) and os.path.exists(hb):
        res['header'] = hunks(rd(hb), rd(ha))
    json.dump(res, open(outp, 'w'), indent=0)
    print('%s: %d functions, %d preamble lines, %d header hunks' % (outp, len(res['funcs']), len(res['preamble']), len(res['header'] or [])))

def apply(fr, hp):
    H = json.load(open(hp))
    todo = dict(H['funcs']); n = 0
    for f in sorted(glob.glob(os.path.join(fr, 'ppu_recomp_0*.cpp'))):
        L = rd(f); out = []; i = 0; changed = False
        # preamble: right after the include line
        if H['preamble']:
            for k, l in enumerate(L):
                if l.startswith('#include "ppu_recomp.h"'):
                    j = k + 1
                    while j < len(L) and L[j].startswith('/*') and L[j].endswith('*/'): j += 1
                    L[j:j] = H['preamble']; changed = True
                    break
        k = 0
        while k < len(L):
            m = FR.match(L[k])
            if m and m.group(1) in todo:
                e = k + 1
                while e < len(L) and not FR.match(L[e]): e += 1
                body = L[k:e]
                body = apply_hunks(body, todo.pop(m.group(1)), m.group(1))
                L[k:e] = body; k += len(body); changed = True; n += 1
                continue
            k += 1
        if changed: open(f, 'w', encoding='utf-8', errors='surrogateescape').write('\n'.join(L))
    if todo: raise SystemExit('functions not found: %s' % ', '.join(sorted(todo)[:10]))
    if H.get('header'):
        hp2 = os.path.join(fr, 'ppu_recomp.h')
        txt = '\n'.join(apply_hunks(rd(hp2), H['header'], 'ppu_recomp.h'))   # read before opening for write
        open(hp2, 'w', encoding='utf-8', errors='surrogateescape').write(txt)
    print('applied %d function hooks' % n)

if __name__ == '__main__':
    if sys.argv[1] == 'extract': extract(sys.argv[2], sys.argv[3], sys.argv[4])
    elif sys.argv[1] == 'apply': apply(sys.argv[2], sys.argv[3])
    else: raise SystemExit(__doc__)
