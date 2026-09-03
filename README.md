# gh3011-vitals

Heart rate, blood pressure and a saturation ratio from the Goodix GH3011 PPG sensor in a pt880
kids' watch, without the vendor's daemon.

The watch ships with `gh3011_service`, a closed daemon that drives the sensor and reports through
its own protocol. This replaces it: `vitalsd` sits in the same init slot, drives the chip directly
over `/dev/gh_tools`, and answers a socket. The launcher speaks to that socket and needs nothing
else from here.

## What works, and how well

| | state |
|---|---|
| heart rate | measured against a cuff at about +1 bpm, three of four measurements inside two |
| blood pressure | tracks poorly - the estimate sits near 114 whether the wearer is at 105 or 120 |
| saturation | a ratio, not a percentage. 0.70 with seven percent of spread when there is a pulse to measure |
| wear detection | the sensor's own detector, about a second, five runs each way with no overlap |

No saturation percentage is published, deliberately. The ratio is real; turning it into a number
out of ten requires a calibration this has no reference for, and a one-point anchor once printed
86% for a healthy wearer at rest - alarming, and with no measurement behind it. `docs/gh3011.md`
says what each number is worth.

## The interface

One line each way over an abstract unix socket named `watchvitals`, which is what
`android.net.LocalSocket` speaks by default, so the app side needs no filesystem permissions.

    ->  hr            green LED, heart rate only
    ->  spo2          red and infrared, adds the ratio and the pulse shape
    ->  wear          the thermometer alone: no LEDs, no measurement

    <-  hr=49 spread=2 hz=99.7 ... r=0.706 sbp=113 dbp=69 conf=0.82
    <-  hr=0 reason=...          when nothing trustworthy came out

A refusal is a normal answer and says why. `weak=1` on a ratio means there was too little pulse
behind it to divide by; `tracked=1` on a rate means the windows disagreed and the previous rate
chose between them.

Every measurement is appended to `/sdcard/vitals.log` with its full field set.

## Layout

    tools/gh3011/vitalsd.c     the daemon: socket, wear check, two-pass measurement
    tools/gh3011/ppgd.c        the measurement itself, and everything that reads a waveform
    tools/gh3011/adtwear.c     the chip's own wear detector
    tools/gh3011/ghcmd.c       sends the vendor daemon a command, for measuring ours against theirs
    tools/gh3011/seq.h         the start sequence, generated from a capture of the vendor daemon
    tools/gh3011/adt_table.h   the auto-detect configuration, read out of the vendor binary
    docs/gh3011.md             the chip: registers, commands, and how a reading is taken

## Building

An Android NDK arm32 toolchain, and nothing else:

    armv7a-linux-androideabi19-clang -Os -Wall -o ppgd ppgd.c -lm
    armv7a-linux-androideabi19-clang -Os -Wall -o vitalsd vitalsd.c
    armv7a-linux-androideabi19-clang -Os -Wall -o adtwear adtwear.c
    armv7a-linux-androideabi19-clang -Os -Wall -o ghcmd ghcmd.c

`tools/gh3011/install-vitalsd.sh` puts them on the watch and takes over the init slot. It keeps
the vendor daemon at `gh3011_service.real` so the swap is one `cat` in either direction.

## What is not here

The probes, register sweeps, vendor-binary harnesses and captures this was worked out with are
not in this repository. `docs/gh3011.md` is the result of them and is the part worth keeping; the
rest was scaffolding, and some of it recorded measurements of a person.
