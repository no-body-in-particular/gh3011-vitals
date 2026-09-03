#!/system/bin/sh
# Hold the gain equal across passes and see whether ac1/ac2 still splits.
#
# R splits by the gain the loop settles on, 1.132 at 0x3939 against 1.040 at 0x3a3a, and the whole
# of that difference is in the amplitude ratio at identical light levels. A ratio of ratios divides
# gain out, so either something here does not, or the gain was only ever standing in for whatever
# actually moved. Alternating the two codes on one wrist separates those.
OUT=/data/local/tmp/gainsplit.log
: > $OUT
setprop ctl.stop gh3011_daemon
sleep 2
i=0
while [ $i -lt 10 ]; do
    i=$((i+1))
    if [ $((i % 2)) -eq 1 ]; then G=0x3939; else G=0x3a3a; fi
    echo "$(date '+%H:%M:%S') pass=$i fix=$G $(GAINFIX=$G /data/local/tmp/ppgd_new 30 /data/local/tmp/wx.txt spo2 2>/dev/null)" >> $OUT
done
setprop ctl.start gh3011_daemon
echo done
