#!/usr/bin/env python3
"""Resistance: Fall of Man port -- hand edits to the lifted SPU modules
(project/spu/<module>/spu_recomp.c), kept as data.

  extract <cur.c> <raw.c> <post.c> <out.json>
      cur  = the port's edited file, raw = fresh lift, post = fresh lift after
      spu_regcache + spu_directhop. A function the port edited before the
      register-cache pass (its body is not register-cached in cur) is recorded
      against the raw lift ("pre"), so regcache sees the edit; others against
      the post-pass output ("post").
  apply pre|post <file.c> <hooks.json>
"""
import json, re, sys
sys.path.insert(0, __file__.rsplit('/', 1)[0] if '/' in __file__ else '.')
from rfom_hooks import hunks, apply_hunks

FR = re.compile(r'^void (\w+_spu_func_[0-9A-F]+)\(spu_context\* ctx\) \{$')
TAIL = re.compile(r'^(#undef _SRC_RL|#define _SRC_RL .*|\s*)$')

def rd(p): return open(p, encoding='utf-8', errors='surrogateescape').read().split('\n')

def split(lines):
    pre, funcs, cur, buf = [], {}, None, []
    for l in lines:
        m = FR.match(l)
        if m:
            if cur: funcs[cur] = buf
            cur, buf = m.group(1), [l]
        elif cur: buf.append(l)
        else: pre.append(l)
    if cur: funcs[cur] = buf
    for n, b in funcs.items():
        k = len(b)
        while k > 1 and TAIL.match(b[k - 1]): k -= 1
        funcs[n] = b[:k]
    return pre, funcs

def extract(curp, rawp, postp, outp):
    pc, C = split(rd(curp)); pr, R = split(rd(rawp)); pp, P = split(rd(postp))
    res = {'pre': {}, 'post': {}, 'preamble': hunks(pp, pc)}
    for n, b in C.items():
        if n not in P: raise SystemExit('%s: %s not in fresh lift' % (curp, n))
        if b == P[n]: continue
        cached_cur = any('u128 S_' in l for l in b)
        cached_post = any('u128 S_' in l for l in P[n])
        if not cached_cur and cached_post: res['pre'][n] = hunks(R[n], b)
        else: res['post'][n] = hunks(P[n], b)
    json.dump(res, open(outp, 'w'), indent=0)
    print('%s: pre %d, post %d functions, preamble %d hunks' % (outp, len(res['pre']), len(res['post']), len(res['preamble'])))

def apply(stage, fp, hp):
    H = json.load(open(hp)); todo = dict(H[stage])
    L = rd(fp)
    if stage == 'post' and H['preamble']:
        k = 0
        while k < len(L) and not FR.match(L[k]): k += 1
        L[:k] = apply_hunks(L[:k], H['preamble'], fp + ' preamble')
    k = 0
    while k < len(L):
        m = FR.match(L[k])
        if m and m.group(1) in todo:
            e = k + 1
            while e < len(L) and not FR.match(L[e]): e += 1
            # keep the trailing #define/#undef/blank lines outside the edited body
            t = e
            while t > k + 1 and TAIL.match(L[t - 1]): t -= 1
            body = apply_hunks(L[k:t], todo.pop(m.group(1)), m.group(1))
            # a hook that brings in a register-cached body also needs the
            # _SRC_RL macro spu_regcache.py writes around such bodies
            regs = re.findall(r'^\s*u128 S_(\d+) = ctx->gpr\[\d+\];$', '\n'.join(body), re.M)
            if regs and not (k > 0 and L[k - 1].startswith('#define _SRC_RL')):
                rl = ' '.join('S_%s = ctx->gpr[%s];' % (r, r) for r in regs)
                body = ['#define _SRC_RL do { %s } while (0)' % rl] + body
                if not (t < len(L) and L[t].startswith('#undef _SRC_RL')):
                    body = body + ['#undef _SRC_RL']
            L[k:t] = body; k += len(body); continue
        k += 1
    if todo: raise SystemExit('%s: functions not found: %s' % (fp, ', '.join(todo)))
    txt = '\n'.join(L)
    open(fp, 'w', encoding='utf-8', errors='surrogateescape').write(txt)

if __name__ == '__main__':
    if sys.argv[1] == 'extract': extract(*sys.argv[2:6])
    elif sys.argv[1] == 'apply': apply(sys.argv[2], sys.argv[3], sys.argv[4])
    else: raise SystemExit(__doc__)
