import pefile, struct, re, collections
pe = pefile.PE('gamedll_x64_rwdi.dll', fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
img = pe.get_memory_mapped_image()
imps = {i.address - base: i.name.decode() for d in pe.DIRECTORY_ENTRY_IMPORT for i in d.imports if i.name}
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
lo, hi = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
def cstr(rva):
    if not (0 < rva < len(img)): return None
    s = img[rva:rva + 100]; z = s.find(b'\0')
    if z <= 0: return None
    try:
        t = s[:z].decode('ascii'); return t if t.isprintable() else None
    except: return None
def lea_targets(win, start):
    out = {}
    for k in range(len(win) - 7):
        if win[k] in (0x48, 0x4c) and win[k + 1] == 0x8d and (win[k + 2] & 0xc7) == 0x05:
            reg = ((win[k + 2] >> 3) & 7) + (8 if win[k] == 0x4c else 0)
            out[reg] = start + k + 7 + struct.unpack_from('<i', win, k + 3)[0]
    return out
iat = {a: n for a, n in imps.items() if 'Method@@QEAA@PEAVCRTTI' in n or n.startswith('??0CRTTI@@')}
print({n[:60] for n in iat.values()})
calls = collections.defaultdict(list)
for m in re.finditer(rb'\xff\x15', img[lo:hi]):
    i = lo + m.start()
    t = i + 6 + struct.unpack_from('<i', img, i + 2)[0]
    if t in iat: calls[iat[t]].append(i)
for k, v in calls.items(): print(len(v), k[:90])
# methods: rdx = CRTTI*, r8 = name
by_class = collections.defaultdict(list)
for n, sites in calls.items():
    if 'Method@@QEAA@PEAVCRTTI' not in n: continue
    for c in sites:
        lt = lea_targets(img[c - 40:c], c - 40)
        name = cstr(lt.get(8, -1)); cls = lt.get(2)
        if name: by_class[cls].append(name)
print(sum(len(v) for v in by_class.values()), 'methods in', len(by_class), 'classes')
# class names: CRTTI objects are probably constructed with a name; look for lea rcx=<crtti> near a lea of a class-like string
crtti_names = {}
for n, sites in calls.items():
    if not n.startswith('??0CRTTI@@'): continue
    for c in sites:
        lt = lea_targets(img[c - 60:c], c - 60)
        obj = lt.get(1)
        for reg in (2, 8, 9):
            s = cstr(lt.get(reg, -1))
            if s and obj: crtti_names[obj] = s; break
print(len(crtti_names), 'named CRTTIs')
with open('methods_by_class.txt', 'w') as f:
    for cls, ms in sorted(by_class.items(), key=lambda kv: -len(kv[1])):
        f.write(f'== {crtti_names.get(cls, hex(cls) if cls else "?")} ({len(ms)})\n')
        for m in ms: f.write('   ' + m + '\n')

import json
want = ['ExplodingObject','Grenade','ThrowableObject','AI','HumanAI','Actor','PlayerDI','HumanAIVis','DeadRagdoll','ExplosionDamager','DamagingObject','Decoy','MolotovCocktail','Flare','DeveloperGrenade','PhysicsObject','DestroyablePhysicsObject','ThrowableRock']
rev = {}
for addr, nm in crtti_names.items():
    if nm in want: rev.setdefault(nm, []).append(hex(addr))
print(json.dumps(rev, indent=1))
