import pefile, struct, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
pe = pefile.PE('gamedll_x64_rwdi.dll', fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
sites = [0x2772d1,0x27749f,0x277abb,0x27c50d,0x27cb27,0x27d086,0x27d140,0x27d2df,0x27fe9d,0x28ace5,0x28b480,0x28cff3,0x2946eb,0x2afd8d,0x2b07f6,0x2b0983,0x2b0b44,0x2b5ec0,0x2b5fe5,0x2b65e4,0x2b7dea,0x2bc920,0x2bcab3,0x2be1d1,0x2caf19,0x2cb500,0x2cf65d,0x2f59ef,0x300c43,0x302737,0x3074ed,0x307706,0x308686,0x4a1937,0x5e0305,0x5f9196,0x5f9535,0x6e8835,0x6e8d7d,0x6e96b4,0x76d8c9,0x9ede5b,0xa0f0ec,0xa0f17c,0xa40aab,0xa4122b,0xa4b665,0xa4b7ad,0xa81c5c,0xa9a8c0,0xaa2441,0xab1349,0xac39e7,0xac47ec,0xac4e58,0xac5c38,0xae625d,0xaf5294,0xb0a56b,0xb0f90d,0xb15d24,0xba471a,0xba960a,0xbc545c,0xbc56ff,0xbc5960,0xbc5b2b,0xbc6916,0xbca040,0xbdc2b9,0xbdc8d5,0xc4dd70,0xc4ec70,0xc4fabc,0xcdea8d,0xce3e0e,0xce4e93,0xce8a2d,0xceb97f,0xcf6a31,0xcfe218,0xd127a7,0xd24a15,0xd251ff,0xd25a18,0xd25fc3,0xd26e61,0xd274d4,0xd28675,0xd337a7,0xd4e036,0xd666b9,0xd6ebac,0xd79c57,0xd8311e,0xd85cc7,0xd8a6ec,0xd8ab2e,0xd95b73,0xdb6029]
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
from collections import Counter
types = Counter(); examples = {}
for s in sites:
    stores = []
    for ins in md.disasm(img[s - 0x80:s + 0x200], base + s - 0x80):
        if ins.mnemonic == 'mov' and ins.op_str.startswith('dword ptr [') and ', 0x' in ins.op_str or (ins.mnemonic=='mov' and ins.op_str.startswith('dword ptr [') and ins.op_str.split(', ')[-1].lstrip('-').isdigit()):
            stores.append(ins.op_str)
    # find store of 0xffffffff followed by a store to [same base + 4]
    for i, st in enumerate(stores):
        m = re.match(r'dword ptr \[(\w+) ([+-]) (0x[0-9a-f]+)\], 0xffffffff', st)
        if not m: continue
        reg, sign, off = m.group(1), m.group(2), int(m.group(3), 16)
        off = off if sign == '+' else -off
        for st2 in stores[i+1:i+6]:
            m2 = re.match(r'dword ptr \[(\w+) ([+-]) (0x[0-9a-f]+)\], (\S+)', st2)
            if m2 and m2.group(1) == reg:
                o2 = int(m2.group(3), 16) * (1 if m2.group(2) == '+' else -1)
                if o2 == off + 4:
                    v = m2.group(4)
                    types[v] += 1; examples.setdefault(v, []).append(hex(s))
                    break
        break
for v, n in types.most_common(): print(v, n, examples[v][:6])
