#!/usr/bin/env bash
# Flash build/picocarpod.uf2 without sudo.
# 1. If the Pico runs firmware with USB stdio, a 1200-baud touch reboots it into BOOTSEL
#    (otherwise hold BOOTSEL while plugging it in).
# 2. Try picotool; if USB permissions block it, fall back to mounting the RPI-RP2 drive
#    with udisksctl and copying the UF2.
set -euo pipefail
cd "$(dirname "$0")"
PICOTOOL="${HOME}/.pico-sdk/picotool/2.2.0/picotool/picotool"; [ -x "$PICOTOOL" ] || PICOTOOL=picotool
UF2=build/picocarpod.uf2
PORT="${PORT:-/dev/ttyACM0}"

in_bootsel() { lsblk -rno LABEL 2>/dev/null | grep -q '^RPI-RP2$' || "$PICOTOOL" info >/dev/null 2>&1; }

if ! in_bootsel && [ -e "$PORT" ]; then
    echo "Rebooting $PORT into BOOTSEL (1200 baud touch)..."
    stty -F "$PORT" 1200 || true
fi
for i in $(seq 1 50); do in_bootsel && break; sleep 0.2; done
in_bootsel || { echo "Pico not in BOOTSEL mode. Hold BOOTSEL while plugging in USB, then re-run."; exit 1; }

if "$PICOTOOL" load -v -x "$UF2" 2>/dev/null; then
    echo "Flashed with picotool."
else
    DEV=$(lsblk -rno NAME,LABEL | awk '$2=="RPI-RP2"{print $1}' | head -1)
    [ -n "$DEV" ] || { echo "picotool failed and no RPI-RP2 drive found"; exit 1; }
    MP=$(lsblk -rno MOUNTPOINT "/dev/$DEV" | head -1)
    if [ -z "$MP" ]; then
        udisksctl mount -b "/dev/$DEV" >/dev/null
        MP=$(lsblk -rno MOUNTPOINT "/dev/$DEV" | head -1)
    fi
    cp "$UF2" "$MP/" && sync
    echo "Flashed by copying UF2 to $MP (device reboots by itself)."
fi
for i in $(seq 1 50); do [ -e "$PORT" ] && break; sleep 0.2; done
echo "Serial console: $PORT @115200, e.g.  stty -F $PORT 115200 raw -echo; cat $PORT   (type single-key commands with: printf 'h' > $PORT)"
