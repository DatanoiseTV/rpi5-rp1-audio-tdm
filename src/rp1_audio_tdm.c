// SPDX-License-Identifier: GPL-2.0
/*
 * rp1_audio_tdm.c - RP1 multi-peripheral I2S/TDM machine driver
 *
 * Supports up to 12 channels full-duplex by combining RP1 i2s0 (8ch)
 * and i2s2 (4ch). Configurable for standard I2S or TDM (DSP-A/B) modes
 * with optional MCLK output via GPCLK.
 *
 * Hardware: Raspberry Pi 5 RP1 southbridge, Synopsys DesignWare I2S
 *
 * Copyright (c) 2026 DatanoiseTV
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>

#include "rp1_audio_tdm.h"

/* Build version info injected at compile time */
#ifndef GIT_BRANCH
#define GIT_BRANCH "unknown"
#endif
#ifndef GIT_HASH
#define GIT_HASH "unknown"
#endif

/*
 * Parse the "dai-format" string from device tree into an ALSA constant.
 * Returns SND_SOC_DAIFMT_I2S as a safe default.
 */
static unsigned int rp1_tdm_parse_dai_fmt(const char *fmt_str)
{
	if (!fmt_str)
		return SND_SOC_DAIFMT_I2S;

	if (!strcmp(fmt_str, RP1_FMT_DSP_A))
		return SND_SOC_DAIFMT_DSP_A;
	if (!strcmp(fmt_str, RP1_FMT_DSP_B))
		return SND_SOC_DAIFMT_DSP_B;
	if (!strcmp(fmt_str, RP1_FMT_LEFT_J))
		return SND_SOC_DAIFMT_LEFT_J;
	if (!strcmp(fmt_str, RP1_FMT_RIGHT_J))
		return SND_SOC_DAIFMT_RIGHT_J;

	return SND_SOC_DAIFMT_I2S;
}

/*
 * Compute the TDM slot mask for the given number of slots.
 * Returns a contiguous bitmask starting from slot 0.
 */
static unsigned int rp1_tdm_slot_mask(unsigned int slots)
{
	if (slots == 0 || slots > RP1_TDM_MAX_SLOTS)
		return 0;
	return (1U << slots) - 1;
}

/*
 * Configure TDM parameters on the CPU DAI at startup.
 * Called once per DAI link when a stream is opened.
 */
static int rp1_tdm_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct rp1_audio_tdm_priv *priv = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	unsigned int fmt;
	unsigned int tdm_mask;
	int ret;

	/* Assemble the full DAI format word */
	fmt = priv->dai_fmt | SND_SOC_DAIFMT_NB_NF;
	if (priv->is_master)
		fmt |= SND_SOC_DAIFMT_BP_FP;
	else
		fmt |= SND_SOC_DAIFMT_BC_FC;

	ret = snd_soc_dai_set_fmt(cpu_dai, fmt);
	if (ret && ret != -ENOTSUPP) {
		dev_err(priv->dev, "failed to set CPU DAI format: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_fmt(codec_dai, fmt);
	if (ret && ret != -ENOTSUPP) {
		dev_err(priv->dev, "failed to set codec DAI format: %d\n", ret);
		return ret;
	}

	/* Configure TDM slots if we're actually using TDM (>2 slots) */
	if (priv->tdm_slots > 2) {
		tdm_mask = rp1_tdm_slot_mask(priv->tdm_slots);

		ret = snd_soc_dai_set_tdm_slot(cpu_dai,
						tdm_mask, tdm_mask,
						priv->tdm_slots,
						priv->tdm_slot_width);
		if (ret) {
			dev_err(priv->dev,
				"failed to set TDM slots on CPU DAI: %d\n",
				ret);
			return ret;
		}

		ret = snd_soc_dai_set_tdm_slot(codec_dai,
						tdm_mask, tdm_mask,
						priv->tdm_slots,
						priv->tdm_slot_width);
		if (ret && ret != -ENOTSUPP) {
			dev_err(priv->dev,
				"failed to set TDM slots on codec DAI: %d\n",
				ret);
			return ret;
		}
	}

	return 0;
}

/*
 * hw_params callback: set BCLK ratio and MCLK rate based on the
 * actual stream parameters.
 */
static int rp1_tdm_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct rp1_audio_tdm_priv *priv = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	unsigned int sample_rate = params_rate(params);
	unsigned int bclk_ratio;
	int ret;

	/*
	 * Calculate BCLK ratio:
	 * - Standard I2S: slot_width * 2 (L+R) = 64 for 32-bit
	 * - TDM mode: slot_width * tdm_slots
	 *
	 * The DWC I2S driver uses bclk_ratio to set the clock divider.
	 * In master mode with TDM, the upstream driver's CCR check only
	 * allows 32/48/64. For TDM with >2 slots in master mode, the
	 * kernel patch from patches/ is required.
	 *
	 * In slave mode, bclk_ratio is informational only -- the external
	 * codec drives BCLK, so no CCR limitation applies.
	 */
	if (priv->tdm_slots > 2)
		bclk_ratio = priv->tdm_slot_width * priv->tdm_slots;
	else
		bclk_ratio = priv->tdm_slot_width * 2;

	ret = snd_soc_dai_set_bclk_ratio(cpu_dai, bclk_ratio);
	if (ret && ret != -ENOTSUPP) {
		dev_err(priv->dev, "failed to set BCLK ratio %u: %d\n",
			bclk_ratio, ret);
		return ret;
	}

	/* Set MCLK rate if configured */
	if (priv->mclk_clk && priv->mclk_fs) {
		unsigned long mclk_rate = sample_rate * priv->mclk_fs;

		ret = clk_set_rate(priv->mclk_clk, mclk_rate);
		if (ret) {
			dev_err(priv->dev,
				"failed to set MCLK to %lu Hz: %d\n",
				mclk_rate, ret);
			return ret;
		}

		ret = clk_prepare_enable(priv->mclk_clk);
		if (ret) {
			dev_err(priv->dev,
				"failed to enable MCLK: %d\n", ret);
			return ret;
		}

		dev_dbg(priv->dev, "MCLK set to %lu Hz (%u * %u)\n",
			mclk_rate, sample_rate, priv->mclk_fs);
	}

	dev_dbg(priv->dev,
		"hw_params: rate=%u channels=%u bclk_ratio=%u fmt=0x%x\n",
		sample_rate, params_channels(params), bclk_ratio,
		priv->dai_fmt);

	return 0;
}

static void rp1_tdm_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct rp1_audio_tdm_priv *priv = snd_soc_card_get_drvdata(rtd->card);

	if (priv->mclk_clk && priv->mclk_fs)
		clk_disable_unprepare(priv->mclk_clk);
}

static const struct snd_soc_ops rp1_tdm_ops = {
	.hw_params = rp1_tdm_hw_params,
	.hw_free = rp1_tdm_hw_free,
};

/*
 * ALSA codec component -- we use snd-soc-dummy when no real codec is
 * specified in the device tree. This allows direct I2S/TDM output to
 * external hardware (FPGA, DSP, standalone DAC/ADC with fixed config).
 */
SND_SOC_DAILINK_DEFS(rp1_tdm_link0,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_CODEC("snd-soc-dummy", "snd-soc-dummy-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(rp1_tdm_link1,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_CODEC("snd-soc-dummy", "snd-soc-dummy-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

/*
 * Read configuration from the device tree and validate constraints.
 */
static int rp1_tdm_parse_dt(struct rp1_audio_tdm_priv *priv)
{
	struct device_node *np = priv->dev->of_node;
	const char *str;
	u32 val;
	int ret;

	/* DAI format */
	ret = of_property_read_string(np, "dai-format", &str);
	priv->dai_fmt = rp1_tdm_parse_dai_fmt(ret ? NULL : str);

	/* Clock role */
	ret = of_property_read_string(np, "clock-role", &str);
	if (!ret && !strcmp(str, RP1_ROLE_SLAVE))
		priv->is_master = false;
	else
		priv->is_master = true;

	/* TDM slots (default 2 = standard I2S) */
	ret = of_property_read_u32(np, "tdm-slots", &val);
	priv->tdm_slots = ret ? 2 : val;

	if (priv->tdm_slots > RP1_TDM_MAX_SLOTS) {
		dev_err(priv->dev, "tdm-slots %u exceeds max %u\n",
			priv->tdm_slots, RP1_TDM_MAX_SLOTS);
		return -EINVAL;
	}

	/* TDM slot width (DWC I2S only supports 32) */
	ret = of_property_read_u32(np, "tdm-slot-width", &val);
	priv->tdm_slot_width = ret ? RP1_TDM_SLOT_WIDTH : val;

	if (priv->tdm_slot_width != RP1_TDM_SLOT_WIDTH) {
		dev_warn(priv->dev,
			 "DWC I2S only supports 32-bit slot width, forcing 32\n");
		priv->tdm_slot_width = RP1_TDM_SLOT_WIDTH;
	}

	/* MCLK configuration (optional) */
	ret = of_property_read_u32(np, "mclk-fs", &val);
	priv->mclk_fs = ret ? 0 : val;

	if (priv->mclk_fs) {
		priv->mclk_clk = devm_clk_get(priv->dev, "mclk");
		if (IS_ERR(priv->mclk_clk)) {
			/* Try getting it by phandle */
			priv->mclk_clk = of_clk_get_by_name(np, "mclk");
			if (IS_ERR(priv->mclk_clk)) {
				dev_warn(priv->dev,
					 "MCLK requested (mclk-fs=%u) but clock not available, disabling\n",
					 priv->mclk_fs);
				priv->mclk_clk = NULL;
				priv->mclk_fs = 0;
			}
		}
	}

	/* Warn about TDM master mode limitation */
	if (priv->tdm_slots > 2 && priv->is_master) {
		dev_warn(priv->dev,
			 "TDM master mode with %u slots requires the DWC I2S CCR kernel patch\n",
			 priv->tdm_slots);
	}

	dev_info(priv->dev,
		 "config: fmt=%s slots=%u width=%u role=%s mclk_fs=%u\n",
		 str ? str : "i2s", priv->tdm_slots, priv->tdm_slot_width,
		 priv->is_master ? "master" : "slave", priv->mclk_fs);

	return 0;
}

/*
 * Build the DAI link array based on which I2S controllers are
 * referenced in the device tree.
 */
static int rp1_tdm_create_dai_links(struct rp1_audio_tdm_priv *priv)
{
	struct device_node *np = priv->dev->of_node;
	struct device_node *i2s_np[RP1_TDM_MAX_CONTROLLERS] = {};
	struct snd_soc_dai_link *links;
	unsigned int num_links = 0;
	unsigned int channels_max;
	int i;

	/* Gather I2S controller references */
	i2s_np[0] = of_parse_phandle(np, "i2s-controller-0", 0);
	i2s_np[1] = of_parse_phandle(np, "i2s-controller-1", 0);

	for (i = 0; i < RP1_TDM_MAX_CONTROLLERS; i++) {
		if (i2s_np[i])
			num_links++;
	}

	if (num_links == 0) {
		dev_err(priv->dev, "no I2S controllers specified\n");
		return -EINVAL;
	}

	links = devm_kcalloc(priv->dev, num_links, sizeof(*links), GFP_KERNEL);
	if (!links)
		return -ENOMEM;

	num_links = 0;

	/* Link 0: i2s0 (up to 8 channels) */
	if (i2s_np[0]) {
		/*
		 * In TDM mode, max channels is bounded by both the FIFO
		 * count (8) and the number of active TDM slots.
		 * In standard I2S, it depends on active data lines.
		 */
		if (priv->tdm_slots > 2)
			channels_max = min_t(unsigned int,
					     priv->tdm_slots,
					     RP1_I2S0_MAX_CHANNELS);
		else
			channels_max = RP1_I2S0_MAX_CHANNELS;

		links[num_links].name = "rp1-i2s0";
		links[num_links].stream_name = "rp1-i2s0-stream";
		links[num_links].cpus = rp1_tdm_link0_cpus;
		links[num_links].num_cpus = ARRAY_SIZE(rp1_tdm_link0_cpus);
		links[num_links].codecs = rp1_tdm_link0_codecs;
		links[num_links].num_codecs = ARRAY_SIZE(rp1_tdm_link0_codecs);
		links[num_links].platforms = rp1_tdm_link0_platforms;
		links[num_links].num_platforms =
			ARRAY_SIZE(rp1_tdm_link0_platforms);
		links[num_links].cpus->of_node = i2s_np[0];
		links[num_links].platforms->of_node = i2s_np[0];
		links[num_links].init = rp1_tdm_dai_init;
		links[num_links].ops = &rp1_tdm_ops;
		links[num_links].dai_fmt = SND_SOC_DAIFMT_NB_NF;

		/* The dummy codec needs to know the channel limits */
		links[num_links].codecs->dai_name = "snd-soc-dummy-dai";

		num_links++;
	}

	/* Link 1: i2s2 (up to 4 channels full-duplex on 6 GPIO pins) */
	if (i2s_np[1]) {
		if (priv->tdm_slots > 2)
			channels_max = min_t(unsigned int,
					     priv->tdm_slots,
					     RP1_I2S2_MAX_CHANNELS);
		else
			channels_max = RP1_I2S2_MAX_CHANNELS;

		links[num_links].name = "rp1-i2s2";
		links[num_links].stream_name = "rp1-i2s2-stream";
		links[num_links].cpus = rp1_tdm_link1_cpus;
		links[num_links].num_cpus = ARRAY_SIZE(rp1_tdm_link1_cpus);
		links[num_links].codecs = rp1_tdm_link1_codecs;
		links[num_links].num_codecs = ARRAY_SIZE(rp1_tdm_link1_codecs);
		links[num_links].platforms = rp1_tdm_link1_platforms;
		links[num_links].num_platforms =
			ARRAY_SIZE(rp1_tdm_link1_platforms);
		links[num_links].cpus->of_node = i2s_np[1];
		links[num_links].platforms->of_node = i2s_np[1];
		links[num_links].init = rp1_tdm_dai_init;
		links[num_links].ops = &rp1_tdm_ops;
		links[num_links].dai_fmt = SND_SOC_DAIFMT_NB_NF;

		links[num_links].codecs->dai_name = "snd-soc-dummy-dai";

		num_links++;
	}

	priv->dai_links = links;
	priv->num_links = num_links;

	/* Release DT node references */
	for (i = 0; i < RP1_TDM_MAX_CONTROLLERS; i++)
		of_node_put(i2s_np[i]);

	return 0;
}

static int rp1_audio_tdm_probe(struct platform_device *pdev)
{
	struct rp1_audio_tdm_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	platform_set_drvdata(pdev, priv);

	ret = rp1_tdm_parse_dt(priv);
	if (ret)
		return ret;

	ret = rp1_tdm_create_dai_links(priv);
	if (ret)
		return ret;

	priv->card.name = "rp1-audio-tdm";
	priv->card.owner = THIS_MODULE;
	priv->card.dev = &pdev->dev;
	priv->card.dai_link = priv->dai_links;
	priv->card.num_links = priv->num_links;

	snd_soc_card_set_drvdata(&priv->card, priv);

	ret = devm_snd_soc_register_card(&pdev->dev, &priv->card);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"failed to register sound card: %d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev,
		 "rp1-audio-tdm: %u link(s), branch=%s hash=%s\n",
		 priv->num_links, GIT_BRANCH, GIT_HASH);

	return 0;
}

static const struct of_device_id rp1_audio_tdm_of_match[] = {
	{ .compatible = "datanoisetv,rp1-audio-tdm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rp1_audio_tdm_of_match);

static struct platform_driver rp1_audio_tdm_driver = {
	.probe = rp1_audio_tdm_probe,
	.driver = {
		.name = "rp1-audio-tdm",
		.of_match_table = rp1_audio_tdm_of_match,
	},
};
module_platform_driver(rp1_audio_tdm_driver);

MODULE_AUTHOR("DatanoiseTV");
MODULE_DESCRIPTION("RP1 multi-peripheral I2S/TDM audio driver");
MODULE_LICENSE("GPL");
