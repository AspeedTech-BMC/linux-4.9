/*
 * Crypto driver for the Aspeed SoC G6
 *
 * Copyright (C) ASPEED Technology Inc.
 * Ryan Chen <ryan_chen@aspeedtech.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/reset.h>
#include <linux/clk.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include "aspeed-acry.h"

// #define ASPEED_ACRY_DEBUG

#ifdef ASPEED_ACRY_DEBUG
//#define ACRY_DBUG(fmt, args...) printk(KERN_DEBUG "%s() " fmt, __FUNCTION__, ## args)
#define ACRY_DBUG(fmt, args...) printk("%s() " fmt, __FUNCTION__, ## args)
#else
#define ACRY_DBUG(fmt, args...)
#endif

int exp_dw_mapping[512];
int mod_dw_mapping[512];
int data_byte_mapping[2048];

static irqreturn_t aspeed_acry_irq(int irq, void *dev)
{
	struct aspeed_acry_dev *acry_dev = (struct aspeed_acry_dev *)dev;
	u32 sts = aspeed_acry_read(acry_dev, ASPEED_ACRY_STATUS);
	int handle = IRQ_NONE;

	ACRY_DBUG("aspeed_crypto_irq sts %x \n", sts);
	aspeed_acry_write(acry_dev, sts, ASPEED_ACRY_STATUS);

	if (sts & ACRY_RSA_ISR) {
		if (acry_dev->flags & CRYPTO_FLAGS_BUSY)
			tasklet_schedule(&acry_dev->done_task);
		else
			dev_warn(acry_dev->dev, "CRYPTO interrupt when no active requests.\n");
		handle = IRQ_HANDLED;
	}

	return handle;
}

int aspeed_acry_handle_queue(struct aspeed_acry_dev *acry_dev,
			     struct crypto_async_request *new_areq)
{
	struct crypto_async_request *areq, *backlog;
	unsigned long flags;
	int ret = 0;

	ACRY_DBUG("\n");
	spin_lock_irqsave(&acry_dev->lock, flags);
	if (new_areq)
		ret = crypto_enqueue_request(&acry_dev->queue, new_areq);
	if (acry_dev->flags & CRYPTO_FLAGS_BUSY) {
		spin_unlock_irqrestore(&acry_dev->lock, flags);
		return ret;
	}
	backlog = crypto_get_backlog(&acry_dev->queue);
	areq = crypto_dequeue_request(&acry_dev->queue);
	if (areq)
		acry_dev->flags |= CRYPTO_FLAGS_BUSY;
	spin_unlock_irqrestore(&acry_dev->lock, flags);

	if (!areq)
		return ret;

	if (backlog)
		backlog->complete(backlog, -EINPROGRESS);

	acry_dev->akcipher_req = container_of(areq, struct akcipher_request, base);

	ret = aspeed_acry_rsa_trigger(acry_dev);

	return ret;
}
static void aspeed_acry_sram_mapping(void)
{
	int i, j;

	j = 0;
	for (i = 0; i < (ASPEED_ACRY_RSA_MAX_LEN / BYTES_PER_DWORD); i++) {
		exp_dw_mapping[i] = j;
		mod_dw_mapping[i] = j + 4;
		// data_dw_mapping[i] = j + 8;
		data_byte_mapping[(i * 4)] = (j + 8) * 4;
		data_byte_mapping[(i * 4) + 1] = (j + 8) * 4 + 1;
		data_byte_mapping[(i * 4) + 2] = (j + 8) * 4 + 2;
		data_byte_mapping[(i * 4) + 3] = (j + 8) * 4 + 3;
		j++;
		j = j % 4 ? j : j + 8;
	}
	// printk("aspeed_acry_sram_mapping\n");
	// printk("exp_dw_mapping\n");
	// for (i = 0; i < ASPEED_ACRY_RSA_MAX_LEN / BYTES_PER_DWORD; i++) {
	// 	if (i % 0x10 == 0)
	// 		printk(KERN_CONT "%05x: ", i);
	// 	printk(KERN_CONT "%02x ", exp_dw_mapping[i]);
	// 	if ((i - 0xf) % 0x10 == 0)
	// 		printk(KERN_CONT "\n");
	// }
	// printk(KERN_CONT "\n");
}

static void aspeed_acry_done_task(unsigned long data)
{
	struct aspeed_acry_dev *acry_dev = (struct aspeed_acry_dev *)data;

	ACRY_DBUG("\n");

	(void)acry_dev->resume(acry_dev);
}

static int aspeed_acry_register(struct aspeed_acry_dev *acry_dev)
{
	aspeed_register_acry_rsa_algs(acry_dev);
	return 0;
}

static void aspeed_acry_unregister(void)
{
	return;
}

static const struct of_device_id aspeed_acry_of_matches[] = {
	{ .compatible = "aspeed,ast2600-acry", .data = (void *) 6,},
	{},
};

static int aspeed_acry_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct aspeed_acry_dev *acry_dev;
	const struct of_device_id *acry_dev_id;
	int err;

	acry_dev = devm_kzalloc(&pdev->dev, sizeof(struct aspeed_acry_dev), GFP_KERNEL);
	if (!acry_dev) {
		dev_err(dev, "unable to alloc data struct.\n");
		return -ENOMEM;
	}

	acry_dev_id = of_match_device(aspeed_acry_of_matches, &pdev->dev);
	if (!acry_dev_id)
		return -EINVAL;

	acry_dev->dev = dev;
	acry_dev->version = (unsigned long)acry_dev_id->data;

	platform_set_drvdata(pdev, acry_dev);
	spin_lock_init(&acry_dev->lock);
	tasklet_init(&acry_dev->done_task, aspeed_acry_done_task, (unsigned long)acry_dev);
	crypto_init_queue(&acry_dev->queue, 50);

	acry_dev->regs = of_iomap(pdev->dev.of_node, 0);
	if (!(acry_dev->regs)) {
		dev_err(dev, "can't ioremap\n");
		return -ENOMEM;
	}

	// acry_dev->irq = platform_get_irq(pdev, 0);
	// if (!acry_dev->irq) {
	// 	dev_err(&pdev->dev, "no memory/irq resource for acry_dev\n");
	// 	return -ENXIO;
	// }

	// if (devm_request_irq(&pdev->dev, acry_dev->irq, aspeed_acry_irq, 0, dev_name(&pdev->dev), acry_dev)) {
	// 	dev_err(dev, "unable to request aes irq.\n");
	// 	return -EBUSY;
	// }

	acry_dev->rsaclk = devm_clk_get(&pdev->dev, "rsaclk");
	if (IS_ERR(acry_dev->rsaclk)) {
		dev_err(&pdev->dev, "no rsaclk clock defined\n");
		return -ENODEV;
	}
	clk_prepare_enable(acry_dev->rsaclk);


	acry_dev->acry_sram = of_iomap(pdev->dev.of_node, 1);
	if (!(acry_dev->acry_sram)) {
		dev_err(dev, "can't rsa ioremap\n");
		return -ENOMEM;
	}

	aspeed_acry_sram_mapping();

	err = aspeed_acry_register(acry_dev);
	if (err) {
		dev_err(dev, "err in register alg");
		return err;
	}


	printk("ASPEED RSA Accelerator successfully registered \n");

	return 0;
}

static int aspeed_acry_remove(struct platform_device *pdev)
{
	return 0;
}

#ifdef CONFIG_PM
static int aspeed_acry_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);

	/*
	 * We only support standby mode. All we have to do is gate the clock to
	 * the spacc. The hardware will preserve state until we turn it back
	 * on again.
	 */

	return 0;
}

static int aspeed_acry_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	return 0;
}

#endif /* CONFIG_PM */

MODULE_DEVICE_TABLE(of, aspeed_acry_of_matches);

static struct platform_driver aspeed_acry_driver = {
	.probe 		= aspeed_acry_probe,
	.remove		= aspeed_acry_remove,
#ifdef CONFIG_PM
	.suspend	= aspeed_acry_suspend,
	.resume 	= aspeed_acry_resume,
#endif
	.driver         = {
		.name   = KBUILD_MODNAME,
		.of_match_table = aspeed_acry_of_matches,
	},
};

module_platform_driver(aspeed_acry_driver);

MODULE_AUTHOR("Ryan Chen <ryan_chen@aspeedtech.com>");
MODULE_DESCRIPTION("ASPEED Acry driver");
MODULE_LICENSE("GPL2");
