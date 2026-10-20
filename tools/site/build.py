#!/usr/bin/env python3
"""Builds the download page into build/site, ready for Cloudflare Pages. Deploy from the repo root (the
production branch is claude/adoring-feynman-rsrxic, the name the first page went up under; any other branch is only a preview):
  npx wrangler pages deploy build/site --project-name vf3-recomp --branch claude/adoring-feynman-rsrxic

The page (tools/site/index.html) holds nothing from the game. The screenshots, the video and its
poster are made from it, so they live only in build/site (never in git):
  build/site/shots/00.webp (the screen's first picture; the trailer itself is on YouTube, in the Watch popup),
  media/screen.mp4 (the same at 640x360, 30 fps, no sound track: the cabinet's screen),
  media/poster.jpg, media/og.jpg
The downloads are attached to the GitHub release, from build/release, build/win and build/deck, and each
must be the build of this version (its program carries the version string exactly): a test build left
there stops it.

The referrer count: the page pings /_refhit on its own origin (the one-line script at the end of the
page; no comment there, the page has none), and a Pages Function there passes the reading on to the
collector, server-side, so nothing in the page names another host. The Function is written to
functions/_refhit.js at the repo root, outside build/site: wrangler takes functions/ from the folder
it runs in, and outside the deployed folder its source is never served as a file. It does nothing
until the project has the REF_KEY secret (wrangler pages secret put REF_KEY --project-name vf3-recomp).
The collector's address is not in the source: it is read from .refhit-url at the repo root (untracked),
or VF3_REF_URL; without it, no Function is written.

  tools/site/media.py TRAILER.mp4   (once: the cabinet's screen, the poster and the shot, from the trailer)
  tools/site/build.py --youtube ID [--url https://vf3-recomp.pages.dev] [--version (default: the latest tag)]
"""
import argparse, re, datetime, json, os, shutil, subprocess, sys

REFHIT = '''// The page pings /_refhit on its OWN origin; this passes the reading to the collector
// server-side. Nothing in the markup points at another host, so the sites stay unlinkable.
// Only the canonical hostname reports — preview deployments are ignored, so a *.pages.dev
// preview URL can never pollute the real counts.
const SITE = "vf3-recomp.pages.dev";
export async function onRequest(ctx) {
  const url = new URL(ctx.request.url);
  const res = new Response(null, { status: 204, headers: { "cache-control": "no-store" } });
  if (url.hostname !== SITE || !ctx.env.REF_KEY) return res;
  const q = new URLSearchParams({ s: SITE, r: url.searchParams.get("r") || "", k: ctx.env.REF_KEY });
  ctx.waitUntil(fetch(%s + q).catch(() => {}));
  return res;
}
'''
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'build', 'site')

# the screen's first picture (the cabinet's video poster), in build/site/shots
SHOTS = [('00.webp', 'A fight in widescreen')]


def size(path):
    mb = os.path.getsize(path) / 1e6
    return f'{mb:.1f} MB'


def carries(package, version):
    """Whether a package's program has exactly this version (VF3_VERSION is a C string in it)."""
    import tarfile, zipfile
    want = version.encode() + b'\0'
    if package.endswith('.zip'):
        with zipfile.ZipFile(package) as z:
            return any(want in z.read(i) for i in z.infolist() if not i.is_dir() and i.file_size < 64 << 20)
    with tarfile.open(package) as t:
        return any(want in t.extractfile(m).read() for m in t.getmembers() if m.isfile() and m.size < 64 << 20)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='https://vf3-recomp.pages.dev')
    ap.add_argument('--version', default=None)
    ap.add_argument('--youtube', required=True, help="the trailer's YouTube video ID (the Watch popup)")
    args = ap.parse_args()
    if not args.version:   # the latest release tag, never a made-up number
        args.version = subprocess.run(['git', 'describe', '--tags', '--abbrev=0'], cwd=ROOT, capture_output=True,
                                      text=True, check=True).stdout.strip()

    for f, _ in SHOTS:
        if not os.path.exists(os.path.join(OUT, 'shots', f)):
            raise SystemExit(f'missing build/site/shots/{f}')
    for f in ('screen.mp4', 'poster.jpg', 'og.jpg'):
        if not os.path.exists(os.path.join(OUT, 'media', f)):
            raise SystemExit(f'missing build/site/media/{f}')

    # downloads
    dl = os.path.join(OUT, 'downloads')
    os.makedirs(dl, exist_ok=True)
    mac_src = os.path.join(ROOT, 'build', 'release', 'VirtuaFighter3Recomp-mac.zip')
    deck_src = os.path.join(ROOT, 'build', 'deck', 'VirtuaFighter3Recomp-deck.tar.gz')
    win_src = os.path.join(ROOT, 'build', 'win', 'VirtuaFighter3Recomp-windows.zip')
    for src in (mac_src, deck_src, win_src):
        if not carries(src, args.version):
            raise SystemExit(f'{os.path.relpath(src, ROOT)} is not the {args.version} build (rebuild it from the tag)')
    mac, deck, win = 'VirtuaFighter3Recomp-mac.zip', 'VirtuaFighter3Recomp-deck.tar.gz', 'VirtuaFighter3Recomp-windows.zip'
    # The files themselves are on GitHub, attached to the release (mr-finster/vf3-recomp): the buttons link to the
    # latest release's, and the old /downloads/ links on this site send there too. The checks above still make sure
    # the packages built here, the ones to attach, are this version's.
    shutil.rmtree(dl)
    gh = 'https://github.com/mr-finster/vf3-recomp/releases/latest/download/'
    open(os.path.join(OUT, '_redirects'), 'w').write(''.join(f'/downloads/{f} {gh}{f} 302\n' for f in (mac, deck, win)))

    # the marquee (the app icon's letters, drawn wide) and the page icons
    img = os.path.join(OUT, 'img')
    os.makedirs(img, exist_ok=True)
    # the README's montage (game pictures: kept out of git, in build/readme, and served from the site)
    m = os.path.join(ROOT, 'build', 'readme', 'vf3_montage.jpg')
    if os.path.exists(m): shutil.copyfile(m, os.path.join(img, 'montage.jpg'))
    else: print('no build/readme/vf3_montage.jpg: no README montage on the site')
    icon = Image.open(os.path.join(ROOT, 'assets', 'icon', 'icon.png')).convert('RGBA')
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'site', 'marquee.py'), os.path.join(img, 'marquee.png')], check=True)
    icon.resize((64, 64), Image.LANCZOS).save(os.path.join(img, 'icon-64.png'), optimize=True)
    icon.resize((180, 180), Image.LANCZOS).save(os.path.join(img, 'icon-180.png'), optimize=True)

    page = open(os.path.join(ROOT, 'tools', 'site', 'index.html')).read()
    today = datetime.date.today()
    subs = {
        '%SITE_URL%': args.url.rstrip('/'),
        '%YOUTUBE_ID%': args.youtube,
        '%VERSION%': args.version,
        '%DATE%': f'{today:%b} {today.day} {today.year}',
        '%MAC_URL%': gh + mac, '%DECK_URL%': gh + deck, '%WIN_URL%': gh + win,
        '%MAC_FILE%': mac, '%MAC_SIZE%': size(mac_src),
        '%DECK_FILE%': deck, '%DECK_SIZE%': size(deck_src),
        '%WIN_FILE%': win, '%WIN_SIZE%': size(win_src),
        '%SHOTS%': json.dumps([list(s) for s in SHOTS]),
    }
    for k, v in subs.items():
        page = page.replace(k, v)
    shutil.rmtree(os.path.join(OUT, 'functions'), ignore_errors=True)   # (an old build's, inside the site)
    os.makedirs(os.path.join(ROOT, 'functions'), exist_ok=True)
    ref_file = os.path.join(ROOT, '.refhit-url')
    ref = os.environ.get('VF3_REF_URL') or (open(ref_file).read().strip() if os.path.exists(ref_file) else '')
    if ref: open(os.path.join(ROOT, 'functions', '_refhit.js'), 'w').write(REFHIT % json.dumps(ref + '?'))
    else: print('no .refhit-url (or VF3_REF_URL): no referrer Function written')
    open(os.path.join(OUT, 'index.html'), 'w').write(page)
    # (every comment stripped from the page as served: tools/site/strip, its inline script's and styles' too)
    strip = os.path.join(ROOT, 'tools', 'site', 'strip')
    if not os.path.isdir(os.path.join(strip, 'node_modules')): subprocess.run(['npm', 'install', '--no-audit', '--no-fund'], cwd=strip, check=True)
    subprocess.run(['node', os.path.join(strip, 'strip.mjs'), os.path.join(OUT, 'index.html'), os.path.join(OUT, 'index.html')], check=True)
    shutil.copyfile(os.path.join(ROOT, 'tools', 'site', '_headers'), os.path.join(OUT, '_headers'))
    # (a 404 page: without one Pages serves index.html for any path, and a crawler following the page's relative
    # links ever deeper, /img/shots/img/..., counted 173k page views in two hours on 19 Oct)
    open(os.path.join(OUT, '404.html'), 'w').write('<!doctype html><meta charset="utf-8"><title>Not found</title>'
        '<p>Not found. <a href="/">Virtua Fighter 3 Recomp</a></p>\n')
    print('built', OUT)


if __name__ == '__main__':
    main()
