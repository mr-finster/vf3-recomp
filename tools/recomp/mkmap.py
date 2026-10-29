#!/usr/bin/env python3
# Writes the recompiler's address map (src/rec/vf3_map.cpp) from merged coverage runs (tools/ref/covmerge.py
# --map) and the ROM: the call targets, the indirect jumps' and calls' targets and the interrupt entries, then
# every instruction seen running that a walk from those (with the recompiler's own rules, src/rec/ppc_rec.cpp
# analyse()) doesn't reach: the places reached only by returning from an interrupt. Addresses only; the ROM is
# read here, on the developer's machine, and nothing from it goes into the map.
#   tools/recomp/mkmap.py <map folder> <vf3.zip> src/rec/vf3_map.cpp
import struct, sys, zipfile

LIMIT = 0xA0000   # the program the boot copies to RAM (from the fixed CROM's offset 0x710000)
REV_D = ['epr-19230d.20', 'epr-19229d.19', 'epr-19228d.18', 'epr-19227d.17']

crom, base = None, 0
def load(rom, names=REV_D):
    # the program image, as romset.cpp lays it out: the four program ROMs in 2-byte chunks every 8 bytes, swapped
    global crom, base
    z = zipfile.ZipFile(rom)
    files = [z.read(n) for n in names]
    crom = bytearray(len(files[0]) * 4)
    for k, f in enumerate(files):
        for i in range(0, len(f), 2):
            o = (i // 2) * 8 + k * 2
            crom[o] = f[i + 1]
            crom[o + 1] = f[i]
    base = 0x710000 - (0x800000 - len(crom))
def word(a): return struct.unpack('>I', crom[base + a:base + a + 4])[0]

OK31 = {0, 4, 8, 520, 10, 522, 11, 19, 20, 23, 24, 26, 28, 540, 32, 40, 552, 54, 86, 246, 278, 470, 1014, 55, 60, 75,
        83, 87, 104, 616, 119, 124, 136, 648, 138, 650, 144, 146, 150, 151, 183, 200, 712, 202, 714, 210, 215, 232,
        744, 234, 746, 235, 747, 242, 247, 266, 778, 279, 284, 306, 370, 566, 311, 316, 339, 343, 371, 375, 407, 412,
        439, 444, 459, 971, 467, 476, 491, 1003, 512, 533, 597, 534, 535, 567, 536, 595, 598, 599, 631, 659, 661, 725,
        662, 663, 695, 727, 759, 758, 790, 792, 824, 854, 918, 922, 954, 982, 983}
OK19 = {0, 16, 33, 50, 129, 150, 193, 225, 257, 289, 417, 449, 528}
OK63 = {0, 32, 12, 14, 15, 18, 20, 21, 22, 26, 38, 70, 40, 64, 72, 134, 136, 264, 583, 711}

def valid(op):   # as ppc_rec.cpp's valid()
    p, x = op >> 26, (op >> 1) & 0x3FF
    if p in (3, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 21, 23, 24, 25, 26, 27, 28, 29): return True
    if p == 19: return x in OK19
    if p == 31: return x in OK31
    if p == 59: return (x & 31) == 25 or (x & 31) >= 28 or x in (18, 20, 21, 22, 24)
    if p == 63: return (x & 31) in (23, 25) or (x & 31) >= 28 or x in OK63
    return 32 <= p <= 55

def sx16(v): return v - 0x10000 if v & 0x8000 else v

def walk(seeds):   # as ppc_rec.cpp's analyse()
    seen = set()
    work = list(seeds) + list(range(0x100, 0x1500, 0x100))
    while work:
        a = work.pop()
        while 0 <= a < LIMIT and not a & 3 and a not in seen:
            op = word(a)
            if not valid(op): break
            seen.add(a)
            p = op >> 26
            if p == 18:
                li = op & 0x3FFFFFC
                if li & 0x2000000: li -= 0x4000000
                work.append((li + (0 if op & 2 else a)) & 0xFFFFFFFF)
                if not op & 1: break
            elif p == 16:
                work.append((sx16(op & 0xFFFC) + (0 if op & 2 else a)) & 0xFFFFFFFF)
                if ((op >> 21) & 0x14) == 0x14 and not op & 1: break
            elif p == 19:
                x = (op >> 1) & 0x3FF
                if x in (16, 528) and ((op >> 21) & 0x14) == 0x14 and not op & 1: break
                if x == 50: break
            a += 4
    return seen

if __name__ == '__main__':   # (imported, mklearnt.py: the functions above only)
    mapdir, rom, out_path = sys.argv[1:4]
    load(rom)
    flags = open(mapdir + '/flags_ram.bin', 'rb').read()
    seeds = set()
    for i in range(LIMIT // 4):
        if flags[i] & (2 | 16):                                # call targets, interrupt entries
            seeds.add(i * 4)
    for line in open(mapdir + '/indirect.txt'):               # indirect jumps' and calls' targets (not rfi's)
        kind, src, dst, n = line.split()
        if kind not in ('rfi', 'interrupt') and int(dst, 16) < LIMIT:
            seeds.add(int(dst, 16))
    ran = {i * 4 for i in range(LIMIT // 4) if flags[i] & 1}
    reached = walk(seeds)
    extra = sorted(ran - reached)
    # each piece the walk missed needs only its first instruction: walk again with each added
    for a in extra:
        if a not in reached:
            seeds.add(a)
            reached |= walk([a])
    missing = ran - reached
    seeds = sorted(seeds)
    with open(out_path, 'w') as out:
        out.write('// The recompiler\'s address map: where VF3\'s code starts (calls, indirect jumps\' targets, interrupt\n')
        out.write('// entries, places reached only by returning from an interrupt), from the coverage runs of\n')
        out.write('// docs/step1-measure.md. Made by tools/recomp/mkmap.py. Addresses only.\n')
        out.write('#include "ppc_rec.h"\n\nnamespace ppcrec {\n\nconst uint32_t vf3_map[] = {\n')
        for k in range(0, len(seeds), 10):
            out.write('\t' + ', '.join('0x%05X' % a for a in seeds[k:k + 10]) + ',\n')
        out.write('};\nconst size_t vf3_map_count = sizeof(vf3_map) / sizeof(vf3_map[0]);\n\n} // namespace ppcrec\n')
    print('%d addresses; %d instructions found, %d of the %d seen running, %d not' % (len(seeds), len(reached), len(ran & reached), len(ran), len(missing)))
