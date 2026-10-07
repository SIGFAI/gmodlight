import pefile, struct, re, sys
pe = pefile.PE('gamedll_x64_rwdi.dll', fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
iat = {}
for d in pe.DIRECTORY_ENTRY_IMPORT:
    for imp in d.imports:
        if imp.name and (b'Raytrace@IGSObject' in imp.name or b'Raytest@IGSObject' in imp.name):
            iat[imp.address - base] = imp.name.decode()
print({hex(k): v for k, v in iat.items()})
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
lo, hi = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
for m in re.finditer(rb'\xff\x15', img[lo:hi]):
    i = lo + m.start()
    t = i + 6 + struct.unpack_from('<i', img, i + 2)[0]
    if t in iat: print(hex(i), iat[t][:40])
