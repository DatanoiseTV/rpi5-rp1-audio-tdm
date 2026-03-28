// SPDX-License-Identifier: GPL-2.0
/*
 * rp1_pio_i2s.c - PIO-based I2S data lane expansion for RP1
 *
 * Implements additional I2S data lanes using the RP1 PIO engine,
 * synchronized to the hardware I2S BCLK on GPIO18. Each PIO state
 * machine handles one data pin, shifting 32-bit samples in or out
 * on BCLK edges with DMA to/from memory.
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

/* --- PIO state machine configuration ----------------------------- */

static int pio_i2s_load_program(struct pio_i2s_dev *piod,
				enum pio_i2s_dir dir)
{
	PIO pio = piod->pio;
	const struct pio_program *prog;
	unsigned int *offset;
	bool *loaded;

	if (dir == PIO_I2S_DIR_CAPTURE) {
		prog = &capture_program;
		offset = &piod->capture_offset;
		loaded = &piod->capture_loaded;
	} else {
		prog = &playback_program;
		offset = &piod->playback_offset;
		loaded = &piod->playback_loaded;
	}

	if (*loaded)
		return 0;

	*offset = pio_add_program(pio, prog);
	if (*offset == PIO_ORIGIN_ANY)
		return -ENOMEM;

	*loaded = true;
	return 0;
}

static int pio_i2s_configure_sm(struct pio_i2s_dev *piod,
				struct pio_i2s_sm_state *sm)
{
	PIO pio = piod->pio;
	pio_sm_config c;
	unsigned int offset;
	bool is_output;

	is_output = (sm->dir == PIO_I2S_DIR_PLAYBACK);
	offset = is_output ? piod->playback_offset : piod->capture_offset;

	pio_gpio_init(pio, sm->gpio);
	pio_sm_set_consecutive_pindirs(pio, sm->sm_index,
				       sm->gpio, 1, is_output);

	c = pio_get_default_sm_config();
	sm_config_set_wrap(&c, offset, offset + PIO_I2S_PROG_LEN - 1);

	if (is_output) {
		sm_config_set_out_pins(&c, sm->gpio, 1);
		sm_config_set_out_shift(&c, false, true, 32);
		sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
	} else {
		sm_config_set_in_pins(&c, sm->gpio);
		sm_config_set_in_shift(&c, false, true, 32);
		sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
	}

	sm_config_set_clkdiv(&c, make_fp24_8(1, 1));
	pio_sm_init(pio, sm->sm_index, offset, &c);

	return 0;
}

/* --- DMA ring buffer management ---------------------------------- */

static void pio_i2s_dma_callback(void *param);

/*
 * Work handler for DMA chaining. Runs in process context so it
 * can call the PIO xfer_data API (which may sleep).
 */
static void pio_i2s_dma_work(struct work_struct *work)
{
	struct pio_i2s_sm_state *sm =
		container_of(work, struct pio_i2s_sm_state, dma_work);
	struct pio_i2s_dev *piod = sm->piod;
	struct snd_pcm_substream *substream;
	unsigned char *buf;
	int pio_dir;

	if (!sm->running)
		return;

	substream = (sm->dir == PIO_I2S_DIR_CAPTURE) ?
		    piod->capture_substream : piod->playback_substream;
	pio_dir = (sm->dir == PIO_I2S_DIR_CAPTURE) ?
		  PIO_DIR_FROM_SM : PIO_DIR_TO_SM;

	/* If this isn't the first transfer, a period just completed */
	if (sm->dma_started) {
		sm->hw_ptr += sm->period_bytes;
		if (sm->hw_ptr >= sm->buf_bytes)
			sm->hw_ptr = 0;

		if (substream)
			snd_pcm_period_elapsed(substream);
	}

	if (!sm->running)
		return;

	sm->dma_started = true;
	buf = sm->dma_area + sm->hw_ptr;

	pio_sm_xfer_data(piod->pio, sm->sm_index, pio_dir,
			 sm->period_bytes, buf, 0,
			 pio_i2s_dma_callback, sm);
}

/*
 * DMA completion callback. Runs in tasklet/softirq context.
 * Cannot call sleeping functions, so we schedule work.
 */
static void pio_i2s_dma_callback(void *param)
{
	struct pio_i2s_sm_state *sm = param;

	if (sm->running)
		queue_work(sm->piod->wq, &sm->dma_work);
}

/* --- ASoC component callbacks ------------------------------------ */

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
	.period_bytes_max = 4096,  /* PIO DMA bounce buffer limit */
	.periods_min = 2,
	.periods_max = 32,
	.buffer_bytes_max = 4096 * 32,
};

static int pio_i2s_pcm_open(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream)
{
	struct pio_i2s_dev *piod = snd_soc_component_get_drvdata(component);

	snd_soc_set_runtime_hwparams(substream, &pio_i2s_pcm_hardware);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		piod->capture_substream = substream;
	else
		piod->playback_substream = substream;

	return 0;
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
	unsigned int num_sms = channels / PIO_I2S_CHANNELS_PER_SM;
	enum pio_i2s_dir dir;
	int ret, i;

	if (channels % PIO_I2S_CHANNELS_PER_SM != 0 ||
	    num_sms > piod->num_data_pins)
		return -EINVAL;

	dir = (substream->stream == SNDRV_PCM_STREAM_CAPTURE) ?
	      PIO_I2S_DIR_CAPTURE : PIO_I2S_DIR_PLAYBACK;

	ret = pio_i2s_load_program(piod, dir);
	if (ret)
		return ret;

	for (i = 0; i < num_sms; i++) {
		struct pio_i2s_sm_state *sm = &piod->sms[i];

		sm->dir = dir;
		sm->period_bytes = period_bytes / num_sms;
		sm->buf_bytes = sm->period_bytes * num_periods;
		sm->hw_ptr = 0;
		sm->dma_started = false;
		sm->running = false;

		ret = pio_i2s_configure_sm(piod, sm);
		if (ret)
			return ret;

		/* Configure PIO DMA bounce buffers: 2 for double-buffering */
		ret = pio_sm_config_xfer(piod->pio, sm->sm_index,
					 dir == PIO_I2S_DIR_CAPTURE ?
					 PIO_DIR_FROM_SM : PIO_DIR_TO_SM,
					 sm->period_bytes, 2);
		if (ret) {
			dev_err(piod->dev,
				"PIO DMA config failed for SM%u: %d\n",
				sm->sm_index, ret);
			return ret;
		}
	}

	piod->num_sms = num_sms;

	dev_info(piod->dev,
		 "pio_i2s: %s %uch rate=%u period=%u(%u bytes) sms=%u\n",
		 dir == PIO_I2S_DIR_CAPTURE ? "capture" : "playback",
		 channels, params_rate(params),
		 params_period_size(params), period_bytes, num_sms);

	return 0;
}

static int pio_i2s_pcm_hw_free(struct snd_soc_component *component,
				struct snd_pcm_substream *substream)
{
	struct pio_i2s_dev *piod = snd_soc_component_get_drvdata(component);
	uint16_t sm_mask = 0;
	int i;

	for (i = 0; i < piod->num_sms; i++) {
		piod->sms[i].running = false;
		sm_mask |= BIT(piod->sms[i].sm_index);
	}

	pio_sm_set_enabled(piod->pio, sm_mask, false);

	/* Wait for pending DMA work to complete */
	flush_workqueue(piod->wq);

	return 0;
}

static int pio_i2s_pcm_prepare(struct snd_soc_component *component,
				struct snd_pcm_substream *substream)
{
	struct pio_i2s_dev *piod = snd_soc_component_get_drvdata(component);
	int i;

	/* Store the ALSA DMA buffer address for each SM */
	for (i = 0; i < piod->num_sms; i++) {
		struct pio_i2s_sm_state *sm = &piod->sms[i];

		/*
		 * For 2ch (1 SM): the full ALSA buffer is ours.
		 * For >2ch: each SM gets a contiguous slice.
		 * TODO: proper interleaving for multi-SM.
		 */
		sm->dma_area = (unsigned char *)substream->runtime->dma_area +
			       (i * sm->buf_bytes);
		sm->hw_ptr = 0;
		sm->dma_started = false;

		pio_sm_clear_fifos(piod->pio, sm->sm_index);
	}

	return 0;
}

static int pio_i2s_pcm_trigger(struct snd_soc_component *component,
				struct snd_pcm_substream *substream, int cmd)
{
	struct pio_i2s_dev *piod = snd_soc_component_get_drvdata(component);
	int i;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		for (i = 0; i < piod->num_sms; i++) {
			piod->sms[i].running = true;
			piod->sms[i].dma_started = false;
			/* Schedule work to start DMA + enable SMs */
			queue_work(piod->wq, &piod->sms[i].dma_work);
		}
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		for (i = 0; i < piod->num_sms; i++)
			piod->sms[i].running = false;
		/* SMs will be disabled in hw_free or next prepare */
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

	if (piod->num_sms > 0)
		return bytes_to_frames(substream->runtime,
				       piod->sms[0].hw_ptr);
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
	.name          = "rp1-pio-i2s",
	.open          = pio_i2s_pcm_open,
	.close         = pio_i2s_pcm_close,
	.hw_params     = pio_i2s_pcm_hw_params,
	.hw_free       = pio_i2s_pcm_hw_free,
	.prepare       = pio_i2s_pcm_prepare,
	.trigger       = pio_i2s_pcm_trigger,
	.pointer       = pio_i2s_pcm_pointer,
	.pcm_construct = pio_i2s_pcm_new,
};

/* --- DAI driver -------------------------------------------------- */

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

	if (of_property_read_u32(dev->of_node, "data-gpio-base", &base_gpio))
		base_gpio = PIO_I2S_DATA_BASE_GPIO;
	if (of_property_read_u32(dev->of_node, "data-gpio-count", &num_pins))
		num_pins = PIO_I2S_MAX_SMS;

	if (num_pins > PIO_I2S_MAX_SMS)
		num_pins = PIO_I2S_MAX_SMS;
	if (base_gpio + num_pins > RP1_PIO_GPIO_COUNT)
		return dev_err_probe(dev, -EINVAL,
				     "GPIO%u-%u exceeds PIO range\n",
				     base_gpio, base_gpio + num_pins - 1);

	piod->data_base_gpio = base_gpio;
	piod->num_data_pins = num_pins;

	/* High-priority workqueue for low-latency DMA chaining */
	piod->wq = alloc_workqueue("pio-i2s", WQ_HIGHPRI | WQ_UNBOUND, 0);
	if (!piod->wq)
		return -ENOMEM;

	piod->pio = pio_open();
	if (IS_ERR(piod->pio)) {
		ret = dev_err_probe(dev, PTR_ERR(piod->pio),
				    "failed to open PIO\n");
		goto err_wq;
	}

	for (i = 0; i < num_pins; i++) {
		int sm = pio_claim_unused_sm(piod->pio, false);

		if (sm < 0) {
			dev_err(dev, "not enough free PIO state machines\n");
			ret = -EBUSY;
			goto err_pio;
		}

		piod->sms[i].sm_index = sm;
		piod->sms[i].gpio = base_gpio + i;
		piod->sms[i].piod = piod;
		INIT_WORK(&piod->sms[i].dma_work, pio_i2s_dma_work);
	}

	pio_i2s_dai_driver.capture.channels_max =
		num_pins * PIO_I2S_CHANNELS_PER_SM;
	pio_i2s_dai_driver.playback.channels_max =
		num_pins * PIO_I2S_CHANNELS_PER_SM;

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
err_wq:
	destroy_workqueue(piod->wq);
	return ret;
}

static void rp1_pio_i2s_remove(struct platform_device *pdev)
{
	struct pio_i2s_dev *piod = platform_get_drvdata(pdev);
	uint16_t sm_mask = 0;
	int i;

	for (i = 0; i < piod->num_data_pins; i++) {
		piod->sms[i].running = false;
		sm_mask |= BIT(piod->sms[i].sm_index);
	}

	pio_sm_set_enabled(piod->pio, sm_mask, false);
	flush_workqueue(piod->wq);
	destroy_workqueue(piod->wq);

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
