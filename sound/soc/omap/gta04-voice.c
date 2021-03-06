/*
 * Copyright (C) 2011 John Ogness
 *   Author: John Ogness <john.ogness@linutronix.de>
 *
 * based on sound/soc/omap/omap3beagle.c by
 *   Steve Sakoman <steve@sakoman.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any kind,
 * whether express or implied; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 */

#include <linux/platform_device.h>
#include <linux/module.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

#include "omap-mcbsp.h"
#include "../codecs/gtm601.h"

static int gta04_voice_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	/* setup codec dai and cpu dai hardware params */
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
//	struct snd_soc_dai *codec_dai = rtd->dai->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	unsigned int fmt;
	int ret;
	
	fmt =	SND_SOC_DAIFMT_I2S |	// I2S
		//	SND_SOC_DAIFMT_GATED |	// try to power down if not needed
			SND_SOC_DAIFMT_IB_IF |	// positive sync pulse, driven on rising, sampled on falling clock
			SND_SOC_DAIFMT_CBM_CFM;	// clocks come from GSM modem
	
#if 0	// set clocks to be outputs

	fmt =	SND_SOC_DAIFMT_I2S |
			SND_SOC_DAIFMT_IB_IF |
			SND_SOC_DAIFMT_CBS_CFS;

#endif	
	/* Set cpu DAI configuration */
	ret = snd_soc_dai_set_fmt(cpu_dai, fmt);
	if (ret < 0) {
		printk(KERN_ERR "can't set cpu DAI configuration\n");
		return ret;
	}
	
	if (ret < 0) {
		printk(KERN_ERR "can't set cpu system clock\n");
		return ret;
	}	
		
	return 0;
}

static int gta04_voice_init(struct snd_soc_pcm_runtime *runtime)
{
	/* add controls */
	/* add routes */
	/* setup pins */
	struct snd_soc_codec *codec = runtime->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	snd_soc_dapm_sync(dapm);
	return 0;
}

static int gta04_voice_startup(struct snd_pcm_substream *substream)
{
	/* enable clock used by codec */
	/* clock is provided by the GTM601 */
	return 0;
}

static void gta04_voice_shutdown(struct snd_pcm_substream *substream)
{
	/* disable clock used by codec */
	/* clock is provided by the GTM601 */
}

static struct snd_soc_ops gta04_voice_ops = {
	.startup	= gta04_voice_startup,
	.hw_params	= gta04_voice_hw_params,
	.shutdown	= gta04_voice_shutdown,
};

/* digital voice interface glue - connects codec <--> cpu */
static struct snd_soc_dai_link gta04_voice_dai = {
	.name 		= "GTM601",
	.stream_name 	= "GTM601",
	.cpu_dai_name	= "omap-mcbsp.4",
	.platform_name	= "omap-pcm-audio",
	.codec_dai_name = "GTM601",
	.codec_name	= "gtm601_codec_audio",
	.init		= gta04_voice_init,
	.ops 		= &gta04_voice_ops,
};

/* voice machine driver */
static struct snd_soc_card gta04_voice_card = {
	.name		= "gta04-voice",
	.dai_link	= &gta04_voice_dai,
	.num_links	= 1,
};

/* voice subsystem */
/*static struct snd_soc_device gta04_voice_devdata = {
	.card		= &gta04_voice_card,
	.codec_dev	= &soc_codec_dev_gtm601,
};*/

static struct platform_device *gta04_voice_snd_device;

static int __init gta04_voice_soc_init(void)
{
	struct device *dev;
	int ret;

	pr_info("gta04-voice SoC init\n");

	gta04_voice_snd_device = platform_device_alloc("soc-audio", 1);
	if (!gta04_voice_snd_device) {
		printk(KERN_ERR "platform device allocation failed\n");
		return -ENOMEM;
	}

	dev = &gta04_voice_snd_device->dev;

	platform_set_drvdata(gta04_voice_snd_device, &gta04_voice_card);

	ret = platform_device_add(gta04_voice_snd_device);
	if (ret) {
		printk(KERN_ERR "unable to add platform device\n");
		platform_device_put(gta04_voice_snd_device);
	}

	return ret;
}

static void __exit gta04_voice_soc_exit(void)
{
	platform_device_unregister(gta04_voice_snd_device);
}

module_init(gta04_voice_soc_init);
module_exit(gta04_voice_soc_exit);

MODULE_AUTHOR("John Ogness <john.ogness@linutronix.de>");
MODULE_DESCRIPTION("ALSA SoC GTA04 Voice");
MODULE_LICENSE("GPL v2");
