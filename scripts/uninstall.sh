#!/bin/bash
# Remove rp1-audio-tdm kernel module and device tree overlay
set -euo pipefail

OVERLAY_DIR="${OVERLAY_DIR:-/boot/overlays}"
MODULE_NAME="snd-soc-rp1-audio-tdm"

echo "Removing kernel module..."
sudo modprobe -r "$MODULE_NAME" 2>/dev/null || true

# Find and remove installed module
MODULE_PATH=$(find /lib/modules -name "${MODULE_NAME}.ko*" 2>/dev/null | head -1)
if [ -n "$MODULE_PATH" ]; then
	sudo rm -f "$MODULE_PATH"
	sudo depmod -a
	echo "Removed $MODULE_PATH"
else
	echo "Module not found in /lib/modules"
fi

echo "Removing device tree overlay..."
sudo rm -f "$OVERLAY_DIR/rp1-audio-tdm.dtbo"

echo ""
echo "Remember to remove the dtoverlay=rp1-audio-tdm line from config.txt"
echo "Done."
