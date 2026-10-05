#!/bin/sh
# Boot a test ISO headless and turn QEMU's exit status into pass/fail.
#   usage: tests/qemu/run.sh <iso> <serial-log>
ISO="$1"
LOG="$2"
SECS="${TEST_TIMEOUT:-30}"

TIMEOUT=$(command -v timeout || command -v gtimeout)
if [ -z "$TIMEOUT" ]; then
    echo "error: need 'timeout' (macOS: brew install coreutils)" >&2
    exit 2
fi

"$TIMEOUT" "$SECS" qemu-system-x86_64 \
    -M q35 -m 512M -cdrom "$ISO" \
    -no-reboot -display none \
    -serial file:"$LOG" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
code=$?

cat "$LOG"
case $code in
    33)  echo "==> PASS"; exit 0 ;;
    35)  echo "==> FAIL (a test failed or the kernel halted)" ;;
    124) echo "==> FAIL (timed out after ${SECS}s: hang?)" ;;
    0)   echo "==> FAIL (QEMU exited without a result: triple fault?)" ;;
    *)   echo "==> FAIL (QEMU exit status $code)" ;;
esac
exit 1
