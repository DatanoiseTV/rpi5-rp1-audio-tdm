/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rp1_pio_i2s.h - PIO-based I2S data lane expansion for RP1
 *
 * Uses RP1 PIO state machines to bit-bang additional I2S data lines,
 * synchronized to the hardware I2S BCLK/LRCLK. Each state machine
 * handles one data pin (2 channels: L + R).
 *
 * With 4 PIO state machines on GPIO24-27, this adds 8 channels beyond
 * the 8 provided by the DWC I2S hardware on GPIO20-23.
 *
 * Copyright (c) 2026 DatanoiseTV
 */

#ifndef _RP1_PIO_I2S_H
#define _RP1_PIO_I2S_H

#include <linux/types.h>

#define PIO_I2S_MAX_SMS        4
#define PIO_I2S_CHANNELS_PER_SM 2    /* L + R per data line */
#define PIO_I2S_MAX_CHANNELS   (PIO_I2S_MAX_SMS * PIO_I2S_CHANNELS_PER_SM)

/* GPIO assignments (fixed by RPi5 40-pin header) */
#define PIO_I2S_BCLK_GPIO     18
#define PIO_I2S_LRCLK_GPIO    19

/* Default data GPIOs for PIO lanes (after the 4 DWC I2S data lines) */
#define PIO_I2S_DATA_BASE_GPIO 24   /* GPIO24-27 = SD4-SD7 */

/*
 * Pre-assembled PIO programs.
 *
 * I2S slave input (capture): sample data on BCLK rising edge.
 *   .wrap_target
 *     wait 0 gpio 18       ; BCLK low
 *     wait 1 gpio 18       ; BCLK rising edge
 *     in pins, 1           ; shift in 1 bit
 *   .wrap
 *
 * I2S slave output (playback): change data on BCLK falling edge.
 *   .wrap_target
 *     wait 1 gpio 18       ; BCLK high
 *     wait 0 gpio 18       ; BCLK falling edge
 *     out pins, 1          ; shift out 1 bit
 *   .wrap
 *
 * Both use autopush/autopull at 32 bits, MSB first.
 */
#define PIO_I2S_PROG_LEN      3

/* wait 0 gpio 18 */
#define PIO_WAIT_BCLK_LO      0x2012
/* wait 1 gpio 18 */
#define PIO_WAIT_BCLK_HI      0x2092
/* wait 0 gpio 19 (LRCLK sync) */
#define PIO_WAIT_LRCLK_LO     0x2013
/* wait 1 gpio 19 (LRCLK sync) */
#define PIO_WAIT_LRCLK_HI     0x2093
/* in pins, 1 */
#define PIO_IN_PINS_1          0x4001
/* out pins, 1 */
#define PIO_OUT_PINS_1         0x6001

enum pio_i2s_dir {
	PIO_I2S_DIR_CAPTURE = 0,
	PIO_I2S_DIR_PLAYBACK,
};

/*
 * Per-SM runtime state for DMA ring buffer management.
 * Each SM handles one data pin (2 channels).
 */
struct pio_i2s_sm_state {
	unsigned int sm_index;       /* PIO state machine number */
	unsigned int gpio;           /* data pin GPIO number */
	enum pio_i2s_dir dir;

	/* DMA ring buffer tracking */
	void *buf;                   /* pointer into ALSA ring buffer */
	unsigned int period_bytes;
	unsigned int num_periods;
	unsigned int buf_bytes;      /* total buffer size */
	unsigned int hw_ptr;         /* current position in bytes */
	bool running;
};

struct pio_i2s_dev {
	struct device *dev;
	void *pio;                   /* PIO handle from pio_open() */

	/* Program offsets in PIO instruction memory */
	unsigned int capture_offset;
	unsigned int playback_offset;
	bool capture_loaded;
	bool playback_loaded;

	/* Active state machines */
	struct pio_i2s_sm_state sms[PIO_I2S_MAX_SMS];
	unsigned int num_sms;

	/* Data pin configuration */
	unsigned int data_base_gpio;
	unsigned int num_data_pins;  /* 1-4 */

	/* Stream state */
	struct snd_pcm_substream *capture_substream;
	struct snd_pcm_substream *playback_substream;
};

#endif /* _RP1_PIO_I2S_H */
