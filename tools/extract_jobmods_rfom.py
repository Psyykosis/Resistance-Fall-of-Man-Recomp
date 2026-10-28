# Extract WWS job modules (C0DEC0DE header) embedded raw in Resistance FoM's EBOOT.elf data segment.
import struct,re,hashlib,sys,os
d=open(sys.argv[1],'rb').read(); out=sys.argv[2]; os.makedirs(out,exist_ok=True)
starts=[m.start()-0x20 for m in re.finditer(b'\xC0\xDE\xC0\xDE',d)]
for i,s in enumerate(starts):
    ent,size=struct.unpack_from('>II',d,s+0x10)
    gap=(starts[i+1] if i+1<len(starts) else 0x911a80)-s   # PM begins at file 0x911a80
    raw=d[s:s+min(size,gap)]
    open(f"{out}/jobmod_{hashlib.sha1(raw[:16]).hexdigest()[:12]}_e{ent:X}.bin",'wb').write(raw)
