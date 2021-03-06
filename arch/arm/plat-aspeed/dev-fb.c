/********************************************************************************
* File Name     : linux/arch/arm/plat-aspeed/dev-fb.c
* Author        : Ryan Chen
* Description   : ASPEED Frambuffer Driver
*
* Copyright (C) ASPEED Tech. Inc.
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by the Free Software Foundation;
* either version 2 of the License, or (at your option) any later version.
* 
* This program is distributed in the hope that it will be useful,  but WITHOUT ANY WARRANTY;
* without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
* 
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

*   History      :
*    1. 2012/12/15 Ryan Chen initial
*
********************************************************************************/
#include <linux/kernel.h>
#include <linux/platform_device.h>

#include <mach/irqs.h>
#include <plat/aspeed.h>

#include <mach/platform.h>

#include <plat/devs.h>
#include <plat/ast-scu.h>

#include <mach/ast_lcd.h>


/* --------------------------------------------------------------------
 *  ASPEED FB
 * -------------------------------------------------------------------- */

#if defined(CONFIG_FB_AST) || defined(CONFIG_FB_AST_MODULE)
static u64 ast_device_fb_dmamask = 0xffffffffUL;

static struct ast_fb_plat_data fb_plat_data = {
#ifdef AST_SOC_G5
#ifdef CONFIG_ARCH_AST3200
	.set_pll = ast_set_d2_pll_clk,
#else	
	//AST2500 force d2 pll for support 800x600
	.set_pll = NULL,
#endif
#endif	
	.disp_dev_no = 0,
};

static struct resource ast_fb0_resources[] = {
	[0] = {
		.start = AST_CRT0_BASE,
		.end = AST_CRT0_BASE + SZ_256 - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_CRT,
		.end = IRQ_CRT,
		.flags = IORESOURCE_IRQ,
	},
	[2] = {
		.start = AST_CRT0_MEM_BASE,
		.end = AST_CRT0_MEM_BASE + AST_CRT0_MEM_SIZE - 1,
		.flags = IORESOURCE_DMA,
	},
#ifdef AST_CURSOR0_MEM_BASE	
	[3] = {
		.start = AST_CURSOR0_MEM_BASE,
		.end = AST_CURSOR0_MEM_BASE + AST_CURSOR0_MEM_SIZE -1,
		.flags = IORESOURCE_DMA,
	},
#endif
};




void __init ast_add_device_fb(void)
{


#ifdef AST_SOC_G5	
	fb_plat_data.clock_src = ast_get_clk_source();
#endif

	platform_device_register(&ast_fb0_device);
#ifdef AST_CRT1_BASE	
	platform_device_register(&ast_fb1_device);
#endif
#ifdef AST_CRT2_BASE	
	platform_device_register(&ast_fb2_device);
#endif
#ifdef AST_CRT3_BASE	
	platform_device_register(&ast_fb3_device);
#endif
}
#else
void __init ast_add_device_fb(void) {}
#endif
