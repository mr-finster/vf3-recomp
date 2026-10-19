#!/usr/bin/env python3
"""The download page's pictures from the trailer (game pictures: they go only in build/site, never in git):
  build/site/media/screen.mp4  the cabinet's screen: the trailer's fights, 640x360, 30 fps, no sound track
  build/site/shots/00.webp     its first picture (the video's poster while it loads)
  build/site/media/poster.jpg  and media/og.jpg (the link preview)
Needs ffmpeg and Pillow.

  tools/site/media.py TRAILER.mp4 [--from 6.2 --to 51 --still 6.2]
"""
import argparse, os, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'build', 'site')


def ff(*args):
    subprocess.run(['ffmpeg', '-v', 'error', '-y', *args], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trailer')
    ap.add_argument('--from', dest='t0', type=float, default=6.2)  # seconds into the trailer: the fights,
    ap.add_argument('--to', dest='t1', type=float, default=51)     # before the end card
    ap.add_argument('--still', type=float, default=6.2)   # (Akira and Wolf at sunset)
    a = ap.parse_args()
    os.makedirs(os.path.join(OUT, 'media'), exist_ok=True)
    os.makedirs(os.path.join(OUT, 'shots'), exist_ok=True)
    ff('-ss', str(a.t0), '-to', str(a.t1), '-i', a.trailer, '-an', '-vf', 'scale=640:360:flags=lanczos,fps=30',
       '-c:v', 'libx264', '-preset', 'slow', '-crf', '24', '-pix_fmt', 'yuv420p', '-movflags', '+faststart',
       os.path.join(OUT, 'media', 'screen.mp4'))
    # (the WebP by Pillow, as build.py needs it anyway: not every ffmpeg has libwebp)
    png = os.path.join(OUT, 'shots', '00.png')
    ff('-ss', str(a.t0), '-i', a.trailer, '-frames:v', '1', '-vf', 'scale=640:360:flags=lanczos', png)
    from PIL import Image
    Image.open(png).save(os.path.join(OUT, 'shots', '00.webp'), quality=86)
    os.remove(png)
    for name in ('poster.jpg', 'og.jpg'):
        ff('-ss', str(a.still), '-i', a.trailer, '-frames:v', '1', '-vf', 'scale=1280:720:flags=lanczos',
           '-q:v', '3', os.path.join(OUT, 'media', name))
    print('made', os.path.join(OUT, 'media'), 'and', os.path.join(OUT, 'shots', '00.webp'))


if __name__ == '__main__':
    main()
