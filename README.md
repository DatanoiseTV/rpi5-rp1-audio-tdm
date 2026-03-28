# rp1-audio-tdm

Out-of-tree Linux kernel module and device tree overlay for multi-channel I2S/TDM audio on the Raspberry Pi 5 RP1 southbridge.

## Features

- Up to **12 channels full-duplex** by combining i2s0 (8ch) and i2s2 (4ch)
- Configurable **TDM** (DSP-A/DSP-B) or standard **I2S** mode
- Optional **MCLK output** via GPCLK0 on GPIO4, derived from pll_audio
- Master or slave clock mode
- Runtime-configurable via config.txt overlay parameters
- Loadable kernel module -- no kernel rebuild required

## Hardware

### RP1 I2S Peripherals

| Peripheral | GPIO Pins | Max Channels | Notes |
|---|---|---|---|
| i2s0 | GPIO18-27 | 8 (4 stereo pairs) | Primary, 10 pins |
| i2s2 | GPIO42-47 | 8 (4 FIFO pairs, 4 data pins) | Secondary, optional |

### Pin Mapping

**i2s0 (GPIO18-27):**
| Pin | Function |
|---|---|
| GPIO18 | BCLK |
| GPIO19 | LRCLK |
| GPIO20 | SDI0 / SDO0 |
| GPIO21 | SDI1 / SDO1 |
| GPIO22 | SDI2 / SDO2 |
| GPIO23 | SDI3 / SDO3 |
| GPIO24-27 | Additional data lines (full-duplex) |

**i2s2 (GPIO42-47):**
| Pin | Function |
|---|---|
| GPIO42 | BCLK |
| GPIO43 | LRCLK |
| GPIO44 | SDI0 / SDO0 |
| GPIO45 | SDI1 / SDO1 |
| GPIO46 | SDI2 / SDO2 |
| GPIO47 | SDI3 / SDO3 |

**MCLK (optional):**
| Pin | Function |
|---|---|
| GPIO4 | GPCLK0 (MCLK output) |

## Building

### On the Raspberry Pi 5

```bash
# Install kernel headers
sudo apt install raspberrypi-kernel-headers

# Build module and overlay
make

# Install (does not auto-load on boot)
sudo make install
```

### Cross-compilation

```bash
make KERNEL_DIR=/path/to/rpi-linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

## Configuration

Add to `/boot/firmware/config.txt`:

```ini
[pi5]
# Basic: i2s0 only, standard I2S, master mode
dtoverlay=rp1-audio-tdm

# TDM 8-slot, slave mode, with MCLK output
dtoverlay=rp1-audio-tdm,tdm_slots=8,dai_fmt=dsp-a,clock_role=slave,mclk_fs=256

# Enable secondary i2s2 controller
dtoverlay=rp1-audio-tdm,enable_i2s2=on
```

### Overlay Parameters

| Parameter | Values | Default | Description |
|---|---|---|---|
| `dai_fmt` | i2s, dsp-a, dsp-b, left-j, right-j | i2s | DAI wire format |
| `tdm_slots` | 2-16 | 2 | TDM slot count (2 = standard I2S) |
| `tdm_slot_width` | 32 | 32 | Bits per slot (DWC I2S only supports 32) |
| `clock_role` | master, slave | master | BCLK/LRCLK provider |
| `mclk_fs` | 0, 128, 256, 384, 512 | 0 | MCLK/fs ratio (0 = disabled) |
| `enable_i2s2` | on/off | on | Enable secondary controller |
| `enable_mclk` | on/off | on | Enable MCLK output on GPIO4 |

## TDM Master Mode

TDM with more than 2 slots in master mode requires a kernel patch to fix the DWC I2S CCR register limitation. Apply the patch from `patches/`:

```bash
cd /path/to/rpi-linux
git apply /path/to/patches/0001-dwc-i2s-fix-tdm-master-mode-clock-config.patch
```

TDM in **slave mode** works without any kernel patches (the external codec provides BCLK).

## MCLK

Many audio codecs require an MCLK (master clock) input, typically 256x or 512x the sample rate. The RP1 can provide this via GPCLK0 on GPIO4, sourced from pll_audio through clk_i2s.

Codecs that generate their internal clocks from BCLK (e.g., PCM5142, ES9038, some CS43xx) do not require MCLK. Set `mclk_fs=0` or omit it to disable MCLK output.

## Architecture

```
+------------------+      +------------------+
|  ALSA userspace  |      |  ALSA userspace  |
+--------+---------+      +--------+---------+
         |                         |
+--------+---------+      +--------+---------+
| rp1-audio-tdm    |      | rp1-audio-tdm    |
| (machine driver) |      | (machine driver) |
+--------+---------+      +--------+---------+
         |                         |
+--------+---------+      +--------+---------+
| dwc-i2s (i2s0)   |      | dwc-i2s (i2s2)   |
| (CPU DAI driver)  |      | (CPU DAI driver)  |
+--------+---------+      +--------+---------+
         |                         |
   GPIO18-27               GPIO42-47
   (BCLK,LRCLK,           (BCLK,LRCLK,
    SD0-SD7)                SD0-SD3)
```

## License

GPL-2.0
