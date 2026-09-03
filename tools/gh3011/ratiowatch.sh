#!/bin/sh
# Log the ratio and everything that might explain it, every ten minutes.
#
# Six explanations for the ratio wandering were tested tonight and all six were falsified,
# including the reboot that was recorded as the fix. What is left is that every good measurement
# was taken while the watch was being worn attentively and every bad one came later with the
# wearer asleep - so the candidate is where the sensor sits, and that is not something another
# register experiment can settle.
#
# This measures instead. Each sample records the two amplitudes, the ratio, and the wrist
# temperature, which is the only contact proxy available: a sensor pressed against skin reads
# warmer than one sitting off it. If ac2 tracks temperature across a night, contact is the answer
# and the calibration has to be conditioned on it. If it does not, that is worth knowing too.
#
# Ten minutes because a measurement lights the LED for half of one, and a night of this should not
# be what flattens the battery.

OUT="$(dirname "$0")/ratiowatch.log"
DEV=10.120.195.42:5555
END=$(( $(date +%s) + 28800 ))          # eight hours

say() { echo "$(date '+%m-%d %H:%M') $*" >> "$OUT"; }

say "=== started ==="

while [ "$(date +%s)" -lt "$END" ]; do
    adb connect "$DEV" >/dev/null 2>&1
    state=$(adb devices 2>/dev/null | grep "$DEV" | awk '{print $2}')

    if [ "$state" != "device" ]; then
        say "unreachable (${state:-absent})"
        sleep 600
        continue
    fi

    # The daemon is stopped for the measurement and started again after, so the launcher's own
    # cycle is not fighting this one for the sensor.
    adb -s "$DEV" shell 'setprop ctl.stop gh3011_daemon' >/dev/null 2>&1
    sleep 2

    line=$(adb -s "$DEV" shell 'PREV_BPM=56 /data/local/tmp/ppgd 25 /data/local/tmp/wx.txt spo2 2>/dev/null' 2>/dev/null | sed 's/\r$//')

    adb -s "$DEV" shell 'setprop ctl.start gh3011_daemon' >/dev/null 2>&1
    sleep 8

    # The thermometer answers through the daemon, so read it after the daemon is back;
    # ppgd has no thermometer and the earlier version asked it for one.
    temp=$(adb -s "$DEV" shell '/data/local/tmp/vtest temp 2>/dev/null' 2>/dev/null | sed 's/$//' | grep -o 'temp=[0-9.]*' | head -1)

    ac=$(echo "$line" | grep -o 'ac1=[0-9]* ac2=[0-9]*')
    r=$(echo "$line" | grep -o ' r=[0-9.]*' | head -1)
    dc=$(echo "$line" | grep -o 'dc1=[0-9]* dc2=[0-9]*')
    why=$(echo "$line" | grep -o 'reason=[a-z_]*')

    say "${ac:-no-amp} ${r:-no-r} ${dc:-} ${temp:-no-temp} ${why:-ok}"
    sleep 600
done

say "=== finished ==="
