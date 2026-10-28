#!/usr/bin/env python3
"""Minimal PS3 SELF -> ELF decryptor (disc/APP SELFs).

    python tools/unself.py --keys key_vault.cpp [--out DIR] EBOOT.BIN game.self

The application keys are parsed from RPCS3's public key_vault.cpp (Setup.bat
downloads it); this repository contains no keys. Each output is written as
<name>.elf, next to its input or into --out.
"""
import argparse, os, re, struct, sys, zlib
from Crypto.Cipher import AES
from Crypto.Util import Counter

def load_app_keys(path):
    src = open(path, encoding='utf-8', errors='replace').read()
    keys = {}
    pat = re.compile(r'sk_APP_arr\.emplace_back\((0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*KEY_APP,\s*"([0-9A-F]*)",\s*"([0-9A-F]*)"', re.S)
    for m in pat.finditer(src):
        keys[int(m.group(3), 16)] = (bytes.fromhex(m.group(4)), bytes.fromhex(m.group(5)))
    if not keys:
        raise SystemExit('no application keys found in ' + path)
    return keys

def u(fmt, b, o): return struct.unpack_from('>' + fmt, b, o)

def ctr(key, iv):
    return AES.new(key, AES.MODE_CTR, counter=Counter.new(128, initial_value=int.from_bytes(iv, 'big')))

def unself(data, keys, verbose=True):
    magic, hver, keyrev, setype, meta, hsize, esize = u('IIHHIQQ', data, 0)
    assert magic == 0x53434500, 'not SCE'
    ext = u('QQQQQQQQQQ', data, 0x20)
    _, appinfo_off, ehdr_off, phdr_off, shdr_off, segext_off, ver_off, supp_off, supp_size, _ = ext
    authid, vendor, self_type, version = u('QIIQ', data, appinfo_off)
    if verbose:
        print(f'  keyrev={keyrev:#x} self_type={self_type} authid={authid:#018x} fw_version={version:#018x}')
    assert self_type == 4, f'self_type {self_type} (only APP handled)'
    erk, riv = keys[keyrev]
    mi_off = meta + 0x20
    mi = AES.new(erk, AES.MODE_CBC, riv).decrypt(data[mi_off:mi_off + 0x40])
    mkey, miv = mi[0:16], mi[32:48]
    assert mi[16:32] == b'\0' * 16 and mi[48:64] == b'\0' * 16, 'metadata info decrypt failed (wrong key?)'
    rest = ctr(mkey, miv).decrypt(data[mi_off + 0x40:hsize])
    _, _, nsec, nkey, _, _, _ = u('QIIIIII', rest, 0)
    secs = [u('QQIIIIIIII', rest, 0x20 + i * 0x30) for i in range(nsec)]
    kb = rest[0x20 + nsec * 0x30:]
    dkeys = [kb[i * 16:(i + 1) * 16] for i in range(nkey)]

    ehdr = bytearray(data[ehdr_off:ehdr_off + 0x40])
    assert ehdr[:4] == b'\x7fELF'
    e_phoff, e_shoff = u('QQ', ehdr, 0x20)
    e_phentsize, e_phnum, e_shentsize, e_shnum = u('HHHH', ehdr, 0x36)
    phdrs = [data[phdr_off + i * 0x38: phdr_off + (i + 1) * 0x38] for i in range(e_phnum)]
    out = bytearray(max(e_phoff + e_phnum * 0x38, 0x40))
    def put(off, b):
        if len(out) < off + len(b): out.extend(b'\0' * (off + len(b) - len(out)))
        out[off:off + len(b)] = b
    put(0, ehdr)
    for i, p in enumerate(phdrs): put(e_phoff + i * 0x38, p)
    for (doff, dsize, typ, pidx, hashed, sha1, enc, kidx, ividx, comp) in secs:
        if typ != 2:  # 2 = program segment
            continue
        blob = data[doff:doff + dsize]
        if enc == 3:
            blob = ctr(dkeys[kidx], dkeys[ividx]).decrypt(blob)
        if comp == 2:
            blob = zlib.decompress(blob)
        p_offset = u('Q', phdrs[pidx], 8)[0]
        put(p_offset, blob)
    if e_shnum and shdr_off:
        put(e_shoff, data[shdr_off:shdr_off + e_shnum * e_shentsize])
    else:
        struct.pack_into('>QHH', out, 0x28, 0, 0, 0)  # e_shoff, (flags ok) - zero section info
        struct.pack_into('>HHH', out, 0x3a, 0, 0, 0)
    return bytes(out)

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--keys', required=True, help="RPCS3's key_vault.cpp")
    ap.add_argument('--out', default=None, help='output directory (default: next to each input)')
    ap.add_argument('selfs', nargs='+')
    a = ap.parse_args()
    keys = load_app_keys(a.keys)
    for path in a.selfs:
        print(path)
        elf = unself(open(path, 'rb').read(), keys)
        name = os.path.splitext(os.path.basename(path))[0] + '.elf'
        outp = os.path.join(a.out, name) if a.out else os.path.join(os.path.dirname(path), name)
        open(outp, 'wb').write(elf)
        print(f'  -> {outp} ({len(elf)} bytes)')
