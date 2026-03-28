/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rp1_audio_tdm.h - RP1 multi-peripheral I2S/TDM machine driver
 *
 * Copyright (c) 2026 DatanoiseTV
 */

#ifndef _RP1_AUDIO_TDM_H
#define _RP1_AUDIO_TDM_H

#include <linux/clk.h>
#include <linux/device.h>
#include <sound/soc.h>

#define RP1_TDM_DRV_NAME         "rp1-audio-tdm"
#define RP1_TDM_MAX_CONTROLLERS  2
#define RP1_TDM_MAX_SLOTS        16
#define RP1_TDM_SLOT_WIDTH       32   /* DWC I2S hardware constraint */

/* 4 DWC I2S FIFO pairs per controller = 8 channels max */
#define RP1_I2S_MAX_CHANNELS     8

/*
 * Low-latency PCM constraints.
 * Period of 32 frames at 48kHz = 0.67ms.
 * Minimum 2 periods for double-buffering.
 */
#define RP1_PERIOD_BYTES_MIN     256
#define RP1_PERIODS_MIN          2

struct rp1_audio_tdm_priv {
	struct snd_soc_card card;
	struct device *dev;

	struct snd_soc_dai_link dai_links[RP1_TDM_MAX_CONTROLLERS];
	struct device_node *i2s_nodes[RP1_TDM_MAX_CONTROLLERS];
	unsigned int num_links;

	unsigned int dai_fmt;
	unsigned int tdm_slots;
	unsigned int tdm_slot_width;
	unsigned int mclk_fs;
	bool is_master;

	struct clk *mclk_clk;
	bool mclk_enabled;
};

#endif /* _RP1_AUDIO_TDM_H */
