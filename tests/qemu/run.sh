#!/bin/sh
# Boot a test ISO headless and turn QEMU's exit status into pass/fail.
#   usage: tests/qemu/run.sh <iso> <serial-log>
#
# The timeout is a plain shell watchdog: no coreutils `timeout`, so this
# runs the same on Linux and macOS.
ISO="$1"
LOG="$2"
SECS="${TEST_TIMEOUT:-30}"

qemu-system-x86_64 \
    -M q35 -cpu max -m 512M -cdrom "$ISO" \
    -no-reboot -display none \
    -serial file:"$LOG" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 &
qemu=$!

# SIGKILL: a wedged QEMU may ignore a polite signal.
(sleep "$SECS"; kill -9 "$qemu" 2>/dev/null) &
watchdog=$!

wait "$qemu"
code=$?
kill "$watchdog" 2>/dev/null
wait "$watchdog" 2>/dev/null

[ -f "$LOG" ] && cat "$LOG"
case $code in
    33)       echo "==> PASS"; exit 0 ;;
    35)       echo "==> FAIL (a test failed or the kernel halted)" ;;
    124|137)  echo "==> FAIL (timed out after ${SECS}s: hang?)" ;;
    0)        echo "==> FAIL (QEMU exited without a result: triple fault?)" ;;
    *)        echo "==> FAIL (QEMU exit status $code)" ;;
esac
exit 1
