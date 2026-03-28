// SPDX-License-Identifier: GPL-2.0
/*
 * rp1_pio_i2s.c - PIO-based I2S data lane expansion for RP1
 *
 * Implements additional I2S data lanes using the RP1 PIO engine,
 * synchronized to the hardware I2S BCLK on GPIO18. Each PIO state
 * machine handles one data pin, shifting 32-bit samples in or out
 * on BCLK edges with DMA to/from memory.
 *
 * The PIO programs are 3 instructions each and run from clk_sys
 * (200 MHz), giving ~65 PIO cycles per BCLK edge at 48kHz -- well
 * within timing margins.
 *
 * Copyright (c) 2026 DatanoiseTV
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/pio_rp1.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "rp1_pio_i2s.h"

/* PIO I2S capture program: sample data on BCLK rising edge */
static const uint16_t pio_i2s_capture_prog[] = {
	PIO_WAIT_BCLK_LO,   /* wait 0 gpio 18 */
	PIO_WAIT_BCLK_HI,   /* wait 1 gpio 18 */
	PIO_IN_PINS_1,       /* in pins, 1     */
};

/* PIO I2S playback program: output data on BCLK falling edge */
static const uint16_t pio_i2s_playback_prog[] = {
	PIO_WAIT_BCLK_HI,   /* wait 1 gpio 18 */
	PIO_WAIT_BCLK_LO,   /* wait 0 gpio 18 */
	PIO_OUT_PINS_1,      /* out pins, 1    */
};

static const struct pio_program capture_program = {
	.instructions = pio_i2s_capture_prog,
	.length = PIO_I2S_PROG_LEN,
	.origin = -1,
};

static const struct pio_program playback_program = {
	.instructions = pio_i2s_playback_prog,
	.length = PIO_I2S_PROG_LEN,
	.origin = -1,
};

/* --- PIO state machine setup ------------------------------------- */

/*
 * Configure one PIO SM for I2S operation on a single data pin.
 * The SM runs the capture or playback program, with autopush/autopull
 * at 32 bits (one sample = 32 BCLK cycles).
 */
static int pio_i2s_configure_sm(struct pio_i2s_dev *piod,
				struct pio_i2s_sm_state *sm,
				enum pio_i2s_dir dir)
{
	PIO pio = piod->pio;
	pio_sm_config c;
	uint offset;

	if (dir == PIO_I2S_DIR_CAPTURE) {
		if (!piod->capture_loaded) {
			piod->capture_offset = pio_add_program(pio,
							       &capture_program);
			if (piod->capture_offset == PIO_ORIGIN_ANY)
				return -ENOMEM;
			piod->capture_loaded = true;
		}
		offset = piod->capture_offset;
	} else {
		if (!piod->playback_loaded) {
			piod->playback_offset = pio_add_program(pio,
								&playback_program);
			if (piod->playback_offset == PIO_ORIGIN_ANY)
				return -ENOMEM;
			piod->playback_loaded = true;
		}
		offset = piod->playback_offset;
	}

	sm->dir = dir;

	/* Set data pin to PIO function */
	pio_gpio_init(pio, sm->gpio);

	if (dir == PIO_I2S_DIR_PLAYBACK)
		pio_sm_set_consecutive_pindirs(pio, sm->sm_index,
					       sm->gpio, 1, true);
	else
		pio_sm_set_consecutive_pindirs(pio, sm->sm_index,
					       sm->gpio, 1, false);

	c = pio_get_default_sm_config();
	sm_config_set_wrap(&c, offset, offset + PIO_I2S_PROG_LEN - 1);

	if (dir == PIO_I2S_DIR_CAPTURE) {
		sm_config_set_in_pins(&c, sm->gpio);
		/* MSB first, autopush at 32 bits */
		sm_config_set_in_shift(&c, false, true, 32);
		sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
	} else {
		sm_config_set_out_pins(&c, sm->gpio, 1);
		/* MSB first, autopull at 32 bits */
		sm_config_set_out_shift(&c, false, true, 32);
		sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
	}

	/*
	 * Clock divider = 1 (run at full clk_sys speed).
	 * The WAIT instructions handle synchronization to BCLK.
	 */
	sm_config_set_clkdiv(&c, make_fp24_8(1, 1));

	pio_sm_init(pio, sm->sm_index, offset, &c);

	return 0;
}

/*
 * Synchronize all PIO SMs to the I2S frame boundary.
 * Execute WAIT instructions to align to the LRCLK falling edge
 * (start of left channel), then enable all SMs simultaneously.
 */
static void pio_i2s_sync_and_start(struct pio_i2s_dev *piod)
{
	PIO pio = piod->pio;
	uint16_t sm_mask = 0;
	int i;

	/* Sync each SM to LRCLK frame boundary */
	for (i = 0; i < piod->num_sms; i++) {
		struct pio_i2s_sm_state *sm = &piod->sms[i];

		/* Execute sync instructions: wait for LRCLK high then low */
		pio_sm_exec(pio, sm->sm_index, PIO_WAIT_LRCLK_HI);
		pio_sm_exec(pio, sm->sm_index, PIO_WAIT_LRCLK_LO);
		sm_mask |= BIT(sm->sm_index);
	}

	/* Start all SMs simultaneously for sample alignment */
	pio_sm_set_enabled(pio, sm_mask, true);
}

static void pio_i2s_stop_all(struct pio_i2s_dev *piod)
{
	PIO pio = piod->pio;
	uint16_t sm_mask = 0;
	int i;

	for (i = 0; i < piod->num_sms; i++) {
		piod->sms[i].running = false;
		sm_mask |= BIT(piod->sms[i].sm_index);
	}

	pio_sm_set_enabled(pio, sm_mask, false);
}

/* --- DMA ring buffer management ---------------------------------- */

/*
 * DMA completion callback. Called when one period of audio data has
 * been transferred between the PIO FIFO and memory.
 *
 * For capture: PIO FIFO -> bounce buffer -> ALSA ring buffer
 * For playback: ALSA ring buffer -> bounce buffer -> PIO FIFO
 */
static void pio_i2s_dma_complete(void *param)
{
	struct pio_i2s_sm_state *sm = param;
	struct pio_i2s_dev *piod;
	struct snd_pcm_substream *substream;

	if (!sm->running)
		return;

	/* Advance the hardware pointer */
	sm->hw_ptr += sm->period_bytes;
	if (sm->hw_ptr >= sm->buf_bytes)
		sm->hw_ptr = 0;

	/* Determine which substream this SM belongs to */
	piod = container_of(sm, struct pio_i2s_dev,
			    sms[sm->sm_index - piod->sms[0].sm_index]);

	if (sm->dir == PIO_I2S_DIR_CAPTURE)
		substream = piod->capture_substream;
	else
		substream = piod->playback_substream;

	/* Notify ALSA that a period has elapsed */
	if (substream)
		snd_pcm_period_elapsed(substream);

	/* Chain the next DMA transfer */
	if (sm->running) {
		void *buf_pos = sm->buf + sm->hw_ptr;

		pio_sm_xfer_data(piod->pio, sm->sm_index,
				 sm->dir == PIO_I2S_DIR_CAPTURE ?
				 PIO_DIR_FROM_SM : PIO_DIR_TO_SM,
				 sm->period_bytes, buf_pos, 0,
				 pio_i2s_dma_complete, sm);
	}
}

static int pio_i2s_start_dma(struct pio_i2s_dev *piod,
			      struct pio_i2s_sm_state *sm)
{
	void *buf_pos = sm->buf + sm->hw_ptr;
	int dir;

	sm->running = true;
	dir = (sm->dir == PIO_I2S_DIR_CAPTURE) ?
	      PIO_DIR_FROM_SM : PIO_DIR_TO_SM;

	return pio_sm_xfer_data(piod->pio, sm->sm_index, dir,
				sm->period_bytes, buf_pos, 0,
				pio_i2s_dma_complete, sm);
}

/* --- ASoC component and DAI -------------------------------------- */

static const struct snd_pcm_hardware pio_i2s_pcm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_8000_192000,
	.rate_min = 8000,
	.rate_max = 192000,
	.channels_min = 2,
	.channels_max = PIO_I2S_MAX_CHANNELS,
	.period_bytes_min = 64,
	.period_bytes_max = 4096,
	.periods_min = 2,
	.periods_max = 32,
	.buffer_bytes_max = 4096 * 32,
};

static int pio_i2s_pcm_open(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream)
{
	struct pio_i2s_dev *piod = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;

	snd_soc_set_runtime_hwparams(substream, &pio_i2s_pcm_hardware);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		piod->capture_substream = substream;
	else
		piod->playback_substream = substream;

	return snd_pcm_hw_constraint_step(runtime, 0,
					  SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 2);
}

static int pio_i2s_pcm_close(struct snd_soc_component *component,
			      struct snd_pcm_substream *substream)
{
	struct pio_i2s_dev *piod = snd_soc_component_get_drvdata(component);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		piod->capture_substream = NULL;
	else
		piod->playback_substream = NULL;

	return 0;
}

static int pio_i2s_pcm_hw_params(struct snd_soc_component *component,
				  struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params)
{
	struct pio_i2s_dev *piod = snd_soc_component_get_drvdata(component);
	unsigned int channels = params_channels(params);
	unsigned int period_bytes = params_period_bytes(params);
	unsigned int num_periods = params_periods(params);
	unsigned int buf_bytes = params_buffer_bytes(params);
	unsigned int num_sms = channels / PIO_I2S_CHANNELS_PER_SM;
	enum pio_i2s_dir dir;
	int ret, i;

	if (channels % PIO_I2S_CHANNELS_PER_SM != 0 || num_sms > piod->num_data_pins)
		return -EINVAL;

	dir = (substream->stream == SNDRV_PCM_STREAM_CAPTURE) ?
	      PIO_I2S_DIR_CAPTURE : PIO_I2S_DIR_PLAYBACK;

	ret = snd_pcm_lib_malloc_pages(substream, buf_bytes);
	if (ret < 0)
		return ret;

	/*
	 * Configure PIO DMA transfer size for each SM.
	 * Each SM handles 2 channels (L+R), so it gets a fraction
	 * of the total period.
	 *
	 * For interleaved audio with N SMs:
	 *   Total period = period_bytes
	 *   Per-SM period = period_bytes / num_sms
	 *   (each SM reads/writes its 2-channel portion)
	 *
	 * Note: for this initial version, each SM gets its own
	 * contiguous buffer region. Interleaving to/from the ALSA
	 * buffer happens in software.
	 */
	for (i = 0; i < num_sms; i++) {
		struct pio_i2s_sm_state *sm = &piod->sms[i];

		ret = pio_i2s_configure_sm(piod, sm, dir);
		if (ret)
			return ret;

		/* Each SM transfers 2ch worth of the period */
		sm->period_bytes = (period_bytes / num_sms);
		sm->num_periods = num_periods;
		sm->buf_bytes = sm->period_bytes * num_periods;
		sm->hw_ptr = 0;

		/*
		 * Point each SM at its portion of the ALSA DMA buffer.
		 * SM0 gets the first chunk, SM1 the next, etc.
		 * This layout matches ALSA non-interleaved access.
		 */
		sm->buf = substream->runtime->dma_area +
			  (i * sm->buf_bytes);

		/* Configure PIO DMA bounce buffers */
		ret = pio_sm_config_xfer(piod->pio, sm->sm_index,
					 dir == PIO_I2S_DIR_CAPTURE ?
					 PIO_DIR_FROM_SM : PIO_DIR_TO_SM,
					 sm->period_bytes, 2);
		if (ret)
			return ret;
	}

	piod->num_sms = num_sms;

	dev_dbg(piod->dev,
		"pio_i2s: %s %uch rate=%u period=%u periods=%u sms=%u\n",
		dir == PIO_I2S_DIR_CAPTURE ? "capture" : "playback",
		channels, params_rate(params), period_bytes, num_periods,
		num_sms);

	return 0;
}

static int pio_i2s_pcm_hw_free(struct snd_soc_component *component,
				struct snd_pcm_substream *substream)
{
	struct pio_i2s_dev *piod = snd_soc_component_get_drvdata(component);

	pio_i2s_stop_all(piod);
	return snd_pcm_lib_free_pages(substream);
}

static int pio_i2s_pcm_trigger(struct snd_soc_component *component,
				struct snd_pcm_substream *substream, int cmd)
{
	struct pio_i2s_dev *piod = snd_soc_component_get_drvdata(component);
	int i, ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		/* Start DMA on all active SMs */
		for (i = 0; i < piod->num_sms; i++) {
			ret = pio_i2s_start_dma(piod, &piod->sms[i]);
			if (ret)
				return ret;
		}
		/* Sync to LRCLK and start all SMs */
		pio_i2s_sync_and_start(piod);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		pio_i2s_stop_all(piod);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static snd_pcm_uframes_t pio_i2s_pcm_pointer(
	struct snd_soc_component *component,
	struct snd_pcm_substream *substream)
{
	struct pio_i2s_dev *piod = snd_soc_component_get_drvdata(component);

	/*
	 * Return the position of SM0 as the canonical pointer.
	 * All SMs advance in lockstep since they're clocked by
	 * the same BCLK.
	 */
	if (piod->num_sms > 0) {
		unsigned int bytes = piod->sms[0].hw_ptr;

		return bytes_to_frames(substream->runtime, bytes);
	}

	return 0;
}

static int pio_i2s_pcm_new(struct snd_soc_component *component,
			    struct snd_soc_pcm_runtime *rtd)
{
	snd_pcm_lib_preallocate_pages_for_all(
		rtd->pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
		pio_i2s_pcm_hardware.buffer_bytes_max,
		pio_i2s_pcm_hardware.buffer_bytes_max);
	return 0;
}

static const struct snd_soc_component_driver pio_i2s_component_driver = {
	.name       = "rp1-pio-i2s",
	.open       = pio_i2s_pcm_open,
	.close      = pio_i2s_pcm_close,
	.hw_params  = pio_i2s_pcm_hw_params,
	.hw_free    = pio_i2s_pcm_hw_free,
	.trigger    = pio_i2s_pcm_trigger,
	.pointer    = pio_i2s_pcm_pointer,
	.pcm_construct = pio_i2s_pcm_new,
};

/* DAI: minimal, just declares capabilities */
static struct snd_soc_dai_driver pio_i2s_dai_driver = {
	.name = "rp1-pio-i2s-dai",
	.capture = {
		.stream_name  = "PIO I2S Capture",
		.channels_min = 2,
		.channels_max = PIO_I2S_MAX_CHANNELS,
		.rates        = SNDRV_PCM_RATE_8000_192000,
		.formats      = SNDRV_PCM_FMTBIT_S32_LE,
	},
	.playback = {
		.stream_name  = "PIO I2S Playback",
		.channels_min = 2,
		.channels_max = PIO_I2S_MAX_CHANNELS,
		.rates        = SNDRV_PCM_RATE_8000_192000,
		.formats      = SNDRV_PCM_FMTBIT_S32_LE,
	},
};

/* --- Platform driver --------------------------------------------- */

static int rp1_pio_i2s_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pio_i2s_dev *piod;
	unsigned int num_pins;
	u32 base_gpio;
	int ret, i;

	piod = devm_kzalloc(dev, sizeof(*piod), GFP_KERNEL);
	if (!piod)
		return -ENOMEM;

	piod->dev = dev;
	platform_set_drvdata(pdev, piod);

	/* Read data pin configuration from DT */
	if (of_property_read_u32(dev->of_node, "data-gpio-base", &base_gpio))
		base_gpio = PIO_I2S_DATA_BASE_GPIO;
	if (of_property_read_u32(dev->of_node, "data-gpio-count", &num_pins))
		num_pins = PIO_I2S_MAX_SMS;

	if (num_pins > PIO_I2S_MAX_SMS)
		num_pins = PIO_I2S_MAX_SMS;
	if (base_gpio + num_pins > RP1_PIO_GPIO_COUNT)
		return dev_err_probe(dev, -EINVAL,
				     "GPIO%u-%u out of PIO range (0-%u)\n",
				     base_gpio, base_gpio + num_pins - 1,
				     RP1_PIO_GPIO_COUNT - 1);

	piod->data_base_gpio = base_gpio;
	piod->num_data_pins = num_pins;

	/* Open PIO and claim state machines */
	piod->pio = pio_open();
	if (IS_ERR(piod->pio))
		return dev_err_probe(dev, PTR_ERR(piod->pio),
				     "failed to open PIO\n");

	for (i = 0; i < num_pins; i++) {
		int sm = pio_claim_unused_sm(piod->pio, false);

		if (sm < 0) {
			dev_err(dev, "not enough free PIO state machines\n");
			ret = -EBUSY;
			goto err_pio;
		}

		piod->sms[i].sm_index = sm;
		piod->sms[i].gpio = base_gpio + i;
	}

	/* Register ASoC component + DAI */
	pio_i2s_dai_driver.capture.channels_max = num_pins * PIO_I2S_CHANNELS_PER_SM;
	pio_i2s_dai_driver.playback.channels_max = num_pins * PIO_I2S_CHANNELS_PER_SM;

	ret = devm_snd_soc_register_component(dev, &pio_i2s_component_driver,
					      &pio_i2s_dai_driver, 1);
	if (ret)
		goto err_pio;

	dev_info(dev, "PIO I2S: %u data pins (GPIO%u-%u), %uch max\n",
		 num_pins, base_gpio, base_gpio + num_pins - 1,
		 num_pins * PIO_I2S_CHANNELS_PER_SM);

	return 0;

err_pio:
	pio_close(piod->pio);
	return ret;
}

static void rp1_pio_i2s_remove(struct platform_device *pdev)
{
	struct pio_i2s_dev *piod = platform_get_drvdata(pdev);

	pio_i2s_stop_all(piod);

	if (piod->capture_loaded)
		pio_remove_program(piod->pio, &capture_program,
				   piod->capture_offset);
	if (piod->playback_loaded)
		pio_remove_program(piod->pio, &playback_program,
				   piod->playback_offset);

	pio_close(piod->pio);
}

static const struct of_device_id rp1_pio_i2s_of_match[] = {
	{ .compatible = "datanoisetv,rp1-pio-i2s" },
	{ }
};
MODULE_DEVICE_TABLE(of, rp1_pio_i2s_of_match);

static struct platform_driver rp1_pio_i2s_driver = {
	.probe  = rp1_pio_i2s_probe,
	.remove = rp1_pio_i2s_remove,
	.driver = {
		.name           = "rp1-pio-i2s",
		.of_match_table = rp1_pio_i2s_of_match,
	},
};
module_platform_driver(rp1_pio_i2s_driver);

MODULE_AUTHOR("DatanoiseTV");
MODULE_DESCRIPTION("RP1 PIO-based I2S data lane expansion");
MODULE_LICENSE("GPL");
