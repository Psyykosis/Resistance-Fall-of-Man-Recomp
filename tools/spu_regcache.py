#!/usr/bin/env python3
"""Resistance port (v142): register caching for the lifted SPU code.

The SPU twin of regcache.py. Each lifted SPU function keeps its 128-bit
registers in spu_context (ctx->gpr[n]); every local-store write goes through
ctx->ls (a uint8_t*), which the compiler must assume can alias the context,
so every register is re-read from memory after each LS store. Locals (S_n)
hold the registers instead; writes stay write-through (ctx->gpr[n] = S_n =
...), so the context is always current, and after any call that can run
other SPU code or touch registers (lifted functions, spu_drain_call, channel
ops, ...) the locals are re-read. Local-store reads/writes (spu_ls_read128 /
spu_ls_write128) only read the context, so they need no reload.

  spu_regcache.py in.c out.c
"""
import re, sys

FHEAD = re.compile(r'^void (\w+)\(spu_context\* ctx\) \{\n', re.M)
PURE = ('spu_ls_read128', 'spu_ls_write128', 'spu_ls_read32', 'spu_ls_write32',
        'spu_trace_pc', 'spu_trace_rt')
MARK = '/* spu regcache v142 */'

def stmt_end(s, i):
    depth = 0
    while i < len(s):
        c = s[i]
        if c in '([': depth += 1
        elif c in ')]': depth -= 1
        elif c == ';' and depth <= 0: return i + 1
        elif c in '{}' and depth <= 0: return -1
        i += 1
    return -1

def call_name(body, i):
    """name of the innermost call whose argument list holds position i"""
    depth = 0; j = i - 1
    while j >= 0:
        c = body[j]
        if c == ')': depth += 1
        elif c == '(':
            if depth == 0:
                k = j - 1
                while k >= 0 and body[k] == ' ': k -= 1
                e = k + 1
                while k >= 0 and (body[k].isalnum() or body[k] == '_'): k -= 1
                return body[k + 1:e]
            depth -= 1
        elif c in ';{}' and depth == 0: return ''
        j -= 1
    return ''

def transform_body(body):
    if re.search(r'&\s*ctx->gpr', body): return None, 'address taken'
    if re.search(r'ctx->gpr(?!\[\d+\])', body): return None, 'computed register index'
    if re.search(r'\bS_\d+\b', body): return None, 'name clash'
    if re.search(r'ctx->gpr\[\d+\](\.\w+\[\d+\])?\s*(\+\+|--|[-+*/|&^%]=|<<=|>>=)', body): return None, 'compound write'
    if re.search(r'ctx->gpr\[\d+\]\.\w+\[\d+\]\s*=(?!=)', body): return None, 'lane write'
    used = sorted(set(int(n) for n in re.findall(r'ctx->gpr\[(\d+)\]', body)))
    if not used: return body, 'no registers'
    ins = []
    for m in re.finditer(r'\bctx\b(?!->)', body):
        cn = call_name(body, m.start())
        if cn in PURE: continue
        if cn in ('SPU_RET', 'SPU_RET_REG', 'SPU_TAILCALL'): continue
        e = stmt_end(body, m.start())
        if e < 0: return None, 'bare ctx outside a simple statement'
        rest = body[e:].lstrip()
        if rest.startswith('else'): return None, 'statement before else'
        if rest.startswith('return'): continue
        ins.append(e)
    ins = sorted(set(ins))
    # a reload followed only by statements that neither read registers nor
    # branch, up to the next reload, is redundant
    keep = []
    for k, e in enumerate(ins):
        if k + 1 < len(ins):
            between = body[e:ins[k + 1]]
            if ('gpr' not in between and not re.search(r'[{}:]|\bgoto\b|\breturn\b|\bif\b|\bswitch\b|\bcase\b', between)):
                continue
        keep.append(e)
    for e in reversed(keep):
        body = body[:e] + ' _SRC_RL;' + body[e:]
    body = re.sub(r'ctx->gpr\[(\d+)\]\s*=(?!=)', lambda m: 'CTXW_%s = S_%s =' % (m.group(1), m.group(1)), body)
    body = re.sub(r'ctx->gpr\[(\d+)\]', lambda m: 'S_' + m.group(1), body)
    body = re.sub(r'CTXW_(\d+)', lambda m: 'ctx->gpr[%s]' % m.group(1), body)
    decl = ''.join('        u128 S_%d = ctx->gpr[%d];\n' % (n, n) for n in used)
    rl = ' '.join('S_%d = ctx->gpr[%d];' % (n, n) for n in used)
    return (decl, body, rl), 'ok'

def main(inp, out):
    src = open(inp, encoding='utf-8', errors='surrogateescape').read()
    if MARK in src:
        print(inp, 'already transformed')
        if inp != out: open(out, 'w', encoding='utf-8', errors='surrogateescape').write(src)
        return
    res = []; pos = 0; stats = {}
    for m in FHEAD.finditer(src):
        if m.start() < pos: continue
        end = src.find('\n}\n', m.end())
        if end < 0: break
        body = src[m.end():end + 1]
        r, why = transform_body(body)
        stats[why] = stats.get(why, 0) + 1
        res.append(src[pos:m.start()])
        if isinstance(r, tuple):
            decl, nb, rl = r
            res.append('#define _SRC_RL do { %s } while (0)\n' % rl)
            res.append(m.group(0) + decl + nb + '}\n#undef _SRC_RL\n')
        else:
            res.append(src[m.start():end + 3])
        pos = end + 3
    res.append(src[pos:])
    txt = ''.join(res)
    first_inc = txt.find('#include')
    eol = txt.find('\n', first_inc) + 1
    txt = txt[:eol] + MARK + '\n' + txt[eol:]
    open(out, 'w', encoding='utf-8', errors='surrogateescape').write(txt)
    print(inp, stats)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
