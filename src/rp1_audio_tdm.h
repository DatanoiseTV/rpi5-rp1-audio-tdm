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

/* Per-controller max channels (4 DWC FIFO pairs x 2 = 8) */
#define RP1_I2S_MAX_CHANNELS     8

struct rp1_audio_tdm_priv {
	struct snd_soc_card card;
	struct device *dev;

	struct snd_soc_dai_link dai_links[RP1_TDM_MAX_CONTROLLERS];
	unsigned int num_links;

	/* Configuration from DT */
	unsigned int dai_fmt;        /* SND_SOC_DAIFMT_xxx */
	unsigned int tdm_slots;      /* 2 = standard I2S */
	unsigned int tdm_slot_width; /* always 32 for DWC I2S */
	unsigned int mclk_fs;        /* MCLK = mclk_fs * sample_rate */
	bool is_master;

	/* MCLK clock (optional, may be NULL) */
	struct clk *mclk_clk;
	bool mclk_enabled;
};

#endif /* _RP1_AUDIO_TDM_H */
