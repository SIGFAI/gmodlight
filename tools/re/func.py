import pefile, sys, bisect, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
pe = pefile.PE(sys.argv[1], fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
base = pe.OPTIONAL_HEADER.ImageBase
funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
starts = [f[0] for f in funcs]
md = Cs(CS_ARCH_X86, CS_MODE_64)
for a in sys.argv[2].split(','):
    rva = int(a, 16)
    i = bisect.bisect_right(starts, rva) - 1
    b, e = funcs[i]
    print(f'=== function {b:x}-{e:x} containing {rva:x} ({e-b} bytes)')
    lim = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    for ins in md.disasm(pe.get_data(b, min(e - b, lim * 8)), base + b):
        r = ins.address - base
        mark = ' <==' if r == rva else ''
        print(f'{r:8x}: {ins.mnemonic:7} {ins.op_str}{mark}')
        if r > e: break
