#!/system/bin/sh
# Does THEIR saturation track a desaturation? Nobody has checked.
#
# Every vendor reading on record - 97, 98, 99 - was taken at rest, and a plausible constant looks
# exactly like that. Ours is a plausible constant: it sat between 96 and 98 while a meter went from
# 99 to 91. If theirs does the same then this sensor configuration cannot measure saturation and
# there is nothing to chase; if theirs falls with the meter then the information is there and we are
# not extracting it.
#
# Their daemon reports about once a second once it has converged, which is far finer than our
# thirty-second passes, so the timing of the wearer's holds barely matters here.
SECS=${1:-240}
mount -o rw,remount /system 2>/dev/null
cp /system/bin/gh3011_service /data/local/tmp/shim.bak || exit 1

setprop ctl.stop gh3011_daemon
sleep 2
cat > /system/bin/gh3011_service <<'W'
#!/system/bin/sh
exec /system/bin/gh3011_service.real "$@"
W
chmod 755 /system/bin/gh3011_service

logcat -c 2>/dev/null
setprop ctl.start gh3011_daemon
sleep 4
logcat -v time > /data/local/tmp/theirhold.txt 2>&1 &
LPID=$!
/data/local/tmp/ghcmd 5 >/dev/null
sleep $SECS
/data/local/tmp/ghcmd 6 >/dev/null
sleep 2
kill $LPID 2>/dev/null

setprop ctl.stop gh3011_daemon
sleep 2
cat /data/local/tmp/shim.bak > /system/bin/gh3011_service
chmod 755 /system/bin/gh3011_service
rm -f /data/local/tmp/shim.bak
setprop ctl.start gh3011_daemon
echo "--- restored: $(ls -l /system/bin/gh3011_service)"
