/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rp1_pio_i2s.h - PIO-based I2S data lane expansion for RP1
 *
 * Copyright (c) 2026 DatanoiseTV
 */

#ifndef _RP1_PIO_I2S_H
#define _RP1_PIO_I2S_H

#include <linux/types.h>
#include <linux/workqueue.h>

#define PIO_I2S_MAX_SMS         4
#define PIO_I2S_CHANNELS_PER_SM 2
#define PIO_I2S_MAX_CHANNELS    (PIO_I2S_MAX_SMS * PIO_I2S_CHANNELS_PER_SM)

#define PIO_I2S_BCLK_GPIO      18
#define PIO_I2S_LRCLK_GPIO     19
#define PIO_I2S_DATA_BASE_GPIO  24

/* Pre-assembled PIO instructions (3 per program) */
#define PIO_I2S_PROG_LEN       3

#define PIO_WAIT_BCLK_LO       0x2012  /* wait 0 gpio 18 */
#define PIO_WAIT_BCLK_HI       0x2092  /* wait 1 gpio 18 */
#define PIO_WAIT_LRCLK_LO      0x2013  /* wait 0 gpio 19 */
#define PIO_WAIT_LRCLK_HI      0x2093  /* wait 1 gpio 19 */
#define PIO_IN_PINS_1           0x4001  /* in pins, 1     */
#define PIO_OUT_PINS_1          0x6001  /* out pins, 1    */

enum pio_i2s_dir {
	PIO_I2S_DIR_CAPTURE = 0,
	PIO_I2S_DIR_PLAYBACK,
};

struct pio_i2s_sm_state {
	unsigned int sm_index;
	unsigned int gpio;
	enum pio_i2s_dir dir;

	/* DMA ring buffer tracking */
	unsigned char *dma_area;
	unsigned int period_bytes;
	unsigned int buf_bytes;
	unsigned int hw_ptr;
	bool running;
	bool dma_started;

	/* Work for DMA chaining (xfer_data can sleep) */
	struct work_struct dma_work;
	struct pio_i2s_dev *piod;
};

struct pio_i2s_dev {
	struct device *dev;
	void *pio;

	unsigned int capture_offset;
	unsigned int playback_offset;
	bool capture_loaded;
	bool playback_loaded;

	struct pio_i2s_sm_state sms[PIO_I2S_MAX_SMS];
	unsigned int num_sms;
	unsigned int data_base_gpio;
	unsigned int num_data_pins;

	struct snd_pcm_substream *capture_substream;
	struct snd_pcm_substream *playback_substream;

	struct workqueue_struct *wq;
};

#endif /* _RP1_PIO_I2S_H */
