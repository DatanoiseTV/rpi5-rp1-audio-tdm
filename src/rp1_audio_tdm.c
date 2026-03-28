// SPDX-License-Identifier: GPL-2.0
/*
 * rp1_audio_tdm.c - RP1 multi-peripheral I2S/TDM machine driver
 *
 * Combines RP1 i2s0 (8ch, master-capable, GPIO18-27 on 40-pin header)
 * and optionally i2s2 (slave-only, GPIO28-33, not on header) into a
 * single ALSA sound card. Supports standard I2S and TDM (DSP-A/B)
 * modes with optional MCLK output via GPCLK0 on GPIO4.
 *
 * Designed for low-latency operation: period sizes down to 32 frames
 * (0.67ms at 48kHz) are supported with the RP1 DMA engine.
 *
 * Hardware: Raspberry Pi 5 RP1 southbridge, Synopsys DesignWare I2S
 *
 * Copyright (c) 2026 DatanoiseTV
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_clk.h>
#include <linux/platform_device.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "rp1_audio_tdm.h"

#ifndef GIT_BRANCH
#define GIT_BRANCH "unknown"
#endif
#ifndef GIT_HASH
#define GIT_HASH "unknown"
#endif

/* --- DAI format helpers ------------------------------------------ */

static const struct {
	const char *name;
	unsigned int fmt;
} rp1_dai_fmt_map[] = {
	{ "i2s",     SND_SOC_DAIFMT_I2S     },
	{ "dsp-a",   SND_SOC_DAIFMT_DSP_A   },
	{ "dsp-b",   SND_SOC_DAIFMT_DSP_B   },
	{ "left-j",  SND_SOC_DAIFMT_LEFT_J  },
	{ "right-j", SND_SOC_DAIFMT_RIGHT_J },
};

static unsigned int rp1_parse_dai_fmt(const char *str)
{
	int i;

	if (str) {
		for (i = 0; i < ARRAY_SIZE(rp1_dai_fmt_map); i++)
			if (!strcmp(str, rp1_dai_fmt_map[i].name))
				return rp1_dai_fmt_map[i].fmt;
	}
	return SND_SOC_DAIFMT_I2S;
}

static const char *rp1_dai_fmt_name(unsigned int fmt)
{
	int i;

	fmt &= SND_SOC_DAIFMT_FORMAT_MASK;
	for (i = 0; i < ARRAY_SIZE(rp1_dai_fmt_map); i++)
		if (rp1_dai_fmt_map[i].fmt == fmt)
			return rp1_dai_fmt_map[i].name;
	return "i2s";
}

/* --- DAI link callbacks ------------------------------------------ */

static int rp1_tdm_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct rp1_audio_tdm_priv *priv = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	unsigned int fmt;
	int ret;

	fmt = priv->dai_fmt | SND_SOC_DAIFMT_NB_NF;
	fmt |= priv->is_master ? SND_SOC_DAIFMT_BP_FP : SND_SOC_DAIFMT_BC_FC;

	ret = snd_soc_dai_set_fmt(cpu_dai, fmt);
	if (ret == -EINVAL && priv->is_master) {
		/*
		 * DWC I2S COMP1 MODE_EN=0: hardware is slave-only.
		 * Transparently fall back instead of failing the card.
		 */
		dev_info(priv->dev, "%s: no master support, using slave mode\n",
			 rtd->dai_link->name);
		fmt &= ~SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK;
		fmt |= SND_SOC_DAIFMT_BC_FC;
		ret = snd_soc_dai_set_fmt(cpu_dai, fmt);
	}
	if (ret && ret != -ENOTSUPP)
		return dev_err_probe(priv->dev, ret,
				     "%s: CPU DAI set_fmt failed\n",
				     rtd->dai_link->name);

	ret = snd_soc_dai_set_fmt(codec_dai, fmt);
	if (ret && ret != -ENOTSUPP)
		return dev_err_probe(priv->dev, ret,
				     "%s: codec DAI set_fmt failed\n",
				     rtd->dai_link->name);

	if (priv->tdm_slots > 2) {
		unsigned int mask = GENMASK(priv->tdm_slots - 1, 0);

		ret = snd_soc_dai_set_tdm_slot(cpu_dai, mask, mask,
						priv->tdm_slots,
						priv->tdm_slot_width);
		if (ret)
			return dev_err_probe(priv->dev, ret,
					     "%s: TDM slot config failed\n",
					     rtd->dai_link->name);

		ret = snd_soc_dai_set_tdm_slot(codec_dai, mask, mask,
						priv->tdm_slots,
						priv->tdm_slot_width);
		if (ret && ret != -ENOTSUPP)
			return dev_err_probe(priv->dev, ret,
					     "%s: codec TDM slot config failed\n",
					     rtd->dai_link->name);
	}

	return 0;
}

/*
 * Apply low-latency PCM constraints at stream open time.
 * This allows userspace (JACK, PipeWire) to request small periods
 * without being rejected by overly conservative defaults.
 */
static int rp1_tdm_startup(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	/* Allow small period sizes for low-latency operation */
	ret = snd_pcm_hw_constraint_minmax(runtime,
					    SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					    RP1_PERIOD_BYTES_MIN, UINT_MAX);
	if (ret < 0)
		return ret;

	ret = snd_pcm_hw_constraint_minmax(runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS,
					    RP1_PERIODS_MIN, UINT_MAX);
	if (ret < 0)
		return ret;

	return 0;
}

static int rp1_tdm_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct rp1_audio_tdm_priv *priv = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	unsigned int rate = params_rate(params);
	unsigned int bclk_ratio;
	int ret;

	if (priv->tdm_slots > 2)
		bclk_ratio = priv->tdm_slot_width * priv->tdm_slots;
	else
		bclk_ratio = priv->tdm_slot_width * 2;

	ret = snd_soc_dai_set_bclk_ratio(cpu_dai, bclk_ratio);
	if (ret && ret != -ENOTSUPP)
		return dev_err_probe(priv->dev, ret,
				     "BCLK ratio %u rejected\n", bclk_ratio);

	if (priv->mclk_clk && priv->mclk_fs) {
		unsigned long mclk_rate = (unsigned long)rate * priv->mclk_fs;

		ret = clk_set_rate(priv->mclk_clk, mclk_rate);
		if (ret)
			return dev_err_probe(priv->dev, ret,
					     "MCLK rate %lu Hz failed\n",
					     mclk_rate);

		if (!priv->mclk_enabled) {
			ret = clk_prepare_enable(priv->mclk_clk);
			if (ret)
				return dev_err_probe(priv->dev, ret,
						     "MCLK enable failed\n");
			priv->mclk_enabled = true;
		}
	}

	return 0;
}

static int rp1_tdm_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct rp1_audio_tdm_priv *priv = snd_soc_card_get_drvdata(rtd->card);

	if (priv->mclk_enabled) {
		clk_disable_unprepare(priv->mclk_clk);
		priv->mclk_enabled = false;
	}

	return 0;
}

static const struct snd_soc_ops rp1_tdm_ops = {
	.startup   = rp1_tdm_startup,
	.hw_params = rp1_tdm_hw_params,
	.hw_free   = rp1_tdm_hw_free,
};

/* --- DAI link definitions ---------------------------------------- */

SND_SOC_DAILINK_DEFS(link0,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_CODEC("snd-soc-dummy", "snd-soc-dummy-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(link1,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_CODEC("snd-soc-dummy", "snd-soc-dummy-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

/* PIO I2S expansion (GPIO24-27, 8ch additional) */
SND_SOC_DAILINK_DEFS(link_pio,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_CODEC("snd-soc-dummy", "snd-soc-dummy-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

/* --- MCLK clock management -------------------------------------- */

static int rp1_tdm_parse_mclk(struct rp1_audio_tdm_priv *priv,
			       struct device_node *np)
{
	struct of_phandle_args clkspec;
	int ret;

	ret = of_parse_phandle_with_args(np, "mclk-clk", "#clock-cells",
					 0, &clkspec);
	if (ret)
		return ret;

	priv->mclk_clk = of_clk_get_from_provider(&clkspec);
	of_node_put(clkspec.np);

	if (IS_ERR(priv->mclk_clk)) {
		ret = PTR_ERR(priv->mclk_clk);
		priv->mclk_clk = NULL;
		return ret;
	}

	return 0;
}

static void rp1_tdm_release_mclk(void *data)
{
	struct rp1_audio_tdm_priv *priv = data;

	if (priv->mclk_enabled) {
		clk_disable_unprepare(priv->mclk_clk);
		priv->mclk_enabled = false;
	}
	clk_put(priv->mclk_clk);
	priv->mclk_clk = NULL;
}

/* --- Device tree parsing ----------------------------------------- */

static int rp1_tdm_parse_dt(struct rp1_audio_tdm_priv *priv)
{
	struct device_node *np = priv->dev->of_node;
	const char *str;
	u32 val;
	int ret;

	ret = of_property_read_string(np, "dai-format", &str);
	priv->dai_fmt = rp1_parse_dai_fmt(ret ? NULL : str);

	ret = of_property_read_string(np, "clock-role", &str);
	priv->is_master = ret || strcmp(str, "slave");

	if (of_property_read_u32(np, "tdm-slots", &val))
		val = 2;
	if (val > RP1_TDM_MAX_SLOTS)
		return dev_err_probe(priv->dev, -EINVAL,
				     "tdm-slots %u exceeds max %u\n",
				     val, RP1_TDM_MAX_SLOTS);
	priv->tdm_slots = val;

	if (of_property_read_u32(np, "tdm-slot-width", &val))
		val = RP1_TDM_SLOT_WIDTH;
	if (val != RP1_TDM_SLOT_WIDTH)
		dev_warn(priv->dev,
			 "DWC I2S requires 32-bit slot width, ignoring %u\n",
			 val);
	priv->tdm_slot_width = RP1_TDM_SLOT_WIDTH;

	if (of_property_read_u32(np, "mclk-fs", &val))
		val = 0;
	priv->mclk_fs = val;

	if (priv->mclk_fs) {
		ret = rp1_tdm_parse_mclk(priv, np);
		if (ret) {
			dev_info(priv->dev,
				 "MCLK clock unavailable, disabling (mclk-fs=%u)\n",
				 priv->mclk_fs);
			priv->mclk_fs = 0;
		} else {
			ret = devm_add_action_or_reset(priv->dev,
						       rp1_tdm_release_mclk,
						       priv);
			if (ret)
				return ret;
		}
	}

	if (priv->tdm_slots > 2 && priv->is_master)
		dev_notice(priv->dev,
			   "TDM master with %u slots needs the DWC I2S CCR patch\n",
			   priv->tdm_slots);

	dev_info(priv->dev, "format=%s slots=%u role=%s mclk_fs=%u\n",
		 rp1_dai_fmt_name(priv->dai_fmt), priv->tdm_slots,
		 priv->is_master ? "master" : "slave", priv->mclk_fs);

	return 0;
}

/* --- DAI link construction --------------------------------------- */

struct rp1_link_def {
	const char *name;
	const char *dt_prop;
	const char *dai_name;  /* NULL = auto-detect from DT */
	struct snd_soc_dai_link_component *cpus;
	int num_cpus;
	struct snd_soc_dai_link_component *codecs;
	int num_codecs;
	struct snd_soc_dai_link_component *platforms;
	int num_platforms;
	bool use_init;         /* whether to call rp1_tdm_dai_init */
};

static const struct rp1_link_def rp1_link_defs[] = {
	{
		.name       = "rp1-i2s0",
		.dt_prop    = "i2s-controller-0",
		.cpus       = link0_cpus,
		.num_cpus   = ARRAY_SIZE(link0_cpus),
		.codecs     = link0_codecs,
		.num_codecs = ARRAY_SIZE(link0_codecs),
		.platforms  = link0_platforms,
		.num_platforms = ARRAY_SIZE(link0_platforms),
		.use_init   = true,
	},
	{
		.name       = "rp1-i2s2",
		.dt_prop    = "i2s-controller-1",
		.cpus       = link1_cpus,
		.num_cpus   = ARRAY_SIZE(link1_cpus),
		.codecs     = link1_codecs,
		.num_codecs = ARRAY_SIZE(link1_codecs),
		.platforms  = link1_platforms,
		.num_platforms = ARRAY_SIZE(link1_platforms),
		.use_init   = true,
	},
	{
		.name       = "rp1-pio-i2s",
		.dt_prop    = "pio-i2s",
		.dai_name   = "rp1-pio-i2s-dai",
		.cpus       = link_pio_cpus,
		.num_cpus   = ARRAY_SIZE(link_pio_cpus),
		.codecs     = link_pio_codecs,
		.num_codecs = ARRAY_SIZE(link_pio_codecs),
		.platforms  = link_pio_platforms,
		.num_platforms = ARRAY_SIZE(link_pio_platforms),
		.use_init   = false, /* PIO has its own PCM ops */
	},
};

static void rp1_tdm_release_of_nodes(void *data)
{
	struct rp1_audio_tdm_priv *priv = data;
	int i;

	for (i = 0; i < RP1_TDM_MAX_LINKS; i++) {
		of_node_put(priv->link_nodes[i]);
		priv->link_nodes[i] = NULL;
	}
}

static int rp1_tdm_setup_links(struct rp1_audio_tdm_priv *priv)
{
	struct device_node *np = priv->dev->of_node;
	unsigned int n = 0;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(rp1_link_defs); i++) {
		struct device_node *i2s_np;
		struct snd_soc_dai_link *link;

		i2s_np = of_parse_phandle(np, rp1_link_defs[i].dt_prop, 0);
		if (!i2s_np)
			continue;

		priv->link_nodes[i] = i2s_np;

		link = &priv->dai_links[n];
		link->name          = rp1_link_defs[i].name;
		link->stream_name   = rp1_link_defs[i].name;
		link->cpus          = rp1_link_defs[i].cpus;
		link->num_cpus      = rp1_link_defs[i].num_cpus;
		link->codecs        = rp1_link_defs[i].codecs;
		link->num_codecs    = rp1_link_defs[i].num_codecs;
		link->platforms     = rp1_link_defs[i].platforms;
		link->num_platforms = rp1_link_defs[i].num_platforms;
		if (rp1_link_defs[i].use_init) {
			link->init = rp1_tdm_dai_init;
			link->ops  = &rp1_tdm_ops;
		}

		link->cpus->of_node      = i2s_np;
		link->cpus->dai_name     = rp1_link_defs[i].dai_name;
		link->platforms->of_node = i2s_np;

		n++;
	}

	if (!n)
		return dev_err_probe(priv->dev, -EINVAL,
				     "no I2S controllers in device tree\n");

	/* Ensure of_node references are released on driver unbind */
	ret = devm_add_action_or_reset(priv->dev, rp1_tdm_release_of_nodes,
				       priv);
	if (ret)
		return ret;

	priv->num_links = n;
	return 0;
}

/* --- Platform driver --------------------------------------------- */

static int rp1_audio_tdm_probe(struct platform_device *pdev)
{
	struct rp1_audio_tdm_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;

	ret = rp1_tdm_parse_dt(priv);
	if (ret)
		return ret;

	ret = rp1_tdm_setup_links(priv);
	if (ret)
		return ret;

	priv->card.name      = RP1_TDM_DRV_NAME;
	priv->card.owner     = THIS_MODULE;
	priv->card.dev       = &pdev->dev;
	priv->card.dai_link  = priv->dai_links;
	priv->card.num_links = priv->num_links;
	snd_soc_card_set_drvdata(&priv->card, priv);

	ret = devm_snd_soc_register_card(&pdev->dev, &priv->card);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "card registration failed\n");

	dev_info(&pdev->dev, "%u link(s) active [%s %s]\n",
		 priv->num_links, GIT_BRANCH, GIT_HASH);

	return 0;
}

static const struct of_device_id rp1_audio_tdm_of_match[] = {
	{ .compatible = "datanoisetv,rp1-audio-tdm" },
	{ }
};
MODULE_DEVICE_TABLE(of, rp1_audio_tdm_of_match);

static struct platform_driver rp1_audio_tdm_driver = {
	.probe  = rp1_audio_tdm_probe,
	.driver = {
		.name           = RP1_TDM_DRV_NAME,
		.of_match_table = rp1_audio_tdm_of_match,
	},
};
module_platform_driver(rp1_audio_tdm_driver);

MODULE_AUTHOR("DatanoiseTV");
MODULE_DESCRIPTION("RP1 multi-peripheral I2S/TDM machine driver");
MODULE_LICENSE("GPL");
