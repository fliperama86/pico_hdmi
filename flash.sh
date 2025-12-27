#!/bin/bash
set -e

UF2_FILE="build/pico_hdmi.uf2"

if [ ! -f "$UF2_FILE" ]; then
    echo "Error: $UF2_FILE not found. Run ./build.sh first."
    exit 1
fi

echo "Rebooting Pico into BOOTSEL mode..."
picotool reboot -f -u || true

echo "Waiting for Pico to enter BOOTSEL mode..."
sleep 2

echo "Flashing $UF2_FILE..."
picotool load "$UF2_FILE" -f
picotool reboot

echo "Done."
