/*
 * linux/arch/arm/mach-pxa/zylonite.c
 *
 * Support for the PXA3xx Development Platform (aka Zylonite)
 *
 * Copyright (C) 2006 Marvell International Ltd.
 *
 * 2007-09-04: eric miao <eric.miao@marvell.com>
 *             rewrite to align with latest kernel
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/platform_device.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/hardware.h>
#include <asm/arch/gpio.h>
#include <asm/arch/pxafb.h>
#include <asm/arch/zylonite.h>
#include <asm/arch/mmc.h>

#include "generic.h"

#define MAX_SLOTS	3
struct platform_mmc_slot zylonite_mmc_slot[MAX_SLOTS];

int gpio_backlight;
int gpio_eth_irq;

int lcd_id;
int lcd_orientation;

static struct resource smc91x_resources[] = {
	[0] = {
		.start	= ZYLONITE_ETH_PHYS + 0x300,
		.end	= ZYLONITE_ETH_PHYS + 0xfffff,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.start	= -1,	/* for run-time assignment */
		.end	= -1,
		.flags	= IORESOURCE_IRQ | IORESOURCE_IRQ_HIGHEDGE,
	}
};

static struct platform_device smc91x_device = {
	.name		= "smc91x",
	.id		= 0,
	.num_resources	= ARRAY_SIZE(smc91x_resources),
	.resource	= smc91x_resources,
};

#if defined(CONFIG_FB_PXA) || (CONFIG_FB_PXA_MODULES)
static void zylonite_backlight_power(int on)
{
	gpio_set_value(gpio_backlight, on);
}

static struct pxafb_mode_info toshiba_ltm035a776c_mode = {
	.pixclock		= 110000,
	.xres			= 240,
	.yres			= 320,
	.bpp			= 16,
	.hsync_len		= 4,
	.left_margin		= 6,
	.right_margin		= 4,
	.vsync_len		= 2,
	.upper_margin		= 2,
	.lower_margin		= 3,
	.sync			= FB_SYNC_VERT_HIGH_ACT,
};

static struct pxafb_mode_info toshiba_ltm04c380k_mode = {
	.pixclock		= 50000,
	.xres			= 640,
	.yres			= 480,
	.bpp			= 16,
	.hsync_len		= 1,
	.left_margin		= 0x9f,
	.right_margin		= 1,
	.vsync_len		= 44,
	.upper_margin		= 0,
	.lower_margin		= 0,
	.sync			= FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT,
};

static struct pxafb_mach_info zylonite_toshiba_lcd_info = {
	.num_modes      	= 1,
	.lccr0			= LCCR0_Act,
	.lccr3			= LCCR3_PCP,
	.pxafb_backlight_power	= zylonite_backlight_power,
};

static struct pxafb_mode_info sharp_ls037_modes[] = {
	[0] = {
		.pixclock	= 158000,
		.xres		= 240,
		.yres		= 320,
		.bpp		= 16,
		.hsync_len	= 4,
		.left_margin	= 39,
		.right_margin	= 39,
		.vsync_len	= 1,
		.upper_margin	= 2,
		.lower_margin	= 3,
		.sync		= 0,
	},
	[1] = {
		.pixclock	= 39700,
		.xres		= 480,
		.yres		= 640,
		.bpp		= 16,
		.hsync_len	= 8,
		.left_margin	= 81,
		.right_margin	= 81,
		.vsync_len	= 1,
		.upper_margin	= 2,
		.lower_margin	= 7,
		.sync		= 0,
	},
};

static struct pxafb_mach_info zylonite_sharp_lcd_info = {
	.modes			= sharp_ls037_modes,
	.num_modes		= 2,
	.lccr0			= LCCR0_Act,
	.lccr3			= LCCR3_PCP | LCCR3_HSP | LCCR3_VSP,
	.pxafb_backlight_power	= zylonite_backlight_power,
};

static void __init zylonite_init_lcd(void)
{
	/* backlight GPIO: output, default on */
	gpio_direction_output(gpio_backlight, 1);

	if (lcd_id & 0x20) {
		set_pxa_fb_info(&zylonite_sharp_lcd_info);
		return;
	}

	/* legacy LCD panels, it would be handy here if LCD panel type can
	 * be decided at run-time
	 */
	if (1)
		zylonite_toshiba_lcd_info.modes = &toshiba_ltm035a776c_mode;
	else
		zylonite_toshiba_lcd_info.modes = &toshiba_ltm04c380k_mode;

	set_pxa_fb_info(&zylonite_toshiba_lcd_info);
}
#else
static inline void zylonite_init_lcd(void) {}
#endif

#if defined(CONFIG_MMC)
static int zylonite_mci_ro(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);

	return gpio_get_value(zylonite_mmc_slot[pdev->id].gpio_wp);
}

static int zylonite_mci_init(struct device *dev,
			     irq_handler_t zylonite_detect_int,
			     void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	int err, cd_irq, gpio_cd, gpio_wp;

	cd_irq = gpio_to_irq(zylonite_mmc_slot[pdev->id].gpio_cd);
	gpio_cd = zylonite_mmc_slot[pdev->id].gpio_cd;
	gpio_wp = zylonite_mmc_slot[pdev->id].gpio_wp;

	/*
	 * setup GPIO for Zylonite MMC controller
	 */
	err = gpio_request(gpio_cd, "mmc card detect");
	if (err)
		goto err_request_cd;
	gpio_direction_input(gpio_cd);

	err = gpio_request(gpio_wp, "mmc write protect");
	if (err)
		goto err_request_wp;
	gpio_direction_input(gpio_wp);

	err = request_irq(cd_irq, zylonite_detect_int,
			  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			  "MMC card detect", data);
	if (err) {
		printk(KERN_ERR "%s: MMC/SD/SDIO: "
				"can't request card detect IRQ\n", __func__);
		goto err_request_irq;
	}

	return 0;

err_request_irq:
	gpio_free(gpio_wp);
err_request_wp:
	gpio_free(gpio_cd);
err_request_cd:
	return err;
}

static void zylonite_mci_exit(struct device *dev, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	int cd_irq, gpio_cd, gpio_wp;

	cd_irq = gpio_to_irq(zylonite_mmc_slot[pdev->id].gpio_cd);
	gpio_cd = zylonite_mmc_slot[pdev->id].gpio_cd;
	gpio_wp = zylonite_mmc_slot[pdev->id].gpio_wp;

	free_irq(cd_irq, data);
	gpio_free(gpio_cd);
	gpio_free(gpio_wp);
}

static struct pxamci_platform_data zylonite_mci_platform_data = {
	.detect_delay	= 20,
	.ocr_mask	= MMC_VDD_32_33|MMC_VDD_33_34,
	.init 		= zylonite_mci_init,
	.exit		= zylonite_mci_exit,
	.get_ro		= zylonite_mci_ro,
};

static struct pxamci_platform_data zylonite_mci2_platform_data = {
	.detect_delay	= 20,
	.ocr_mask	= MMC_VDD_32_33|MMC_VDD_33_34,
};

static void __init zylonite_init_mmc(void)
{
	pxa_set_mci_info(&zylonite_mci_platform_data);
	pxa3xx_set_mci2_info(&zylonite_mci2_platform_data);
	if (cpu_is_pxa310())
		pxa3xx_set_mci3_info(&zylonite_mci_platform_data);
}
#else
static inline void zylonite_init_mmc(void) {}
#endif

static void __init zylonite_init(void)
{
	/* board-processor specific initialization */
	zylonite_pxa300_init();
	zylonite_pxa320_init();

	/*
	 * Note: We depend that the bootloader set
	 * the correct value to MSC register for SMC91x.
	 */
	smc91x_resources[1].start = gpio_to_irq(gpio_eth_irq);
	smc91x_resources[1].end   = gpio_to_irq(gpio_eth_irq);
	platform_device_register(&smc91x_device);

	zylonite_init_lcd();
	zylonite_init_mmc();
}

MACHINE_START(ZYLONITE, "PXA3xx Platform Development Kit (aka Zylonite)")
	.phys_io	= 0x40000000,
	.boot_params	= 0xa0000100,
	.io_pg_offst	= (io_p2v(0x40000000) >> 18) & 0xfffc,
	.map_io		= pxa_map_io,
	.init_irq	= pxa3xx_init_irq,
	.timer		= &pxa_timer,
	.init_machine	= zylonite_init,
MACHINE_END
