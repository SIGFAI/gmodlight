import pefile, struct, re, sys
pe = pefile.PE('gamedll_x64_rwdi.dll', fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
eng = pefile.PE('engine_x64_rwdi.dll', fast_load=True); ebase = eng.OPTIONAL_HEADER.ImageBase
def bases(name):
    m = img.find(name.encode() + b'\0')
    td = m - 16
    for c in re.finditer(re.escape(struct.pack('<I', td)), img):
        col = c.start() - 12
        if struct.unpack_from('<I', img, col)[0] != 1: continue
        chd = struct.unpack_from('<I', img, col + 16)[0]
        n = struct.unpack_from('<I', img, chd + 8)[0]
        bca = struct.unpack_from('<I', img, chd + 12)[0]
        out = []
        for i in range(min(n, 80)):
            bcd = struct.unpack_from('<I', img, bca + 4 * i)[0]
            tdr, nc, mdisp, pdisp, vdisp, attr = struct.unpack_from('<IIiiiI', img, bcd)
            s = img[tdr + 16: tdr + 90]; out.append((mdisp, s[:s.index(b'\0')].decode()))
        return out
for cls in sys.argv[1:]:
    b = bases(cls)
    print('==', cls, len(b), 'bases')
    for mdisp, n in sorted(set(b))[:60]:
        if mdisp in (0, 0x18, 0x28) or 'IControl' in n or 'IModel' in n or 'IGS' in n or 'CRTTI' in n:
            print(f'   +{mdisp:#x} {n}')
