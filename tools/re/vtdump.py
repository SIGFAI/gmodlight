import pefile, struct, sys
pe = pefile.PE(sys.argv[1], fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
vt = int(sys.argv[2], 16)
for i in range(int(sys.argv[3]) if len(sys.argv) > 3 else 8):
    f = struct.unpack_from('<Q', img, vt + 8*i)[0]
    print(i, hex(f - base))
