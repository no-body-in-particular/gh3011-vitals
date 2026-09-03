#!/usr/bin/env python3
"""Compute the ratio of ratios from a raw sample dump, independently of ppgd.

The point of this is to be a second opinion. ppgd measures amplitude peak-to-baseline, beat by beat,
and that method has twice now been dominated by something other than the pulse - once by DC steps
from a hunting gain loop, and once by a sub-cardiac component three to ten times larger than the
heartbeat. Both times the resulting ratio was steady, plausible and wrong.

So this does the narrow thing instead. It takes the amplitude at one frequency, per channel, over
the whole window, and forms

    R = (A1 / L1) / (A2 / L2)

with L the light level above the pedestal. Nothing outside the band contributes, so baseline wander
and gain steps are excluded by construction rather than by a threshold.

Reads the interleaved format that ppgd's RAWDUMP and fifograb both write: one unsigned sample per
line, channel 1 and channel 2 alternating.

    stream-ratio.py <file> [heart-rate-bpm] [--skip SECONDS] [--fs HZ]

With no rate given it reports the strongest component between 40 and 200 bpm and uses that, and
also prints the spectrum around it - which is how the sub-cardiac component was found. Give it the
rate ppgd reported when comparing the two, because they have to be measuring at the same frequency
for the comparison to mean anything.
"""

import math
import sys

PEDESTAL = 3145728.0      # three units of 0x100000; see the dark_for note in ppgd.c
DEFAULT_FS = 100.0


def goertzel(x, fs, f):
    """Amplitude of the component at f, over the whole of x."""
    n = len(x)
    if n == 0:
        return 0.0
    mean = sum(x) / n
    w = 2.0 * math.pi * f / fs
    re = im = 0.0
    for i, v in enumerate(x):
        d = v - mean
        re += d * math.cos(w * i)
        im += d * math.sin(w * i)
    return 2.0 * math.sqrt(re * re + im * im) / n


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    path = argv[1]
    bpm = None
    skip_s = 5.0
    fs = DEFAULT_FS

    rest = argv[2:]
    i = 0
    while i < len(rest):
        if rest[i] == "--skip":
            skip_s = float(rest[i + 1]); i += 2
        elif rest[i] == "--fs":
            fs = float(rest[i + 1]); i += 2
        else:
            bpm = float(rest[i]); i += 1

    with open(path) as fh:
        v = [int(line) for line in fh if line.strip()]

    a, b = v[0::2], v[1::2]
    n0 = int(skip_s * fs)
    a, b = a[n0:], b[n0:]
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    if n < 256:
        print("only %d paired samples after a %.0fs skip - not enough" % (n, skip_s))
        return 1

    print("%s: %d paired samples, %.1f s at %.0f Hz, first %.0f s skipped"
          % (path, n, n / fs, fs, skip_s))

    if bpm is None:
        best = max((goertzel(a, fs, x * 0.5 / 60.0), x * 0.5)
                   for x in range(80, 401))
        bpm = best[1]
        print("strongest component between 40 and 200 bpm: %.1f" % bpm)
        print("(if that is well under the rate ppgd reports, it is not the pulse - "
              "pass the rate explicitly)")

    out = {}
    for lab, s in (("ch1", a), ("ch2", b)):
        level = sum(s) / n - PEDESTAL
        amp = goertzel(s, fs, bpm / 60.0)
        harm = goertzel(s, fs, 2.0 * bpm / 60.0)
        out[lab] = (amp, level)
        print("  %s  level %8.0f  amp@%.0fbpm %8.2f  amp@2x %7.2f  perfusion %.5f"
              % (lab, level, bpm, amp, harm, amp / level if level > 0 else 0.0))

    (a1, l1), (a2, l2) = out["ch1"], out["ch2"]
    if l1 <= 0 or l2 <= 0 or a2 <= 0:
        print("  unusable: a level or amplitude is not positive")
        return 1

    r = (a1 / l1) / (a2 / l2)
    print("  R = %.3f" % r)

    # Their curve for this watch, from the host parameter block their daemon writes: ids 0x2030 to
    # 0x2035, three 32-bit values at a scale of ten thousand. Printed for both the ratio as measured
    # and half of it, because the divisor between their scale and ours is the open question and
    # neither value should be presented as the answer.
    def curve(x):
        return -1.0223 * x * x - 30.5835 * x + 113.4171

    print("  their curve at R      : %.1f%%" % curve(r))
    print("  their curve at R/2    : %.1f%%" % curve(r / 2.0))
    print("  for 98%% their R is 0.496, so ours would have to be %.3f at a divisor of 2"
          % (0.496 * 2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
