#!/usr/bin/env python3
# Writes a recompiler address map from addresses the game was seen running on the interpreter for want of one
# (vf3run's VF3_MAP_OUT: each run's list, learnt until a run adds none) instead of from Supermodel's coverage runs:
# as mkmap.py does, only the ones the recompiler's walk from the others doesn't reach are kept. Addresses only; the
# ROM is read here, on the developer's machine, and nothing from it goes into the map.
#   tools/recomp/mklearnt.py <learnt.txt> <zip> <set> src/rec/<set>_map.cpp
# <set>: vf3tb (Team Battle's program ROMs) or vf3.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkmap

PROGRAM = {'vf3': mkmap.REV_D, 'vf3tb': ['epr-20129.20', 'epr-20128.19', 'epr-20127.18', 'epr-20126.17']}
learnt_path, rom, set_name, out_path = sys.argv[1:5]
mkmap.load(rom, PROGRAM[set_name])
learnt = sorted({int(l, 16) for l in open(learnt_path) if l.strip()} | {0x100, 0x500, 0x900})
learnt = [a for a in learnt if a < mkmap.LIMIT]
seeds, reached = [], set()
for a in learnt:   # (in address order: a routine's start before the places inside it)
    if a not in reached:
        seeds.append(a)
        reached |= mkmap.walk([a])
name = set_name + '_map'
with open(out_path, 'w') as out:
    out.write('// The recompiler\'s address map for %s: where its code starts, learnt from the addresses it ran on the\n' % set_name)
    out.write('// interpreter for want of one (vf3run VF3_MAP_OUT, runs of tools/ref/gen_scripts.py\'s scripts until one adds\n')
    out.write('// none). Made by tools/recomp/mklearnt.py. Addresses only.\n')
    out.write('#include "ppc_rec.h"\n\nnamespace ppcrec {\n\nconst uint32_t %s[] = {\n' % name)
    for k in range(0, len(seeds), 10):
        out.write('\t' + ', '.join('0x%05X' % a for a in seeds[k:k + 10]) + ',\n')
    out.write('};\nconst size_t %s_count = sizeof(%s) / sizeof(%s[0]);\n\n} // namespace ppcrec\n' % (name, name, name))
missing = [a for a in learnt if a not in reached]
print('%d addresses kept of %d learnt; %d instructions found, %d learnt not' % (len(seeds), len(learnt), len(reached), len(missing)))
