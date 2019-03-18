// SPDX-License-Identifier: GPL-2.0
/*
 * ASPEED Secure Digital Host Controller Interface.
 * Copyright (C) ASPEED Technology Inc.
 * Ryan Chen <ryan_chen@aspeedtech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 */

#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/mmc/sdhci-aspeed-data.h>
#include <linux/reset.h>
#include "sdhci-pltfm.h"

#include <linux/gpio.h>
#include <dt-bindings/gpio/aspeed-gpio.h>

static void sdhci_aspeed_set_clock(struct sdhci_host *host, unsigned int clock)
{
	int div;
	u16 clk;
	unsigned long timeout;

	if (clock == host->clock)
		return;

	sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);

	if (clock == 0)
		goto out;

	for (div = 1; div < 256; div *= 2) {
		if ((host->max_clk / div) <= clock)
			break;
	}
	div >>= 1;

	//Issue : For ast2300, ast2400 couldn't set div = 0 means /1 , so set source is ~50Mhz up 
	
	clk = div << SDHCI_DIVIDER_SHIFT;
	clk |= SDHCI_CLOCK_INT_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	/* Wait max 20 ms */
	timeout = 20;
	while (!((clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL))
		 & SDHCI_CLOCK_INT_STABLE)) {
		if (timeout == 0) {
			pr_err("%s: Internal clock never stabilised.\n",
			       mmc_hostname(host->mmc));
//			sdhci_dumpregs(host);
			return;
		}
		timeout--;
		mdelay(1);
	}

	clk |= SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

out:
	host->clock = clock;
}

static void sdhci_aspeed_set_bus_width(struct sdhci_host *host, int width)
{
	struct sdhci_pltfm_host *pltfm_priv = sdhci_priv(host);
	struct aspeed_sdhci_irq *sdhci_irq = sdhci_pltfm_priv(pltfm_priv);

	u8 ctrl = sdhci_readb(host, SDHCI_HOST_CONTROL);

	if (sdhci_irq->regs) {
		if (width == MMC_BUS_WIDTH_8)
			aspeed_sdhci_set_8bit_mode(sdhci_irq, 1);
		else
			aspeed_sdhci_set_8bit_mode(sdhci_irq, 0);
	}
	if (width == MMC_BUS_WIDTH_4)
		ctrl |= SDHCI_CTRL_4BITBUS;
	else
		ctrl &= ~SDHCI_CTRL_4BITBUS;

	sdhci_writeb(host, ctrl, SDHCI_HOST_CONTROL);

}

void sdhci_aspeed_reset(struct sdhci_host *host, u8 mask)
{
	struct sdhci_pltfm_host *pltfm_priv = sdhci_priv(host);
	struct aspeed_sdhci_irq *sdhci_irq = sdhci_pltfm_priv(pltfm_priv);
	unsigned long timeout;

	printk("sdhci_aspeed_reset \n");
	sdhci_writeb(host, mask, SDHCI_SOFTWARE_RESET);

	if (mask & SDHCI_RESET_ALL) {
		//steven:turn off bus power here
		if (sdhci_irq->pwr_ctrl_gpio >= 0)
			gpio_set_value(sdhci_irq->pwr_ctrl_gpio, 0);
		host->pwr = 0;
		host->clock = 0;
	}

	/* Wait max 100 ms */
	timeout = 100;

	/* hw clears the bit when it's done */
	while (sdhci_readb(host, SDHCI_SOFTWARE_RESET) & mask) {
		if (timeout == 0) {
			pr_err("%s: Reset 0x%x never completed.\n",
				mmc_hostname(host->mmc), (int)mask);
			return;
		}
		timeout--;
		mdelay(1);
	}
}

static void sdhci_aspeed_set_power(struct sdhci_host *host, unsigned char mode,
			   unsigned short vdd)
{
	struct sdhci_pltfm_host *pltfm_priv = sdhci_priv(host);
	struct aspeed_sdhci_irq *sdhci_irq = sdhci_pltfm_priv(pltfm_priv);
	u8 pwr = 0;

	printk("sdhci_aspeed_set_power \n");
	if (mode != MMC_POWER_OFF) {
		switch (1 << vdd) {
		case MMC_VDD_165_195:
			pwr = SDHCI_POWER_180;
			break;
		case MMC_VDD_29_30:
		case MMC_VDD_30_31:
			pwr = SDHCI_POWER_300;
			break;
		case MMC_VDD_32_33:
		case MMC_VDD_33_34:
			pwr = SDHCI_POWER_330;
			break;
		default:
			WARN(1, "%s: Invalid vdd %#x\n",
			     mmc_hostname(host->mmc), vdd);
			break;
		}
	}

	if (host->pwr == pwr)
		return;

	host->pwr = pwr;

	if (pwr == 0) {
		//steven:turn off bus power here
		if (sdhci_irq->pwr_ctrl_gpio >= 0)
			gpio_set_value(sdhci_irq->pwr_ctrl_gpio, 0);
		sdhci_writeb(host, 0, SDHCI_POWER_CONTROL);
	} else {
		/*
		 * Spec says that we should clear the power reg before setting
		 * a new value. Some controllers don't seem to like this though.
		 */
		if (!(host->quirks & SDHCI_QUIRK_SINGLE_POWER_WRITE))
			sdhci_writeb(host, 0, SDHCI_POWER_CONTROL);

		/*
		 * At least the Marvell CaFe chip gets confused if we set the
		 * voltage and set turn on power at the same time, so set the
		 * voltage first.
		 */
		if (host->quirks & SDHCI_QUIRK_NO_SIMULT_VDD_AND_POWER)
			sdhci_writeb(host, pwr, SDHCI_POWER_CONTROL);

		pwr |= SDHCI_POWER_ON;

		sdhci_writeb(host, pwr, SDHCI_POWER_CONTROL);
		//steven:turn on bus power here
		if (sdhci_irq->pwr_ctrl_gpio >= 0)
			gpio_set_value(sdhci_irq->pwr_ctrl_gpio, 1);

		/*
		 * Some controllers need an extra 10ms delay of 10ms before
		 * they can apply clock after applying power
		 */
		if (host->quirks & SDHCI_QUIRK_DELAY_AFTER_POWER)
			mdelay(10);
	}
}

/*
	AST2300/AST2400 : SDMA/PIO
	AST2500 : ADMA/SDMA/PIO
*/
static struct sdhci_ops  sdhci_aspeed_ops= {
#ifdef CONFIG_MACH_ASPEED_G6
	.set_clock = sdhci_set_clock,
#else
	.set_clock = sdhci_aspeed_set_clock,
#endif	
	.get_max_clock = sdhci_pltfm_clk_get_max_clock,
	.set_bus_width = sdhci_aspeed_set_bus_width,
	.get_timeout_clock = sdhci_pltfm_clk_get_max_clock,
	.reset = sdhci_reset,
	.set_uhs_signaling = sdhci_set_uhs_signaling,
};

static struct sdhci_pltfm_data sdhci_aspeed_pdata = {
	.ops = &sdhci_aspeed_ops,
	.quirks = SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_CLOCK_DIV_ZERO_BROKEN,
};

static int sdhci_aspeed_probe(struct platform_device *pdev)
{
	struct sdhci_host *host;
	struct device_node *pnode;
	struct device_node *np = pdev->dev.of_node;
	struct sdhci_pltfm_host *pltfm_host;
	struct aspeed_sdhci_irq *sdhci_irq;

	int ret;

	host = sdhci_pltfm_init(pdev, &sdhci_aspeed_pdata, sizeof(struct aspeed_sdhci_irq));
	if (IS_ERR(host))
		return PTR_ERR(host);

	pltfm_host = sdhci_priv(host);
	sdhci_irq = sdhci_pltfm_priv(pltfm_host);

	sdhci_get_of_property(pdev);

	if (of_property_read_u32(np, "pwr_ctrl_gpio", &sdhci_irq->pwr_ctrl_gpio) < 0) {
		printk("no pwe ctrl gpio \n");
	} else {
		printk("gpio power ctrl gpio %d \n", sdhci_irq->pwr_ctrl_gpio);
		sdhci_aspeed_ops.set_power = sdhci_aspeed_set_power;
		sdhci_aspeed_ops.reset = sdhci_aspeed_reset;
		if (gpio_request(sdhci_irq->pwr_ctrl_gpio, "pwr")) {
			printk("GPIO request failure: GPIO Pwr\n");
		}
//		gpio_direction_output(sdhci_irq->pwr_ctrl_gpio, 1);
	}

	pltfm_host->clk = devm_clk_get(&pdev->dev, NULL);

	pnode = of_parse_phandle(np, "interrupt-parent", 0);
	if (pnode)
		memcpy(sdhci_irq, pnode->data, sizeof(struct aspeed_sdhci_irq));

	ret = mmc_of_parse(host->mmc);
	if (ret)
		goto err_sdhci_add;

	ret = sdhci_add_host(host);
	if (ret)
		goto err_sdhci_add;

	return 0;

err_sdhci_add:
	sdhci_pltfm_free(pdev);
	return ret;
}

static int sdhci_aspeed_remove(struct platform_device *pdev)
{
	struct sdhci_host	*host = platform_get_drvdata(pdev);
	int dead = (readl(host->ioaddr + SDHCI_INT_STATUS) == 0xffffffff);

	sdhci_remove_host(host, dead);
	sdhci_pltfm_free(pdev);
	return 0;
}

static const struct of_device_id sdhci_aspeed_of_match[] = {
	{ .compatible = "aspeed,sdhci-ast2400", .data = &sdhci_aspeed_pdata },
	{ .compatible = "aspeed,sdhci-ast2500", .data = &sdhci_aspeed_pdata },
	{ .compatible = "aspeed,sdhci-ast2600", .data = &sdhci_aspeed_pdata },	
	{ .compatible = "aspeed,emmc-ast2600", .data = &sdhci_aspeed_pdata },	
	{}
};

MODULE_DEVICE_TABLE(of, sdhci_aspeed_of_match);

static struct platform_driver sdhci_aspeed_driver = {
	.driver		= {
		.name	= "sdhci-aspeed",
		.pm	= &sdhci_pltfm_pmops,
		.of_match_table = sdhci_aspeed_of_match,
	},
	.probe		= sdhci_aspeed_probe,
	.remove		= sdhci_aspeed_remove,
};

module_platform_driver(sdhci_aspeed_driver);

MODULE_DESCRIPTION("Driver for the ASPEED SDHCI Controller");
MODULE_AUTHOR("Ryan Chen <ryan_chen@aspeedtech.com>");
MODULE_LICENSE("GPL v2");
