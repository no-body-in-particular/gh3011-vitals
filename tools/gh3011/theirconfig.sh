#!/system/bin/sh
# Their configuration, our reader: split acquisition from arithmetic.
#
# The published saturation does not track a desaturation, and no curve fixes that. But their daemon
# gets a working one from the same sensor and the same FIFO, so the information is in the hardware
# and our extraction loses it. This finds out which half is wrong.
#
#   1. their daemon configures the part and runs a saturation, and reports its own answer
#   2. the daemon is killed - not stopped - so the part keeps streaming with their settings
#   3. fifograb drains the FIFO and writes the samples out, configuring nothing
#
# Then R is computed offline from their stream:
#
#   still near 1.05  ->  the fault is our arithmetic
#   near 0.5         ->  the fault is our configuration, and theirs is the fix
#
# Killing rather than stopping matters. `setprop ctl.stop` lets the daemon run its shutdown, which
# halts the chip - and then there is nothing left to read. SIGKILL leaves the part exactly as their
# code last set it.
#
# Everything is restored at the end whatever happens, including on the paths that exit early.
SECS=${1:-30}
HOLD=${2:-45}

restore() {
    setprop ctl.stop gh3011_daemon 2>/dev/null
    sleep 2
    if [ -f /data/local/tmp/shim.bak ]; then
        cat /data/local/tmp/shim.bak > /system/bin/gh3011_service
        chmod 755 /system/bin/gh3011_service
        rm -f /data/local/tmp/shim.bak
    fi
    setprop ctl.start gh3011_daemon 2>/dev/null
    echo "--- restored: $(ls -l /system/bin/gh3011_service)"
}

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

echo "--- their saturation, $HOLD seconds:"
/data/local/tmp/ghcmd 5 >/dev/null
sleep $HOLD
logcat -d | grep "spo2_result" > /data/local/tmp/theirs.txt
grep -c . /data/local/tmp/theirs.txt

# Kill, do not stop: a clean shutdown would halt the part and leave nothing to read.
PID=$(ps | grep gh3011_service.real | while read u p rest; do echo $p; done)
echo "--- killing $PID and reading their stream for ${SECS}s"
kill -9 $PID 2>/dev/null
/data/local/tmp/fifograb $SECS /data/local/tmp/theirstream.txt
echo "--- samples: $(grep -c . /data/local/tmp/theirstream.txt)"

restore
