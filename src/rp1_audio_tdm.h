/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rp1_audio_tdm.h - RP1 multi-peripheral I2S/TDM audio driver
 *
 * Copyright (c) 2026 DatanoiseTV
 */

#ifndef _RP1_AUDIO_TDM_H
#define _RP1_AUDIO_TDM_H

#include <linux/types.h>

#define RP1_TDM_MAX_CONTROLLERS   2
#define RP1_TDM_MAX_SLOTS         16
#define RP1_TDM_SLOT_WIDTH        32

/* Per-controller channel limits based on DWC I2S FIFO pairs */
#define RP1_I2S0_MAX_CHANNELS     8   /* 4 FIFO pairs, GPIO18-27 */
#define RP1_I2S2_MAX_CHANNELS     8   /* 4 FIFO pairs, GPIO42-47 (but 4 data pins) */

/* DWC I2S CCR register values */
#define DWC_CCR_32BIT_FRAME       0x00
#define DWC_CCR_48BIT_FRAME       0x08
#define DWC_CCR_64BIT_FRAME       0x10

/* Supported DAI format strings from device tree */
#define RP1_FMT_I2S               "i2s"
#define RP1_FMT_DSP_A             "dsp-a"
#define RP1_FMT_DSP_B             "dsp-b"
#define RP1_FMT_LEFT_J            "left-j"
#define RP1_FMT_RIGHT_J           "right-j"

/* Clock role strings */
#define RP1_ROLE_MASTER           "master"
#define RP1_ROLE_SLAVE            "slave"

/* Channel count per DAI link when using both controllers */
struct rp1_tdm_link_config {
	unsigned int channels_min;
	unsigned int channels_max;
	unsigned int tdm_slots;
	unsigned int tdm_slot_width;
	unsigned int tdm_mask;
};

struct rp1_audio_tdm_priv {
	struct snd_soc_card card;
	struct device *dev;

	/* DAI links (one per active controller) */
	struct snd_soc_dai_link *dai_links;
	unsigned int num_links;

	/* Configuration from DT */
	unsigned int dai_fmt;
	unsigned int tdm_slots;
	unsigned int tdm_slot_width;
	unsigned int mclk_fs;
	bool is_master;

	/* MCLK clock handle */
	struct clk *mclk_clk;
};

#endif /* _RP1_AUDIO_TDM_H */
