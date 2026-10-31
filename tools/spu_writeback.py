#!/usr/bin/env python3
"""Resistance port (v189): write-back register cache for the lifted SPU code.

Runs after spu_regcache.py (which keeps registers in locals S_n but writes
every result through to ctx->gpr[n] as well). Here the write-through goes
away inside a function: a register write updates only its local, and every
local the function writes is stored back to the context (_SWB) right before
anything that can look at the context -- a call that takes ctx (lifted
functions, channel ops, SPU_HOP / SPU_RET, stop/halt), a `return`, and the
end of the function. Local-store accesses (spu_ls_read128/write128 ...) only
read ctx->ls and need nothing. Statements that take ctx keep their own
write-through, so the reload after them (_SRC_RL) reads current values.

With --merge-chunks the hot job loops are whole C functions, so their inner
loops now run entirely in registers.

Per function it is all or nothing: a function whose ctx statements are not
one-per-line (hand edits spanning lines, unbalanced braces) is left as is.

  spu_writeback.py in.c out.c
"""
import re, sys

FHEAD = re.compile(r'^void (\w+)\(spu_context\* ctx\) \{\n', re.M)
PURE = ('spu_ls_read128', 'spu_ls_write128', 'spu_ls_read32', 'spu_ls_write32',
        'spu_trace_pc', 'spu_trace_rt')
MARK = '/* spu writeback v189 */'
REGC = '/* spu regcache v142 */'
CTXRE = re.compile(r'\bctx\b(?!->)')
WT = re.compile(r'ctx->gpr\[(\d+)\] = (S_\1) = ')

def call_name(s, i):
    depth = 0; j = i - 1
    while j >= 0:
        c = s[j]
        if c == ')': depth += 1
        elif c == '(':
            if depth == 0:
                k = j - 1
                while k >= 0 and s[k] == ' ': k -= 1
                e = k + 1
                while k >= 0 and (s[k].isalnum() or s[k] == '_'): k -= 1
                return s[k + 1:e]
            depth -= 1
        j -= 1
    return ''

def needs_ctx(line):
    for m in CTXRE.finditer(line):
        if call_name(line, m.start()) not in PURE:
            return True
    return False

def transform(body):
    if 'u128 S_' not in body:
        return None
    lines = body.split('\n')
    out = []; W = set(); wrap = []
    for ln in lines:
        st = ln.strip()
        if st.startswith('u128 S_'):
            out.append(ln); wrap.append(False); continue
        c = needs_ctx(ln) or re.search(r'\breturn\b', ln) is not None
        if c:
            if ln.count('{') != ln.count('}') or ln.count('(') != ln.count(')'):
                return None
            if not st.endswith(';') and not st.endswith('}'):
                return None
            out.append(ln); wrap.append(True)
        else:
            if 'ctx->gpr' in ln and not WT.search(ln):
                return None                     # some other register access
            n2 = WT.sub(lambda m: (W.add(int(m.group(1))), m.group(2) + ' = ')[1], ln)
            if 'ctx->gpr' in n2:
                return None
            out.append(n2); wrap.append(False)
    if not W:
        return None
    res = []
    for ln, w in zip(out, wrap):
        if w:
            ind = ln[:len(ln) - len(ln.lstrip())]
            res.append(ind + '{ _SWB; ' + ln.strip() + ' }')
        else:
            res.append(ln)
    swb = ' '.join('ctx->gpr[%d] = S_%d;' % (n, n) for n in sorted(W))
    return '\n'.join(res), swb

def main(inp, outp):
    src = open(inp, encoding='utf-8', errors='surrogateescape').read()
    if MARK in src:
        print(inp, 'already transformed')
        if inp != outp: open(outp, 'w', encoding='utf-8', errors='surrogateescape').write(src)
        return
    if REGC not in src:
        sys.exit(inp + ': run spu_regcache.py first')
    res = []; pos = 0; ok = skip = 0
    for m in FHEAD.finditer(src):
        if m.start() < pos: continue
        end = src.find('\n}\n', m.end())
        if end < 0: break
        body = src[m.end():end + 1]
        r = transform(body)
        res.append(src[pos:m.start()])
        if r:
            nb, swb = r
            res.append('#define _SWB do { %s } while (0)\n' % swb)
            res.append(m.group(0) + nb + '        _SWB;\n}\n#undef _SWB\n')
            ok += 1
        else:
            res.append(src[m.start():end + 3])
            skip += 1
        pos = end + 3
    res.append(src[pos:])
    txt = ''.join(res)
    first_inc = txt.find('#include')
    eol = txt.find('\n', first_inc) + 1
    txt = txt[:eol] + MARK + '\n' + txt[eol:]
    open(outp, 'w', encoding='utf-8', errors='surrogateescape').write(txt)
    print(inp, {'write-back': ok, 'left as is': skip})

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
