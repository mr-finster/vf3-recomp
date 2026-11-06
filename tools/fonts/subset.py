#!/usr/bin/env python3
# The menus' fonts for what Barlow Condensed hasn't got (assets/fonts/NotoSans*.ttf): each a subset of Noto Sans with
# only the letters the menus' strings use (src/host/lang/*.inc), so the download stays small. Run again after a
# translation adds a letter (tools/langtest says which are missing):
#   pip install fonttools
#   python3 tools/fonts/subset.py <folder with the Noto sources>
# The sources (SIL OFL 1.1), from https://github.com/google/fonts: ofl/notosanskr/NotoSansKR[wght].ttf,
# ofl/notosansjp/NotoSansJP[wght].ttf, ofl/notosanssc/NotoSansSC[wght].ttf, ofl/notosanstc/NotoSansTC[wght].ttf,
# ofl/notosans/NotoSans[wdth,wght].ttf and NotoSans-Italic[wdth,wght].ttf.
# Korean, Japanese and Chinese at two weights (500 for text, 700 for titles); Cyrillic condensed (wdth 75, as
# Barlow Condensed) at Barlow's three: 500, 600 and 700 italic. Japanese gets all of the kana too.
import os
import re
import sys
from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.normpath(os.path.join(here, '..', '..'))
lang_dir = os.path.join(root, 'src', 'host', 'lang')
out_dir = os.path.join(root, 'assets', 'fonts')
src = sys.argv[1] if len(sys.argv) > 1 else '.'

barlow = TTFont(os.path.join(out_dir, 'BarlowCondensed-SemiBold.ttf')).getBestCmap()
barlow_i = TTFont(os.path.join(out_dir, 'BarlowCondensed-BoldItalic.ttf')).getBestCmap()


def unquote(lit):
    """a C++ string literal's text: its \\xNN bytes and \\" \\\\ escapes, the rest UTF-8 as it is"""
    out, i = bytearray(), 0
    while i < len(lit):
        if lit[i] == '\\' and lit[i + 1] == 'x':
            out.append(int(lit[i + 2:i + 4], 16))
            i += 4
        elif lit[i] == '\\':
            out += lit[i + 1].encode('utf-8')
            i += 2
        else:
            out += lit[i].encode('utf-8')
            i += 1
    return out.decode('utf-8')


def letters(code):
    """the letters of a language's strings (the T(ID, "...") lines of its .inc) that Barlow hasn't got"""
    out = set()
    for line in open(os.path.join(lang_dir, code + '.inc'), encoding='utf-8'):
        if not line.lstrip().startswith('T('):
            continue
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', line):
            out.update(ord(c) for c in unquote(lit))
    return {c for c in out if c >= 0x80 and not (c in barlow and c in barlow_i)}


# each language's own name, as Game > Language lists it (src/host/i18n.cpp's info)
NAMES = {'KR': '한국어', 'JP': '日本語', 'SC': '简体中文', 'TC': '繁體中文', 'Cyr': 'Русский'}
KANA = set(range(0x3041, 0x3097)) | set(range(0x30A0, 0x3100)) | {0x3001, 0x3002, 0x300C, 0x300D, 0x30FB, 0x30FC}

sets = {
    'KR': letters('ko'),
    'JP': letters('ja') | KANA,
    'SC': letters('zh-Hans'),
    'TC': letters('zh-Hant'),
    'Cyr': letters('ru') | set(range(0x0410, 0x0450)) | {0x0401, 0x0451},
}
for k, n in NAMES.items():
    sets[k] |= {ord(c) for c in n}

jobs = [
    ('KR', 'NotoSansKR[wght].ttf', [('Medium', {'wght': 500}), ('Bold', {'wght': 700})]),
    ('JP', 'NotoSansJP[wght].ttf', [('Medium', {'wght': 500}), ('Bold', {'wght': 700})]),
    ('SC', 'NotoSansSC[wght].ttf', [('Medium', {'wght': 500}), ('Bold', {'wght': 700})]),
    ('TC', 'NotoSansTC[wght].ttf', [('Medium', {'wght': 500}), ('Bold', {'wght': 700})]),
    ('Cyr', 'NotoSans[wdth,wght].ttf', [('Medium', {'wght': 500, 'wdth': 75}), ('SemiBold', {'wght': 600, 'wdth': 75})]),
    ('Cyr', 'NotoSans-Italic[wdth,wght].ttf', [('BoldItalic', {'wght': 700, 'wdth': 75})]),
]
total = 0
for script, source, weights in jobs:
    for name, axes in weights:
        font = TTFont(os.path.join(src, source))
        font = instancer.instantiateVariableFont(font, axes)
        opts = subset.Options()
        opts.layout_features = ['kern', 'liga', 'calt', 'ccmp', 'locl', 'vert', 'palt'] if script != 'Cyr' else ['kern', 'liga', 'ccmp', 'locl']
        opts.name_IDs = ['*']
        opts.notdef_outline = True
        opts.drop_tables += ['DSIG']
        sub = subset.Subsetter(opts)
        sub.populate(unicodes=sorted(sets[script]))
        sub.subset(font)
        out = os.path.join(out_dir, 'NotoSans%s-%s.ttf' % (script, name))
        font.save(out)
        size = os.path.getsize(out)
        total += size
        print('%-28s %5d letters %7d bytes' % (os.path.basename(out), len(sets[script]), size))
print('all: %d bytes' % total)
