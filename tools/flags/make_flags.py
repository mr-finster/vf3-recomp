#!/usr/bin/env python3
# Makes assets/ui/flags.png and flags.rcss: every country's flag (the two-letter ones of flag-icons, MIT, by
# Panayiotis Lipiridis), 48 x 36 each in rows of 20 with a pixel between (so none bleeds into the next scaled),
# rendered from its 4:3 SVG by a headless Chromium.
#   npm pack flag-icons@7.2.3 && mkdir x && tar xzf flag-icons-7.2.3.tgz -C x
#   tools/flags/make_flags.py x/package /path/to/chromium
import os, re, subprocess, sys
from PIL import Image
src, chromium = os.path.abspath(sys.argv[1]), sys.argv[2]
codes = sorted(f[:-4] for f in os.listdir(src + '/flags/4x3') if re.fullmatch(r'[a-z]{2}\.svg', f))
COLS, CW, CH = 20, 50, 38
W, H = COLS * CW, (len(codes) + COLS - 1) // COLS * CH
page = '/tmp/flags_atlas.html'
with open(page, 'w') as f:
    f.write('<!doctype html><html><head><style>html,body{margin:0;background:transparent}img{position:absolute;width:48px;height:36px}</style></head><body>')
    for i, c in enumerate(codes):
        f.write(f'<img src="file://{src}/flags/4x3/{c}.svg" style="left:{(i % COLS) * CW + 1}px;top:{(i // COLS) * CH + 1}px">')
    f.write('</body></html>')
subprocess.run([chromium, '--headless=new', '--no-sandbox', '--disable-gpu', '--hide-scrollbars', '--allow-file-access-from-files',
                '--default-background-color=00000000', f'--window-size={W},{H + 200}', '--screenshot=/tmp/flags_shot.png',   # (taller: headless takes some off the bottom)
                'file://' + page], check=True, capture_output=True)
atlas = Image.open('/tmp/flags_shot.png').convert('RGBA').crop((0, 0, W, H))
for i, c in enumerate(codes):   # (each drawn: none cut off, none failed to load)
    x, y = (i % COLS) * CW + 1, (i // COLS) * CH + 1
    if atlas.crop((x, y, x + 48, y + 36)).getextrema()[3][1] == 0: sys.exit(f'{c}: not drawn')
atlas.save('assets/ui/flags.png')
with open('assets/ui/flags.rcss', 'w') as f:
    f.write("/* Countries' flags (flag-icons by Panayiotis Lipiridis, MIT: assets/ui/flags-LICENSE.txt), 48 x 36 each in\n"
            "   flags.png, by their two letters: <img sprite=\"flag-au\"/>. Made by rendering its 4:3 SVGs (tools/flags). */\n"
            "@spritesheet flags {\n\tsrc: flags.png;\n")
    for i, c in enumerate(codes):
        f.write(f'\tflag-{c}: {(i % COLS) * CW + 1}px {(i // COLS) * CH + 1}px 48px 36px;\n')
    f.write('}\n')
with open('assets/ui/flags-LICENSE.txt', 'w') as f:
    f.write(open(src + '/LICENSE').read())
with open('src/host/flags.h', 'w') as f:   # (the list the game finds a flag's place by)
    f.write('// The countries with a flag in assets/ui/flags.png (made with it by tools/flags/make_flags.py), in its order: the\n'
            '// i-th\'s at (i % 20 * 50 + 1, i / 20 * 38 + 1), 48 x 36.\n#pragma once\n#include <string>\n\nnamespace flags {\n\n'
            'constexpr int COLS = 20, CELL_W = 50, CELL_H = 38, W = 48, H = 36;\ninline const char *const CODES =\n')
    for i in range(0, len(codes), 20):
        f.write('\t"' + ' '.join(codes[i:i + 20]) + (' ' if i + 20 < len(codes) else '') + '"' + (';' if i + 20 >= len(codes) else '') + '\n')
    f.write('\n// A country\'s place in the list (two letters, either case), or -1 if it has no flag\ninline int index_of(const std::string &code)\n{\n'
            '\tif (code.size() != 2) return -1;\n\tconst std::string c = { char(code[0] | 0x20), char(code[1] | 0x20) };\n\tconst std::string all = CODES;\n'
            '\tfor (size_t i = 0, n = 0; i + 1 < all.size(); i += 3, n++)\n\t\tif (all[i] == c[0] && all[i + 1] == c[1]) return int(n);\n\treturn -1;\n}\n\n} // namespace flags\n')
print(len(codes), 'flags')
