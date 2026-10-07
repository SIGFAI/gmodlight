import pefile, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
pe = pefile.PE(sys.argv[1], fast_load=False)
base = pe.OPTIONAL_HEADER.ImageBase
exp = {e.name.decode(): e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name} if hasattr(pe,'DIRECTORY_ENTRY_EXPORT') else {}
rev = {v: k for k, v in exp.items()}
def dis(rva, n=80):
    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = False
    code = pe.get_data(rva, n * 8)
    for i, ins in enumerate(md.disasm(code, base + rva)):
        tgt = ''
        if ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
            t = int(ins.op_str, 16) - base
            tgt = '  ; ' + rev.get(t, hex(t))
        print(f'{ins.address-base:8x}: {ins.mnemonic:6} {ins.op_str}{tgt}')
        if i >= n or ins.mnemonic == 'int3': break
target = sys.argv[2]
rva = exp[target] if target in exp else int(target, 16)
dis(rva, int(sys.argv[3]) if len(sys.argv) > 3 else 60)
