#!/usr/bin/env python3
# langcheck: menutest in every language (and pseudo), each language's text against its boxes compared with the
# English's. A language fails on a box its text overflows where the English's doesn't (the English's own, listed,
# are the layout's as it was), or on a check menutest makes that the English passes (what the screens do can't
# depend on the language). Pictures: <out>/<code>/*.png.
#   SDL_VIDEO_DRIVER=offscreen python3 tools/langcheck.py <menutest> <out folder> [codes...]
# (with VF_LOBBY=<a lobby, e.g. tools/lobby's test/serve.mjs> the room screens too, one language at a time)
import concurrent.futures
import os
import re
import subprocess
import sys

CODES = ['en', 'ko', 'ja', 'zh-Hans', 'zh-Hant', 'es', 'pt-BR', 'fr', 'ru', 'de', 'it', 'pl', 'tr', 'pseudo']


def run(menutest, out, code):
    d = os.path.join(out, code)
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ, VF_LANG=code)
    p = subprocess.run([menutest, d], env=env, capture_output=True, timeout=1200)
    text = (p.stdout + p.stderr).decode('utf-8', 'replace')
    over = {}
    for line in text.splitlines():
        if line.startswith('overflow: '):
            m = re.match(r'overflow: (.*?): "(.*)" past (.*)$', line)
            if m:
                over.setdefault(m.group(1) + ' | ' + m.group(3), m.group(2))
    fails = sorted(set(re.findall(r'FAIL (.*)', text)) - {s for s in re.findall(r'FAIL (.*)', text) if s.startswith('past its box')})
    return over, fails


def main():
    menutest, out = sys.argv[1], sys.argv[2]
    codes = sys.argv[3:] or CODES
    if 'en' not in codes:
        codes = ['en'] + codes
    # (each its own menutest, side by side; one at a time with a lobby (VF_LOBBY): Quick match would pair two languages)
    workers = 1 if os.environ.get('VF_LOBBY') else (os.cpu_count() or 4)
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        results = dict(zip(codes, pool.map(lambda c: run(menutest, out, c), codes)))
    en_over, en_fails = results['en']
    bad = 0
    print('en: %d lines past their box (the layout as it was), %d checks failing' % (len(en_over), len(en_fails)))
    for c in codes:
        if c == 'en':
            continue
        over, fails = results[c]
        new = {k: v for k, v in over.items() if k not in en_over}
        new_fails = [f for f in fails if not any(f.split(' (')[0] == e.split(' (')[0] for e in en_fails)]
        status = 'ok' if not new and not new_fails else ('listed' if c == 'pseudo' else 'FAIL')
        print('%s: %s, %d boxes overflowed that the English fits, %d checks the English passes failed' % (c, status, len(new), len(new_fails)))
        for k, v in sorted(new.items()):
            print('   %s  "%s"' % (k, v))
        for f in new_fails:
            print('   check: %s' % f)
        if status == 'FAIL':
            bad += 1
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
