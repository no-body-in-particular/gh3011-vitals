#!/system/bin/sh
# The test that matters: does the ratio track a desaturation now that the LED is driven?
#
# 0x0136 was 0x0000 in the shipped configuration and 0x0110 in Goodix reference array. With it set,
# channel 2 carries 2.2 times channel 1 instead of 1.0, and R reads 0.480 against their 0.496 for
# the 98 percent a meter agrees with. That is a ratio on the right scale with the right channel
# balance, which is necessary and not sufficient. Only a desaturation settles it.
#
# The override is passed on the command line rather than made the default: making it the default
# before tracking is shown would put a saturation on the chart on the strength of a channel balance.
OUT=/data/local/tmp/calib3.log
: > $OUT
setprop ctl.stop gh3011_daemon
sleep 3
i=0
while [ $i -lt 15 ]; do
    i=$((i+1))
    echo "$(date '+%H:%M:%S') pass=$i $(/data/local/tmp/ppgd 26 /data/local/tmp/wx.txt spo2 0136=0110 2>/dev/null)" >> $OUT
done
setprop ctl.start gh3011_daemon
echo done
