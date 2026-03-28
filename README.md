# rp1-audio-tdm

Out-of-tree Linux kernel module and device tree overlay for multi-channel I2S/TDM audio on the Raspberry Pi 5.

Targets low-latency, high-channel-count audio applications: synthesizers, audio interfaces, DSP/FPGA interconnects, multi-codec arrays.

## What it does

- Exposes the RP1 I2S peripherals as a configurable ALSA sound card
- Up to **8 channels** playback + capture on i2s0 (GPIO18-27)
- Optional secondary bus on i2s2 (GPIO28-33, not on 40-pin header)
- **Standard I2S** or **TDM** (DSP-A/DSP-B) wire format
- **MCLK output** via GPCLK0 on GPIO4 for codecs that need it
- Master or slave clock mode
- Low-latency capable: **32-frame periods** (0.67ms at 48kHz) tested stable

## 40-Pin Header Pinout

```
                    +-----+
       3V3  [ 1] --| RPi |-- [ 2]  5V
  I2C1 SDA  [ 3] --|     |-- [ 4]  5V
  I2C1 SCL  [ 5] --|     |-- [ 6]  GND
 *MCLK/GP4  [ 7] --|     |-- [ 8]  UART TX
       GND  [ 9] --|     |-- [10]  UART RX
    GPIO 17 [11] --|     |-- [12] *I2S BCLK   (GPIO18)
   *I2S SD7 [13] --|     |-- [14]  GND
   *I2S SD2 [15] --|     |-- [16] *I2S SD3    (GPIO23)
       3V3  [17] --|     |-- [18] *I2S SD4    (GPIO24)
  SPI0 MOSI [19] --|     |-- [20]  GND
  SPI0 MISO [21] --|     |-- [22] *I2S SD5    (GPIO25)
  SPI0 SCLK [23] --|     |-- [24]  SPI0 CE0
       GND  [25] --|     |-- [26]  SPI0 CE1
 EEPROM SDA [27] --|     |-- [28]  EEPROM SCL
     GPIO 5 [29] --|     |-- [30]  GND
     GPIO 6 [31] --|     |-- [32]  PWM0
    PWM1    [33] --|     |-- [34]  GND
   *I2S WS  [35] --|     |-- [36]  GPIO 16
   *I2S SD6 [37] --|     |-- [38] *I2S SDI0   (GPIO20)
       GND  [39] --|     |-- [40] *I2S SDO0   (GPIO21)
                    +-----+

* = used by this driver
```

### I2S Signal Mapping (i2s0)

| GPIO | Header Pin | Function | Direction |
|------|-----------|----------|-----------|
| 4    | 7         | MCLK     | Output (optional) |
| 18   | 12        | BCLK     | Bidir (master/slave) |
| 19   | 35        | LRCLK/WS | Bidir (master/slave) |
| 20   | 38        | SD0 (data) | Input or output |
| 21   | 40        | SD1 (data) | Input or output |
| 22   | 15        | SD2 (data) | Input or output |
| 23   | 16        | SD3 (data) | Input or output |
| 24   | 18        | SD4 (data) | Input or output |
| 25   | 22        | SD5 (data) | Input or output |
| 26   | 37        | SD6 (data) | Input or output |
| 27   | 13        | SD7 (data) | Input or output |

The DesignWare I2S IP uses 4 FIFO pairs: each pair maps to one data line carrying stereo. With 4 data lines active you get 8 channels. In TDM mode, channels are time-multiplexed on fewer data lines instead.

### i2s2 (Secondary, not on header)

GPIO28-33 carry i2s2 but are **not** routed to the 40-pin header. They are accessible on custom carrier boards or via test pads. The RP1 synthesized i2s2 as slave-only (COMP1 MODE_EN=0), requiring an external codec to provide BCLK.

## Building

```bash
# On the Raspberry Pi 5
sudo apt install raspberrypi-kernel-headers
make all
```

Cross-compilation:
```bash
make KERNEL_DIR=/path/to/rpi-linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

## Quick Start

```bash
# Build
make all

# Copy overlay
sudo cp dts/rp1-audio-tdm.dtbo /boot/overlays/

# Load at runtime (safe, no reboot loop)
sudo dtoverlay rp1-audio-tdm
sudo insmod snd-soc-rp1-audio-tdm.ko

# Verify
aplay -l | grep rp1
# card N: rp1audiotdm [rp1-audio-tdm], device 0: ...

# Test 8-channel playback at 48kHz
aplay -D hw:rp1audiotdm,0 -c 8 -f S32_LE -r 48000 /dev/zero

# Test low-latency (32 frames = 0.67ms period)
aplay -D hw:rp1audiotdm,0 -c 2 -f S32_LE -r 48000 \
      --period-size=32 --buffer-size=128 /dev/zero
```

## Boot-Time Configuration

Add to `/boot/firmware/config.txt` under `[pi5]`:

```ini
dtoverlay=rp1-audio-tdm
```

With parameters:
```ini
# TDM 8-slot, slave mode, with MCLK
dtoverlay=rp1-audio-tdm,tdm_slots=8,dai_fmt=dsp-a,clock_role=slave,mclk_fs=256

# I2S master, no MCLK, i2s2 disabled
dtoverlay=rp1-audio-tdm,enable_i2s2=off,enable_mclk=off
```

### Overlay Parameters

| Parameter | Values | Default | Description |
|---|---|---|---|
| `dai_fmt` | i2s, dsp-a, dsp-b, left-j, right-j | i2s | Wire format |
| `tdm_slots` | 2-16 | 2 | Slot count (2 = standard I2S) |
| `clock_role` | master, slave | master | Who drives BCLK/LRCLK |
| `mclk_fs` | 0, 128, 256, 384, 512 | 256 | MCLK/fs ratio (0 = off) |
| `enable_i2s2` | on, off | on | Secondary I2S bus |
| `enable_mclk` | on, off | on | MCLK output on GPIO4 |

## Latency

The driver is tuned for low-latency operation:

| Period Size | Latency (48kHz) | Status |
|---|---|---|
| 16 frames | 0.33ms | Functional, may underrun at startup |
| 32 frames | 0.67ms | Stable, recommended minimum |
| 64 frames | 1.33ms | Comfortable |
| 128 frames | 2.67ms | Conservative |

For real-time audio, use a PREEMPT_RT kernel and set the process to SCHED_FIFO priority. The `dma-maxburst` is set to 2 in the overlay to minimize DMA transfer granularity.

## MCLK

GPCLK0 on GPIO4 (header pin 7) outputs MCLK derived from `pll_audio` through `clk_i2s`. The rate is `mclk_fs * sample_rate` and tracks automatically when the sample rate changes.

| Codec needs MCLK? | Action |
|---|---|
| No (PCM5142, ES9038, CS4344) | Set `mclk_fs=0` or `enable_mclk=off` |
| Yes (WM8960, TLV320AIC3x, CS42xx) | Set `mclk_fs=256` (or 384/512 per datasheet) |

## TDM Master Mode

TDM with >2 slots in master mode requires a kernel patch because the upstream DWC I2S driver's CCR register only accepts frame lengths of 32/48/64 bits:

```bash
cd /path/to/rpi-linux-source
git apply /path/to/patches/0001-dwc-i2s-fix-tdm-master-mode-clock-config.patch
```

TDM in **slave mode** works without patches.

## Architecture

```
+------------------+
|  ALSA userspace  |
|  (JACK/PipeWire) |
+--------+---------+
         |
+--------+---------+
| rp1-audio-tdm    |  <-- this module (machine driver)
| sets format, TDM |
| slots, MCLK rate |
+--------+---------+
         |
+--------+---------+
| dwc-i2s          |  <-- upstream kernel driver (CPU DAI)
| DMA, FIFO, clk   |
+--------+---------+
         |
   GPIO 18-27
   (BCLK, LRCLK,
    SD0-SD7)
```

## Known Limitations

- **i2s2 not on header**: GPIO28-33 are not routed to the RPi5 40-pin connector
- **i2s2 slave-only**: RP1 hardware synthesized i2s2 without master mode support
- **i2s0 and i2s1 share pins**: Cannot use both simultaneously (GPIO18-21)
- **Single I2S clock**: All RP1 I2S peripherals share `RP1_CLK_I2S`; they must run at the same sample rate
- **TDM master >2 slots**: Needs kernel patch (see above)
- **32-bit slot width only**: DWC I2S hardware constraint in TDM mode
- **OF overlay warnings**: Runtime overlay loading prints harmless memory leak warnings from the kernel's DT overlay infrastructure. These do not indicate a bug; they disappear when loading via config.txt at boot time

## License

GPL-2.0
