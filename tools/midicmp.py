#!/usr/bin/env python3
# midicmp.py DIR [WINDOW]: what each player's sound board was sent in a vf3net run (VF3NET_MIDI=DIR) against the
# straight run's (the same inputs, no rollback). Each sound command (A0, request, value) of the straight run is
# matched with one the side sent: the same command, made within WINDOW frames (default 8) of it, the nearest first.
# Effects are requests 0x11-0x14 (the game's effects, voices and announcer, docs/step4-game.md); every other request
# is the music's or the sound program's state (music, its volume, fades, stops). Said for each side: the commands
# matched (sent on their frame, or how late: the live frame they went at against the straight frame that made them),
# early (a guess's, made before the straight run made it), those dropped (never sent) and those extra (sent but the straight run never made them: a wrong guess's).
import struct, sys
from collections import defaultdict

def read(path):
    d = open(path, 'rb').read()
    i, out = 0, []
    while i < len(d):
        live, made, n = struct.unpack_from('<III', d, i)
        i += 12
        b = d[i:i + n]
        i += n
        k = 0
        while k + 2 < len(b) + 0:
            if b[k] == 0xA0:
                out.append((live, made, (b[k + 1], b[k + 2])))
                k += 3
            else:
                out.append((live, made, ('?', b[k])))
                k += 1
        while k < len(b):
            out.append((live, made, ('?', b[k])))
            k += 1
    return out

def kind(c):
    return 'effect' if c[0] != '?' and 0x11 <= c[0] <= 0x14 else 'music'

def compare(straight, sent, window):
    by = defaultdict(list)   # (command) -> indices into sent, unmatched
    for i, (live, made, c) in enumerate(sent):
        by[c].append(i)
    used = set()
    res = {k: dict(n=0, on_time=0, late=defaultdict(int), dropped=0, extra=0, order=0) for k in ('effect', 'music')}
    pairs = []
    for f, _, c in straight:
        k = kind(c)
        res[k]['n'] += 1
        best = None
        for i in by[c]:
            if i in used: continue
            live, made, _ = sent[i]
            if abs(made - f) > window: continue
            key = (abs(made - f), i)
            if best is None or key < best[0]: best = (key, i)
        if best is None:
            res[k]['dropped'] += 1
            continue
        i = best[1]
        used.add(i)
        live = sent[i][0]
        lag = live - f
        if lag == 0: res[k]['on_time'] += 1
        else: res[k]['late'][lag] += 1   # (negative: early, a guess's made before the straight run's)
        pairs.append((f, i))
    for i, (live, made, c) in enumerate(sent):
        if i not in used: res[kind(c)]['extra'] += 1
    # the music's commands in the order sent against the straight run's order (a pair out of order: one went before
    # another the straight run made first)
    m = sorted((i, f) for f, i in pairs if kind(sent[i][2]) == 'music')
    res['music']['order'] = sum(1 for a, b in zip(m, m[1:]) if b[1] < a[1])
    return res

def main():
    d = sys.argv[1]
    window = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    straight = read(d + '/straight.mid')
    last = max((f for f, _, _ in straight), default=0)
    for side in ('host', 'guest'):
        sent = [x for x in read(d + '/' + side + '.mid') if x[1] <= last]
        r = compare(straight, sent, window)
        for k in ('effect', 'music'):
            x = r[k]
            late = sum(v for a, v in x['late'].items() if a > 0)
            early = sum(v for a, v in x['late'].items() if a < 0)
            lat = ', '.join('%d: %d' % (a, b) for a, b in sorted(x['late'].items()))
            print('%-5s %-6s straight %4d: on time %4d, late %4d, early %3d (frames: %s), dropped %3d, extra %3d%s' % (
                side, k, x['n'], x['on_time'], late, early, lat or '-', x['dropped'], x['extra'],
                ', out of order %d' % x['order'] if k == 'music' else ''))

main()
