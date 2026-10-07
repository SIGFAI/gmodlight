import pefile, struct, re, sys
def load(name):
    pe = pefile.PE(name, fast_load=True)
    data = pe.get_memory_mapped_image()
    return pe, data
for name in ['engine_x64_rwdi.dll', 'gamedll_x64_rwdi.dll']:
    pe, img = load(name)
    base = pe.OPTIONAL_HEADER.ImageBase
    for m in re.finditer(rb'\.\?A[UV]\w*Damage\w*@@', img):
        td = m.start() - 16  # TypeDescriptor: vftable ptr, spare, name
        # find COLs referencing this TD (x64: sig=1, ..., typeDescriptor RVA at +12)
        tdrva = td
        cols = [c.start() for c in re.finditer(re.escape(struct.pack('<I', tdrva)), img)]
        vts = []
        for c in cols:
            col = c - 12
            if struct.unpack_from('<I', img, col)[0] != 1: continue
            colva = base + col
            for v in re.finditer(re.escape(struct.pack('<Q', colva)), img):
                vts.append(hex(v.start() + 8))
        print(name, m.group().decode(), 'td', hex(tdrva), 'vtables', vts[:3])
