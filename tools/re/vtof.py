import pefile, struct, re, sys
pe = pefile.PE('gamedll_x64_rwdi.dll', fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
eng = pefile.PE('engine_x64_rwdi.dll', fast_load=True)
name = sys.argv[1].encode()
m = img.find(name)
td = m - 16
vts = []
for c in re.finditer(re.escape(struct.pack('<I', td)), img):
    col = c.start() - 12
    if struct.unpack_from('<I', img, col)[0] != 1: continue
    off = struct.unpack_from('<I', img, col + 4)[0]
    colva = base + col
    for v in re.finditer(re.escape(struct.pack('<Q', colva)), img):
        vts.append((off, v.start() + 8))
for off, vt in vts:
    s20 = struct.unpack_from('<Q', img, vt + 20*8)[0]
    print(f'vtable at {vt:#x} (subobject offset {off:#x}): slot20 -> {s20:#x} ({"gamedll+%#x" % (s20-base) if base <= s20 < base+len(img) else "outside (engine import?)"})')
