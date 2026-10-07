import pefile, struct, re, sys
pe = pefile.PE(sys.argv[1], fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
target = int(sys.argv[2], 16)
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
lo, hi = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
pat = re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', re.S)
hits = []
for m in pat.finditer(img, lo, hi):
    i = m.start()
    disp = struct.unpack_from('<i', img, i + 3)[0]
    if i + 7 + disp == target: hits.append(i)
print([hex(h) for h in hits])
