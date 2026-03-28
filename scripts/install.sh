#!/bin/bash
# Install rp1-audio-tdm kernel module and device tree overlay
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
OVERLAY_DIR="${OVERLAY_DIR:-/boot/overlays}"
CONFIG_FILE="${CONFIG_FILE:-/boot/firmware/config.txt}"

echo "Building module and overlay..."
cd "$PROJECT_DIR"
make clean
make all

echo "Installing kernel module..."
sudo make install-module

echo "Installing device tree overlay..."
sudo make install-dtbo

# Check if overlay is already in config.txt
if ! grep -q "^dtoverlay=rp1-audio-tdm" "$CONFIG_FILE" 2>/dev/null; then
	echo ""
	echo "Add the following to $CONFIG_FILE under [all] or [pi5]:"
	echo ""
	echo "  # RP1 I2S/TDM audio driver"
	echo "  dtoverlay=rp1-audio-tdm"
	echo ""
	echo "Optional parameters:"
	echo "  dtoverlay=rp1-audio-tdm,tdm_slots=8,dai_fmt=dsp-a,clock_role=slave"
	echo "  dtoverlay=rp1-audio-tdm,mclk_fs=256,enable_i2s2=on"
	echo ""
	echo "Reboot required after configuration change."
else
	echo "Overlay already configured in $CONFIG_FILE"
fi

echo "Done."
