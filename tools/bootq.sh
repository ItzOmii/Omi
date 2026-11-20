#!/bin/bash
# Boot the ISO headless, collect the suite tallies.
#   tools/bootq.sh <tag> [seconds] [iso]
set -u
TAG=${1:?tag}
SECS=${2:-100}
ISO=${3:-/home/meep/omi-fork/omi.iso}
MEM=${MEM:-512M}
LOG=/tmp/boot-$TAG.log
SOCK=/tmp/boot-$TAG.sock
pkill -f '^qemu-system-x86_64' 2>/dev/null
sleep 1
rm -f "$LOG" "$SOCK"
qemu-system-x86_64 -M pc -m "$MEM" -cdrom "$ISO" -boot d -display none \
  -serial file:"$LOG" -monitor unix:"$SOCK",server,nowait \
  -netdev user,id=n0 -device e1000,netdev=n0 -enable-kvm >/dev/null 2>&1 &
QPID=$!
sleep "$SECS"
kill $QPID 2>/dev/null
sleep 1
pkill -f '^qemu-system-x86_64' 2>/dev/null
exit 0
