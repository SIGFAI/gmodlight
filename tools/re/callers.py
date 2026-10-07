import pefile, struct, re, sys, bisect
pe = pefile.PE('gamedll_x64_rwdi.dll', fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
starts = [f[0] for f in funcs]
img = pe.get_memory_mapped_image()
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
lo, hi = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
target = int(sys.argv[1], 16)
hits = []
for m in re.finditer(rb'\xe8', img[lo:hi]):
    i = lo + m.start()
    if i + 5 + struct.unpack_from('<i', img, i + 1)[0] == target: hits.append(i)
# also as a pointer in data (vtables)
ptr = struct.pack('<Q', base + target)
data_refs = [m.start() for m in re.finditer(re.escape(ptr), img)]
for h in hits:
    k = bisect.bisect_right(starts, h) - 1
    print('call at', hex(h), 'in function', hex(funcs[k][0]))
print('pointer refs', [hex(x) for x in data_refs][:10])
