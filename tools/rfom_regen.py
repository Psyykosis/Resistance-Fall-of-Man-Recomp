#!/usr/bin/env python3
"""Resistance: Fall of Man port -- regenerate all game-derived code from the
player's own decrypted EBOOT.elf and game.elf.

    python tools/rfom_regen.py --eboot <EBOOT.elf> --game <game.elf> [--port <port dir>]

Writes, inside the port folder:
    recompiled/          front end (EBOOT)      PPU code
    recompiled_game/     game (game.self)       PPU code
    project/spu/<mod>/   SPU job modules, job manager, raw SPU programs
Nothing here is taken from the game except through the lifters run on the
player's own files; tools/hooks/ holds the port's own edits.
"""
import argparse, glob, json, os, shutil, struct, subprocess, sys, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.stderr.write(r.stdout[-2000:] + r.stderr[-4000:])
        raise SystemExit('FAILED: ' + ' '.join(cmd[:4]))
    return r.stdout

def ppu(port, tk, elf, outdir, name, chunks, post_passes, hooks, work):
    print('[%s] lifting PPU code...' % name, flush=True)
    od = os.path.join(work, name + '_an')
    run([sys.executable, os.path.join(tk, 'ppu_loader.py'), elf, '-o', od])
    stem = os.path.splitext(os.path.basename(elf))[0]
    tmp = os.path.join(work, name)
    if os.path.isdir(tmp): shutil.rmtree(tmp)
    run([sys.executable, os.path.join(tk, 'ppu_lifter.py'), elf, '--functions', os.path.join(od, stem + '.functions.json'),
         '--hle-stubs', os.path.join(od, stem + '.imports.json'), '--chunks', str(chunks), '--output', tmp])
    chunksf = sorted(glob.glob(os.path.join(tmp, 'ppu_recomp_0*.cpp')))
    if post_passes:
        print('[%s] register cache + fast memory passes...' % name, flush=True)
        for f in chunksf:
            run([sys.executable, os.path.join(HERE, 'regcache.py'), f, f])
            run([sys.executable, os.path.join(HERE, 'ppu_fastmem.py'), f, f])
        # the function table is its own file in this lifter version: give it the
        # same vm_base declaration guard ppu_fastmem.py gives the chunks
        tf = os.path.join(tmp, 'ppu_recomp_table.cpp')
        if os.path.exists(tf):
            s = open(tf, encoding='utf-8', errors='surrogateescape').read()
            s = s.replace('\nextern "C" uint8_t* vm_base;\n', '\n#ifndef PPU_FIXED_BOUNDS   /* v163: ppu_recomp.h declares it const */\nextern "C" uint8_t* vm_base;\n#endif\n', 1)
            open(tf, 'w', encoding='utf-8', errors='surrogateescape').write(s)
    run([sys.executable, os.path.join(HERE, 'rfom_hooks.py'), 'apply', tmp, hooks])
    shutil.copy(os.path.join(HERE, 'hooks', 'ppu_hle_nids.cpp'), os.path.join(tmp, 'ppu_hle_nids.cpp'))
    dst = os.path.join(port, outdir)
    if os.path.isdir(dst): shutil.rmtree(dst)
    shutil.copytree(tmp, dst)

def spu_post(f, hooks):
    run([sys.executable, os.path.join(HERE, 'rfom_spu_hooks.py'), 'apply', 'pre', f, hooks])
    run([sys.executable, os.path.join(HERE, 'spu_regcache.py'), f, f])
    run([sys.executable, os.path.join(HERE, 'spu_directhop.py'), f])
    run([sys.executable, os.path.join(HERE, 'rfom_spu_hooks.py'), 'apply', 'post', f, hooks])

def fix_includes(d):
    for f in glob.glob(os.path.join(d, 'spu_recomp.*')):
        s = open(f, encoding='utf-8', errors='surrogateescape').read().replace('"../../runtime/spu/', '"')
        open(f, 'w', encoding='utf-8', errors='surrogateescape').write(s)

def spu(port, tk, eboot, game, work, stage_raw=None):
    sys.path.insert(0, tk)
    from wrap_spu_elf import wrap
    spudir = os.path.join(port, 'project', 'spu')
    hk = os.path.join(HERE, 'hooks', 'spu')
    elfs = {'EBOOT': open(eboot, 'rb').read(), 'game': open(game, 'rb').read()}
    # 1. WWS job modules (C0DEC0DE images in game.elf's data)
    print('[spu] extracting + lifting job modules...', flush=True)
    jm = os.path.join(work, 'jobmods'); out = os.path.join(work, 'spu')
    for d in (jm, out):
        if os.path.isdir(d): shutil.rmtree(d)
    run([sys.executable, os.path.join(HERE, 'extract_jobmods_rfom.py'), game, jm])
    env = dict(os.environ, PYTHONPATH=tk)
    run([sys.executable, os.path.join(HERE, 'lift_jobmods_base.py'), '--jobmods', jm, '--out', out, '--register', os.path.join(work, 'jobmods_register.c')], env=env)
    mods = sorted(os.path.basename(d) for d in glob.glob(os.path.join(out, 'jobmod_*')))
    shutil.copy(os.path.join(work, 'jobmods_register.c'), os.path.join(spudir, 'jobmods_register.c'))
    # 2. job manager (policy module) and the two raw SPU programs
    spec = json.load(open(os.path.join(HERE, 'rfom_spu_specs.json')))
    for n, s in spec.items():
        print('[spu] lifting %s...' % n, flush=True)
        src = elfs[s['elf']]
        raw = b'\0' * s['pad'] + src[s['off'] + s['pad']:s['off'] + s['size']]
        os.makedirs(os.path.join(work, n), exist_ok=True)
        pe = os.path.join(work, n, 'p.elf'); open(pe, 'wb').write(wrap(raw, base=s['base'], entry=s['entry']))
        run([sys.executable, os.path.join(tk, 'spu_lifter.py'), '--auto-functions', pe, '--base', hex(s['base']),
             '--symbol-prefix', n + '_', '-o', os.path.join(out, n), '--source-name', 'spu_recomp.c',
             '--header-name', 'spu_recomp.h', '--extra-funcs', ','.join(s['funcs'])], env=env)
        mods.append(n)
    print('[spu] port edits + register cache...', flush=True)
    for n in mods:
        d = os.path.join(out, n); fix_includes(d)
        if stage_raw is not None: shutil.copytree(d, os.path.join(stage_raw, n))
        hp = os.path.join(hk, n + '.json')
        if os.path.exists(hp): spu_post(os.path.join(d, 'spu_recomp.c'), hp)
        else:
            f = os.path.join(d, 'spu_recomp.c')
            run([sys.executable, os.path.join(HERE, 'spu_regcache.py'), f, f]); run([sys.executable, os.path.join(HERE, 'spu_directhop.py'), f])
            if n.startswith('jobmod_'):   # v189: registers stay in locals inside a function
                run([sys.executable, os.path.join(HERE, 'spu_writeback.py'), f, f])
                run([sys.executable, os.path.join(HERE, 'spu_group_entry.py'), f, f])   # v191: enter merged groups mid-way
        dst = os.path.join(spudir, n)
        if os.path.isdir(dst): shutil.rmtree(dst)
        shutil.copytree(d, dst)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--eboot', required=True); ap.add_argument('--game', required=True)
    ap.add_argument('--port', default=os.path.dirname(HERE)); ap.add_argument('--work', default=None)
    ap.add_argument('--only', choices=['ppu', 'spu', 'eboot', 'gameppu'], default=None)
    ap.add_argument('--stage-raw', default=None, help=argparse.SUPPRESS)
    a = ap.parse_args()
    port = os.path.abspath(a.port); tk = os.path.join(port, 'ps3recomp', 'tools')
    work = os.path.abspath(a.work or os.path.join(port, 'build_regen'))
    os.makedirs(work, exist_ok=True)
    H = os.path.join(HERE, 'hooks')
    if a.only in (None, 'ppu', 'eboot'):
        ppu(port, tk, a.eboot, 'recompiled', 'eboot', 15, False, os.path.join(H, 'eboot.json'), work)
    if a.only in (None, 'ppu', 'gameppu'):
        ppu(port, tk, a.game, 'recompiled_game', 'game', 13, True, os.path.join(H, 'game.json'), work)
    if a.only in (None, 'spu'):
        spu(port, tk, a.eboot, a.game, work, a.stage_raw)
    print('done: game code regenerated.')

if __name__ == '__main__':
    main()
