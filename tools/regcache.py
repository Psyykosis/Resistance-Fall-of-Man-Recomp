#!/usr/bin/env python3
"""Resistance port (v141): register caching for the lifted PPU code.

Every guest register lives in ppu_context (ctx->gpr[n], ->cr, ->lr, ->ctr).
Guest memory stores go through vm_base with strict aliasing off, so after
each one the compiler must assume ctx changed and re-read every register it
needs from memory -- each lifted instruction becomes a load/compute/store
chain through ctx. This pass gives each function local copies (R_n) that
the compiler keeps in host registers. Writes stay write-through
(ctx->gpr[n] = R_n = ...), so ctx is always current for anything that looks
at it (callees, HLE, the debugger, crash dumps); after every statement that
hands ctx to something else, the locals are re-read (_RCX_RL).

  regcache.py in.cpp out.cpp   -- a function it cannot prove simple is left as is
"""
import re, sys

FHEAD = re.compile(r'^void (func_[0-9A-Fa-f]+)\(ppu_context\* ctx\) \{\n', re.M)
FIELDS = r'(gpr\[(\d+)\]|cr|lr|ctr|xer)'

def lname(f):
    m = re.match(r'gpr\[(\d+)\]', f)
    return 'R_%s' % m.group(1) if m else 'R_' + f

def stmt_end(s, i):
    """index just past the ';' ending the statement containing position i"""
    depth = 0
    while i < len(s):
        c = s[i]
        if c in '([': depth += 1
        elif c in ')]': depth -= 1
        elif c == ';' and depth <= 0: return i + 1
        elif c in '{}' and depth <= 0: return -1
        i += 1
    return -1

def transform_body(name, body):
    # 1. bare ctx uses (ctx not followed by ->): find reload points
    ins = []
    for m in re.finditer(r'\bctx\b(?!->)|&\s*ctx->xer\b', body):
        i = m.start()
        # DRAIN_TRAMPOLINE(ctx) itself: reload after it
        e = stmt_end(body, i)
        if e < 0: return None, 'bare ctx outside a simple statement'
        rest = body[e:].lstrip()
        if rest.startswith('else'): return None, 'statement before else'
        if rest.startswith('return') or rest.startswith('DRAIN_TRAMPOLINE(ctx)'): continue
        ins.append((e, m.group(0).startswith('&')))
    # 2. fields used
    used = []
    for f in ['gpr[%d]' % n for n in range(32)] + ['cr', 'lr', 'ctr', 'xer']:
        if re.search(r'ctx->' + re.escape(f) + r'(?!\w)', body): used.append(f)
    if not used: return body, 'no registers'
    for m in re.finditer(r'&\s*ctx->(gpr\[\d+\]|cr|lr|ctr)(?!\w)', body):
        j = m.start() - 1
        while j >= 0 and body[j] in ' \t\n': j -= 1
        if j < 0 or body[j] in '(,={;:?!&|<>+-*/%^~': return None, 'address taken'
    if re.search(r'ctx->' + FIELDS + r'\s*(\+\+|--)', body): return None, 'post inc/dec'
    if re.search(r'ctx->gpr(?!\[\d+\])', body): return None, 'computed register index'
    if re.search(r'ctx->(cr|lr|ctr|xer)\w', body): pass
    if re.search(r'\bR_(\d+|cr|lr|ctr)\b', body): return None, 'name clash'
    # 3. insert reloads (from the back so offsets hold)
    full = set(e for e, x in ins if not x)
    xonly = set(e for e, x in ins if x) - full
    for e in sorted(full | xonly, reverse=True):
        body = body[:e] + (' _RCX_RL;' if e in full else ' R_xer = CTXRD_XER;') + body[e:]
    body = re.sub(r'&\s*ctx->xer\b', 'XERADDR_PLACEHOLDER', body)
    # 4. writes: pre inc/dec, then (compound) assignments -> write-through
    body = re.sub(r'(\+\+|--)\s*ctx->' + FIELDS + r'(?!\w)',
                  lambda m: '(CTXW_%s = %s%s)' % (m.group(2), m.group(1), lname(m.group(2))), body)
    body = re.sub(r'ctx->' + FIELDS + r'(?!\w)\s*(\+=|-=|\*=|\|=|&=|\^=|<<=|>>=|=(?!=))',
                  lambda m: 'CTXW_%s = %s %s' % (m.group(1), lname(m.group(1)), m.group(3)), body)
    # 5. reads
    body = re.sub(r'ctx->' + FIELDS + r'(?!\w)', lambda m: lname(m.group(1)), body)
    body = re.sub(r'CTXW_' + FIELDS, lambda m: 'ctx->' + m.group(1), body)
    body = body.replace('XERADDR_PLACEHOLDER', '&ctx->xer').replace('CTXRD_XER', 'ctx->xer')
    # 6. declarations + reload macro
    decl = []
    for f in used:
        t = 'uint32_t' if f in ('cr', 'xer') else 'uint64_t'
        decl.append('        %s %s = ctx->%s;\n' % (t, lname(f), f))
    rl = ' '.join('%s = ctx->%s;' % (lname(f), f) for f in used)
    return (''.join(decl), body, rl), 'ok'

def main(inp, out):
    src = open(inp, encoding='utf-8', errors='surrogateescape').read()
    if '/* regcache v141 */' in src:
        print(inp, 'already transformed'); open(out, 'w', encoding='utf-8', errors='surrogateescape').write(src); return
    res = []; pos = 0; stats = {}
    for m in FHEAD.finditer(src):
        if m.start() < pos: continue
        end = src.find('\n}\n', m.end())
        if end < 0: break
        body = src[m.end():end + 1]
        r, why = transform_body(m.group(1), body)
        stats[why] = stats.get(why, 0) + 1
        res.append(src[pos:m.start()])
        if isinstance(r, tuple):
            decl, nb, rl = r
            res.append('#define _RCX_RL do { %s } while (0)\n' % rl)
            res.append(m.group(0) + decl + nb + '}\n#undef _RCX_RL\n')
        else:
            res.append(src[m.start():end + 3])
        pos = end + 3
    res.append(src[pos:])
    txt = ''.join(res)
    txt = txt.replace('#include "ppu_recomp.h"\n', '#include "ppu_recomp.h"\n/* regcache v141 */\n', 1)
    open(out, 'w', encoding='utf-8', errors='surrogateescape').write(txt)
    print(inp, stats)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
