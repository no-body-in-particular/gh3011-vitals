#!/usr/bin/env python3
"""Try different ratio formulations against one captured pass, offline.

Every tracking run in this project tested one formulation - band_amp at the heart rate, divided by
the light level, per channel. Five of them said the ratio does not follow a desaturation. That is a
statement about that formulation, and the others are worth a look before concluding it is the sensor:
log-ratio, peak-to-trough, per-beat medians, and the second harmonic are all different enough to
behave differently on the same samples.

Reads the paired dump ppgd writes under RAWDUMP - "# burst N" lines and "c1 c2" frames. The flat
one-sample-a-line format it used to write could not be de-interleaved: bursts are lvl*3 bytes and a
burst holding an odd number of samples flips the channel phase.

**This reproduces ppgd's own number before offering an opinion**, which took four attempts to get
right and is the only reason to believe anything else it prints:

    a single Goertzel over the whole pass   R 1.05    ppgd said 0.522
    the same, per beat                      R 1.10
    band_amp as ppgd computes it            R 0.404   ppgd said 0.409

The difference is that band_amp averages six-second windows at 50% overlap. A wearer's rate moves
with their breathing, so a fine frequency bin over a long window misses most of the pulse - a one bpm
shift moved the amplitude by a factor of two in testing. Coarse bins, averaged, do not.

    ratio-lab.py <dump> [bpm]
"""

import math
import statistics as st
import sys

FS = 100.0
PEDESTAL_UNIT = 1048576.0


def load(path):
    a, b = [], []
    for line in open(path):
        if line.startswith('#'):
            continue
        p = line.split()
        if len(p) == 2:
            a.append(int(p[0]))
            b.append(int(p[1]))
    return a, b


def dark_for(dc):
    """ppgd's pedestal: the largest whole multiple of 0x100000 at or below the reading."""
    units = math.floor(dc / PEDESTAL_UNIT)
    return max(units, 1.0) * PEDESTAL_UNIT


def bin_amp(x, dc, f):
    n = len(x)
    if n < 8 or f <= 0.0 or f >= FS / 2.0:
        return 0.0
    w = 2.0 * math.pi * f / FS
    c = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s0 = (v - dc) + c * s1 - s2
        s2, s1 = s1, s0
    p = s1 * s1 + s2 * s2 - c * s1 * s2
    return 2.0 * math.sqrt(p) / n if p > 0.0 else 0.0


def band_amp(x, f):
    """ppgd's: bin_amp over six-second windows at half-window step, averaged."""
    n = len(x)
    w = min(max(int(FS * 6.0), 16), n)
    step = max(1, w // 2)
    tot, nw = 0.0, 0
    i = 0
    while i + w <= n:
        seg = x[i:i + w]
        tot += bin_amp(seg, sum(seg) / w, f)
        nw += 1
        i += step
    return tot / nw if nw else 0.0


def per_beat(x, bpm):
    """Peak-to-trough within each beat window, returned as a list."""
    period = int(FS * 60.0 / bpm)
    out = []
    for i in range(0, len(x) - period, period):
        w = x[i:i + period]
        out.append((max(w), min(w)))
    return out


def formulations(a, b, bpm):
    f = bpm / 60.0
    da, db = sum(a) / len(a), sum(b) / len(b)
    la, lb = da - dark_for(da), db - dark_for(db)
    if la < 100 or lb < 100:
        return {}

    out = {}

    A, B = band_amp(a, f), band_amp(b, f)
    out['band_amp (reference)'] = (A / la) / (B / lb) if B else 0.0

    A2, B2 = band_amp(a, 2 * f), band_amp(b, 2 * f)
    out['second harmonic'] = (A2 / la) / (B2 / lb) if B2 else 0.0

    pa, pb = per_beat(a, bpm), per_beat(b, bpm)
    if len(pa) >= 4 and len(pb) >= 4:
        ta = st.median([h - l for h, l in pa])
        tb = st.median([h - l for h, l in pb])
        out['peak-to-trough'] = (ta / la) / (tb / lb) if tb else 0.0

        # log-ratio: ln(Imax/Imin) per channel. Less sensitive to path length and to the pedestal,
        # which is why it is the form most references use.
        ga = st.median([math.log(h / l) for h, l in pa if l > 0])
        gb = st.median([math.log(h / l) for h, l in pb if l > 0])
        out['log-ratio'] = ga / gb if gb else 0.0

        # the same, per beat, then the median of the ratios rather than a ratio of the medians
        rs = []
        for (ha, lo_a), (hb, lo_b) in zip(pa, pb):
            if lo_a > 0 and lo_b > 0 and (hb - lo_b) > 0:
                rs.append(((ha - lo_a) / la) / ((hb - lo_b) / lb))
        if len(rs) >= 4:
            out['per-beat median'] = st.median(rs)

    # no pedestal subtracted at all, in case the assumed 0x300000 is wrong
    out['raw DC (no pedestal)'] = (A / da) / (B / db) if B else 0.0
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    a, b = load(argv[1])
    bpm = float(argv[2]) if len(argv) > 2 else 59.0
    print('%s: %d frames, %.0f s, at %.0f bpm' % (argv[1], len(a), len(a) / FS, bpm))
    for name, val in formulations(a, b, bpm).items():
        print('   %-22s %.3f' % (name, val))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
