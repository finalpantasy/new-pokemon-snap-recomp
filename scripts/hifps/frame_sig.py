"""Find one- or two-present flashes in a SNAP_FRAME_SIG file (runtime/nvn/nvn_vk.cpp; docs/high-fps-native.md; project code, ours).

A record is (u32 present, f64 ms, 32 x 18 luma bytes) of the game's window texture. A flash is a run of 1 or 2 presents that differs from
the present before it AND the present after it by more than --thr (mean absolute luma difference over the grid), while the presents before
and after agree with each other within --thr / 3: something was on screen for one or two frames and then gone again.

python -I scripts/hifps/frame_sig.py <file> [--thr 6] [--log <boot_stderr.log>]   (--log names the route waits around each flash)
"""
import argparse
import re
import struct

ap = argparse.ArgumentParser()
ap.add_argument('file')
ap.add_argument('--thr', type=float, default=6.0)
ap.add_argument('--log')
a = ap.parse_args()

N = 32 * 18
recs = []
with open(a.file, 'rb') as f:
    while True:
        h = f.read(12)
        if len(h) < 12:
            break
        p, ms = struct.unpack('<Id', h)
        px = f.read(N)
        if len(px) < N:
            break
        recs.append((p, ms, px))
recs.sort(key=lambda r: r[0])


def diff(x, y):
    return sum(abs(i - j) for i, j in zip(x, y)) / N


waits = []
if a.log:
    for line in open(a.log, encoding='utf-8', errors='replace'):
        m = re.search(r"wait '([^']+)' met after [\d.]+ s at present (\d+)", line)
        if m:
            waits.append((int(m.group(2)), m.group(1)))


def where(p):
    last = 'start'
    for wp, c in waits:
        if wp <= p:
            last = c
    return last


flashes = []
for i in range(1, len(recs) - 3):
    for w in (1, 2):
        if i + w >= len(recs):
            continue
        prev, nxt = recs[i - 1][2], recs[i + w][2]
        if recs[i + w][0] - recs[i - 1][0] != w + 1:
            continue   # a gap in the records: no claim
        if diff(prev, nxt) > a.thr / 3:
            continue
        ins = [recs[i + k][2] for k in range(w)]
        if all(diff(x, prev) > a.thr and diff(x, nxt) > a.thr for x in ins):
            flashes.append((recs[i][0], w, min(diff(x, prev) for x in ins), recs[i][1]))
print('%d records, presents %d..%d; %d flashes (threshold %.1f)' % (len(recs), recs[0][0] if recs else 0, recs[-1][0] if recs else 0, len(flashes), a.thr))
for p, w, d, ms in flashes:
    print('  present %d (+%d): differs by %.1f from both neighbours; after %s' % (p, w, d, where(p)))
