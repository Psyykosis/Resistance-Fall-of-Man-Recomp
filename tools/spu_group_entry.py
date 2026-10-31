#!/usr/bin/env python3
"""Resistance port (v191): enter merged SPU function groups in the middle.

--merge-chunks lifts each run of chunks as ONE C function with gotos, and
still emits every chunk as its own function so an indirect branch can land
on any of them. But once execution lands on such a per-chunk function, every
branch from there goes to the next per-chunk function: the job never comes
back into the merged group, and its inner loop runs chunk by chunk -- each
hop reloading and storing back every register it touches. Job 6dc227fd's
loop (0x4218..0x43D8) ran that way: an indirect branch enters at 0x40D8, and
func_000042C8 alone was 15% of two SPURS lanes in Cheddar Gorge.

This pass, run after spu_regcache / spu_directhop / spu_writeback:
  * every per-chunk function whose address is also a label inside a merged
    group becomes a stub that records the address in g_spu_grp_entry and
    hops to the group;
  * every such group gets a dispatch right after its register loads: when
    g_spu_grp_entry is set it is cleared and the group jumps to that label.

  spu_group_entry.py in.c out.c
"""
import re, sys

FHEAD = re.compile(r'^void (\w+_spu_func_)([0-9A-F]{8})\(spu_context\* ctx\) \{\n', re.M)
MARK = '/* spu group entry v191 */'
LOAD = re.compile(r'^\s+u128 S_\d+ = ctx->gpr\[\d+\];$')


def main(inp, outp):
    src = open(inp, encoding='utf-8', errors='surrogateescape').read()
    if MARK in src:
        print(inp, 'already transformed')
        if inp != outp:
            open(outp, 'w', encoding='utf-8', errors='surrogateescape').write(src)
        return
    funcs = []   # (head_start, body_start, body_end, prefix, addr)
    for m in FHEAD.finditer(src):
        end = src.find('\n}\n', m.end())
        if end < 0:
            break
        funcs.append([m.start(), m.end(), end + 1, m.group(1), m.group(2)])
    names = {f[4] for f in funcs}
    # label -> group function (first group that holds it)
    owner = {}
    for f in funcs:
        body = src[f[1]:f[2]]
        for lab in re.findall(r'^loc_([0-9A-F]{8}):', body, re.M):
            if lab != f[4] and lab in names and lab not in owner:
                owner[lab] = f[4]
    if not owner:
        print(inp, 'no merged groups with inner entries')
        if inp != outp:
            open(outp, 'w', encoding='utf-8', errors='surrogateescape').write(src)
        return
    entries = {}
    for lab, g in owner.items():
        entries.setdefault(g, []).append(lab)
    prefix = funcs[0][3]
    out = []; pos = 0; n_stub = n_grp = 0
    for f in funcs:
        hs, bs, be, pf, addr = f
        out.append(src[pos:bs])
        body = src[bs:be]
        if addr in owner:
            g = owner[addr]
            out.append(
                '        /* v191: inside merged group 0x%s -- run it there */\n'
                '        g_spu_grp_entry = 0x%su;\n'
                '        ctx->pc = 0x%s;\n'
                '        if (__builtin_expect(spu_hop_direct_ok(ctx, 0x%s), 1)) {\n'
                '            ctx->steps++;\n'
                '            __attribute__((musttail)) return %s%s(ctx);\n'
                '        }\n'
                '        g_spu_trampoline_fn = %s%s;\n'
                '        return;\n' % (g, addr, addr, addr, pf, g, pf, g))
            n_stub += 1
        elif addr in entries:
            lines = body.split('\n')
            k = 0
            while k < len(lines) and LOAD.match(lines[k]):
                k += 1
            cases = ' '.join('case 0x%su: goto loc_%s;' % (e, e) for e in sorted(entries[addr]))
            disp = ('        if (__builtin_expect(g_spu_grp_entry != 0, 0)) { const uint32_t _ge = g_spu_grp_entry; '
                    'g_spu_grp_entry = 0; if ((ctx->pc & 0x3FFFFu) == _ge) switch (_ge) { %s default: break; } }' % cases)
            lines.insert(k, disp)
            out.append('\n'.join(lines))
            n_grp += 1
        else:
            out.append(body)
        pos = be
    out.append(src[pos:])
    txt = ''.join(out)
    first_inc = txt.find('#include')
    eol = txt.find('\n', first_inc) + 1
    txt = (txt[:eol] + MARK + '\n'
           '#if defined(_MSC_VER) && !defined(__clang__)\n'
           '#  error "spu group entry needs clang (musttail)"\n'
           '#endif\n'
           'extern SPU_THREAD_LOCAL uint32_t g_spu_grp_entry;\n' + txt[eol:])
    open(outp, 'w', encoding='utf-8', errors='surrogateescape').write(txt)
    print(inp, {'stubs': n_stub, 'groups with entries': n_grp})


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
