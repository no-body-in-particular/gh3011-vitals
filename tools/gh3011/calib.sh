#!/system/bin/sh
# Repeated saturation passes with a timestamp on each, for a calibration run.
#
# A curve is only tested by a saturation that moves. Every comparison against their daemon so far
# has been taken at rest, a single percent apart, which fixes an offset and says nothing about a
# slope. A breath hold moves it.
#
# Each pass is about twenty-two seconds and the gain settles inside that, so the time resolution is
# one point per half minute - coarse against a dip that lasts half a minute or so, which is why the
# run is long enough to catch several and why the wall clock is written beside every line.
SECS=${1:-22}
N=${2:-14}
OUT=/data/local/tmp/calib.log
: > $OUT
setprop ctl.stop gh3011_daemon
sleep 2
i=0
while [ $i -lt $N ]; do
    i=$((i+1))
    echo "$(date '+%H:%M:%S') pass=$i $(/data/local/tmp/ppgd $SECS /data/local/tmp/wx.txt spo2 2>/dev/null)" >> $OUT
done
setprop ctl.start gh3011_daemon
echo done
