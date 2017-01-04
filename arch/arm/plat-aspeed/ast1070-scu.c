/********************************************************************************
* File Name     : arch/arm/mach-aspeed/ast1070-scu.c 
* Author         : Ryan Chen
* Description   : AST1070 SCU 
* 
* Copyright (C) 2012-2020  ASPEED Technology Inc.
* This program is free software; you can redistribute it and/or modify 
* it under the terms of the GNU General Public License as published by the Free Software Foundation; 
* either version 2 of the License, or (at your option) any later version. 
* This program is distributed in the hope that it will be useful,  but WITHOUT ANY WARRANTY; 
* without even the implied warranty of MERCHANTABILITY or 
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details. 
* You should have received a copy of the GNU General Public License 
* along with this program; if not, write to the Free Software 
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA 

*   History      : 
*    1. 2013/05/15 Ryan Chen Create
* 
********************************************************************************/
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>

#include <mach/platform.h>
#include <asm/io.h>

#include <mach/hardware.h>

#include <plat/ast1070-scu.h>
#include <plat/regs-ast1070-scu.h>

#define CONFIG_AST1070_SCU_LOCK
//#define AST1070_SCU_DEBUG

#ifdef AST1070_SCU_DEBUG
#define SCUDBUG(fmt, args...) printk("%s() " fmt, __FUNCTION__, ## args)
#else
#define SCUDBUG(fmt, args...)
#endif

#define SCUMSG(fmt, args...) printk(fmt, ## args)

static u32 ast1070_scu_base[MAX_AST1070_NR];

static inline u32 
ast1070_scu_read(u8 chip, u32 reg)
{
	u32 val;
		
	val = readl(ast1070_scu_base[chip] + reg);
	
	SCUDBUG("ast1070_scu_read-%d : reg = 0x%08x, val = 0x%08x\n",chip, reg, val);
	
	return val;
}

static inline void
ast1070_scu_write(u8 chip, u32 val, u32 reg) 
{
	SCUDBUG("ast1070_scu_write-%d : reg = 0x%08x, val = 0x%08x\n",chip, reg, val);
#ifdef CONFIG_AST1070_SCU_LOCK
	//unlock 
	writel(AST1070_SCU_PROTECT_UNLOCK, ast1070_scu_base[chip]);
	writel(val, ast1070_scu_base[chip] + reg);
	//lock
//	writel(0xaa,ast1070_scu_base[chip]);	
#else
	writel(val, ast1070_scu_base[chip] + reg);
#endif
}

extern void
ast1070_scu_init_uart(u8 chip)
{
	//SCU UART Reset
	ast1070_scu_write(chip, ast1070_scu_read(chip, AST1070_SCU_RESET) & 
			~(SCU_RESET_N1_UART | SCU_RESET_N2_UART | 
			SCU_RESET_N3_UART | SCU_RESET_N4_UART), 
			AST1070_SCU_RESET);
}

EXPORT_SYMBOL(ast1070_scu_init_uart);

extern void
ast1070_scu_init_i2c(u8 chip)
{
	//SCU I2C Reset 
	ast1070_scu_write(chip, ast1070_scu_read(chip, AST1070_SCU_RESET) & ~SCU_RESET_I2C, AST1070_SCU_RESET);
}

EXPORT_SYMBOL(ast1070_scu_init_i2c);

extern void
ast1070_scu_dma_init(u8 chip)
{
	u32 val =0;

	//let the uart_dma engine leave the reset state
	ast1070_scu_write(chip, ast1070_scu_read(chip, AST1070_SCU_RESET) & ~SCU_RESET_DMA, AST1070_SCU_RESET);

	val = ast1070_scu_read(chip, AST1070_SCU_MISC_CTRL) & ~SCU_DMA_M_S_MASK;

	if(ast1070_scu_read(chip, AST1070_SCU_TRAP) & TRAP_MULTI_MASTER) {
		//AST1070 multi Initial DMA
		if(ast1070_scu_read(chip, AST1070_SCU_TRAP) & TRAP_DEVICE_SLAVE)
			ast1070_scu_write(chip, val | SCU_DMA_SLAVE_EN, AST1070_SCU_MISC_CTRL);			
		else
			//Enable DMA master
			ast1070_scu_write(chip, val | SCU_DMA_MASTER_EN, AST1070_SCU_MISC_CTRL);

	} else {
		//AST1070 single 
		ast1070_scu_write(chip, val, AST1070_SCU_MISC_CTRL);
	}

}
EXPORT_SYMBOL(ast1070_scu_dma_init);


extern void
ast1070_scu_init_lpc(void)
{

}

EXPORT_SYMBOL(ast1070_scu_init_lpc);

//***********************************Multi-function pin control***********************************

extern void
ast1070_multi_func_uart(u8 chip, u8 uart)
{
	//Noticed : Only UART 3/4 can pin out 
	ast1070_scu_write(chip, (ast1070_scu_read(chip, AST1070_SCU_UART_MUX) &
				~UART_MUX_MASK(uart)) |
				SET_UART_IO_PAD(uart,PAD_FROM_BMC) | 
				SET_NODE_UART_CTRL(uart, NODE_UART_FROM_NONE) | 
				SET_BMC_UART_CTRL(uart, BMC_UART_FROM_PAD1),
				AST1070_SCU_UART_MUX); 

}

EXPORT_SYMBOL(ast1070_multi_func_uart);


//***********************************CLK control***********************************


//***********************************CLK Information***********************************
extern u32
ast1070_get_clk_source(void)
{

}
EXPORT_SYMBOL(ast1070_get_clk_source);

//***********************************Information ***********************************
extern u32
ast1070_revision_id_info(u8 chip)
{
	return ast1070_scu_read(chip, AST1070_SCU_CHIP_ID);	
}	

EXPORT_SYMBOL(ast1070_revision_id_info);

extern void
ast1070_scu_init(u8 chip,u32 lpc_base)
{
	switch(chip) {
		case 0:
			ast1070_scu_base[0] = (u32)ioremap(AST1070_C0_SCU_BASE, SZ_256);
			break;
		case 1:
			ast1070_scu_base[1] = (u32)ioremap(AST1070_C1_SCU_BASE, SZ_256);
			break;
	}	
}	
EXPORT_SYMBOL(ast1070_scu_init);
