import pefile, struct, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
pe = pefile.PE('gamedll_x64_rwdi.dll', fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
lo, hi = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
names = {}
for m in re.finditer(rb'EDamageType::([A-Z0-9_]+)\x00', img):
    names[m.start()] = m.group(1).decode()
print(len(names), 'names')
# xrefs via lea reg,[rip+disp]
pat = re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', re.S)
md = Cs(CS_ARCH_X86, CS_MODE_64)
res = {}
for m in pat.finditer(img, lo, hi):
    i = m.start()
    t = i + 7 + struct.unpack_from('<i', img, i + 3)[0]
    if t in names:
        # look at the few instructions around for an immediate (enum value)
        ctx = list(md.disasm(img[i - 24:i + 40], base + i - 24))
        imms = [ins.op_str for ins in ctx if ins.mnemonic in ('mov',) and re.search(r', (0x[0-9a-f]+|\d+)$', ins.op_str) and 'ptr' not in ins.op_str.split(',')[0]]
        res.setdefault(names[t], set()).add(' | '.join(imms[:3]))
for n in sorted(res): print(n, list(res[n])[:2])
