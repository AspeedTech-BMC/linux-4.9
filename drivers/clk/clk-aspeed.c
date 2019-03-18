// SPDX-License-Identifier: GPL-2.0+

#define pr_fmt(fmt) "clk-aspeed: " fmt

#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <dt-bindings/clock/aspeed-clock.h>

#define ASPEED_NUM_CLKS		40

#define ASPEED_RESET2_OFFSET	32

#define ASPEED_RESET_CTRL	0x04
#define ASPEED_CLK_SELECTION	0x08
#define  ASPEED_SDIO_CLK_EN BIT(15)
#define ASPEED_CLK_STOP_CTRL	0x0c
#define ASPEED_MPLL_PARAM	0x20
#define ASPEED_HPLL_PARAM	0x24
#define  AST2500_HPLL_BYPASS_EN	BIT(20)
#define  AST2400_HPLL_PROGRAMMED BIT(18)
#define  AST2400_HPLL_BYPASS_EN	BIT(17)
#define ASPEED_MISC_CTRL	0x2c
#define  UART_DIV13_EN		BIT(12)
#define ASPEED_STRAP		0x70
#define  CLKIN_25MHZ_EN		BIT(23)
#define  AST2400_CLK_SOURCE_SEL	BIT(18)
#define ASPEED_CLK_SELECTION_2	0xd8
#define ASPEED_RESET_CTRL2	0xd4
//**************************************************************************
#define ASPEED_G6_RESET_CTRL		0x40
#define ASPEED_G6_RESET_CTRL2		0x50

#define ASPEED_G6_CLK_SELECTION		0x300
#define ASPEED_G6_CLK_SELECTION2	0x304
#define ASPEED_G6_CLK_SELECTION3	0x310

#define ASPEED_G6_CLK_STOP_CTRL		0x80
#define ASPEED_G6_CLK_STOP_CTRL2	0x90
//**************************************************************************

/* Globally visible clocks */
static DEFINE_SPINLOCK(aspeed_clk_lock);

/* Keeps track of all clocks */
static struct clk_hw_onecell_data *aspeed_clk_data;

static void __iomem *scu_base;

/**
 * struct aspeed_gate_data - Aspeed gated clocks
 * @clock_idx: bit used to gate this clock in the clock register
 * @reset_idx: bit used to reset this IP in the reset register. -1 if no
 *             reset is required when enabling the clock
 * @name: the clock name
 * @parent_name: the name of the parent clock
 * @flags: standard clock framework flags
 */
struct aspeed_gate_data {
//	u32		clk_offset;
	u8		clock_idx;
//	u32		reset_offset;
	s8		reset_idx;
	const char	*name;
	const char	*parent_name;
	unsigned long	flags;
};

/**
 * struct aspeed_clk_gate - Aspeed specific clk_gate structure
 * @hw:		handle between common and hardware-specific interfaces
 * @reg:	register controlling gate
 * @clock_idx:	bit used to gate this clock in the clock register
 * @reset_idx:	bit used to reset this IP in the reset register. -1 if no
 *		reset is required when enabling the clock
 * @flags:	hardware-specific flags
 * @lock:	register lock
 *
 * Some of the clocks in the Aspeed SoC must be put in reset before enabling.
 * This modified version of clk_gate allows an optional reset bit to be
 * specified.
 */
struct aspeed_clk_gate {
	struct clk_hw	hw;
	struct regmap	*map;
	u8		clock_idx;
	s8		reset_idx;
	u8		flags;
	spinlock_t	*lock;
};

#define to_aspeed_clk_gate(_hw) container_of(_hw, struct aspeed_clk_gate, hw)

/* TODO: ask Aspeed about the actual parent data */
static const struct aspeed_gate_data aspeed_gates[] = {
	/*				 clk rst   name			parent	flags */
	[ASPEED_CLK_GATE_ECLK] =	{  0, -1, "eclk-gate",		"eclk",	0 }, /* Video Engine */
	[ASPEED_CLK_GATE_GCLK] =	{  1,  7, "gclk-gate",		NULL,	0 }, /* 2D engine */
	[ASPEED_CLK_GATE_MCLK] =	{  2, -1, "mclk-gate",		"mpll",	CLK_IS_CRITICAL }, /* SDRAM */
	[ASPEED_CLK_GATE_VCLK] =	{  3,  6, "vclk-gate",		NULL,	0 }, /* Video Capture */
	[ASPEED_CLK_GATE_BCLK] =	{  4,  8, "bclk-gate",		"bclk",	CLK_IS_CRITICAL }, /* PCIe/PCI */
	[ASPEED_CLK_GATE_DCLK] =	{  5, -1, "dclk-gate",		NULL,	CLK_IS_CRITICAL }, /* DAC */
	[ASPEED_CLK_GATE_REFCLK] =	{  6, -1, "refclk-gate",	"clkin", CLK_IS_CRITICAL },
	[ASPEED_CLK_GATE_USBPORT2CLK] =	{  7,  3, "usb-port2-gate",	NULL,	0 }, /* USB2.0 Host port 2 */
	[ASPEED_CLK_GATE_LCLK] =	{  8,  5, "lclk-gate",		NULL,	CLK_IS_CRITICAL }, /* LPC */
	[ASPEED_CLK_GATE_USBUHCICLK] =	{  9, 15, "usb-uhci-gate",	NULL,	0 }, /* USB1.1 (requires port 2 enabled) */
	[ASPEED_CLK_GATE_D1CLK] =	{ 10, 13, "d1clk-gate",		NULL,	0 }, /* GFX CRT */
	[ASPEED_CLK_GATE_YCLK] =	{ 13,  4, "yclk-gate",		NULL,	0 }, /* HAC */
	[ASPEED_CLK_GATE_USBPORT1CLK] = { 14, 14, "usb-port1-gate",	NULL,	0 }, /* USB2 hub/USB2 host port 1/USB1.1 dev */
	[ASPEED_CLK_GATE_UART1CLK] =	{ 15, -1, "uart1clk-gate",	"uart",	0 }, /* UART1 */
	[ASPEED_CLK_GATE_UART2CLK] =	{ 16, -1, "uart2clk-gate",	"uart",	0 }, /* UART2 */
	[ASPEED_CLK_GATE_UART5CLK] =	{ 17, -1, "uart5clk-gate",	"uart",	0 }, /* UART5 */
	[ASPEED_CLK_GATE_ESPICLK] =	{ 19, -1, "espiclk-gate",	NULL,	CLK_IS_CRITICAL }, /* eSPI */
	[ASPEED_CLK_GATE_MAC1CLK] =	{ 20, 11, "mac1clk-gate",	"mac",	0 }, /* MAC1 */
	[ASPEED_CLK_GATE_MAC2CLK] =	{ 21, 12, "mac2clk-gate",	"mac",	0 }, /* MAC2 */
	[ASPEED_CLK_GATE_RSACLK] =	{ 24,  4, "rsaclk-gate",	NULL,	0 }, /* RSA */
	[ASPEED_CLK_GATE_UART3CLK] =	{ 25, -1, "uart3clk-gate",	"uart",	0 }, /* UART3 */
	[ASPEED_CLK_GATE_UART4CLK] =	{ 26, -1, "uart4clk-gate",	"uart",	0 }, /* UART4 */
	[ASPEED_CLK_GATE_SDCLK] =	{ 27, 16, "sdclk-gate",		NULL,	0 }, /* SDIO/SD */
	[ASPEED_CLK_GATE_LHCCLK] =	{ 28, -1, "lhclk-gate",		"lhclk", 0 }, /* LPC master/LPC+ */
	[ASPEED_CLK_GATE_SDEXTCLK] = { 29, -1, "sdextclk-gate",		"sdio",	0 }, /* For card clk */
#ifdef CONFIG_MACH_ASPEED_G6	
	[ASPEED_CLK_GATE_EMMCCLK] = { 30, -1, "emmcclk-gate",		NULL,	0 }, /* For card clk */
	[ASPEED_CLK_GATE_EMMCEXTCLK] = { 31, -1, "emmcextclk-gate",		"emmc",	0 }, /* For card clk */
#endif
};

//ast2400
static const char * const ast2400_eclk_parent_names[] = {
	"mpll/2",
	"hpll",
	"inverted mpll/2",
	"inverted hpll",
};

//ast2500
static const char * const eclk_parent_names[] = {
	"mpll",
	"hpll",
	"dpll",
};

static const struct clk_div_table ast2500_eclk_div_table[] = {
	{ 0x0, 2 }, 
	{ 0x1, 2 },
	{ 0x2, 3 },
	{ 0x3, 4 },
	{ 0x4, 5 },
	{ 0x5, 6 },
	{ 0x6, 7 },
	{ 0x7, 8 },
	{ 0 }
};

static const struct clk_div_table ast2400_eclk_div_table[] = {
	{ 0x0, 2 }, 
	{ 0x1, 4 },
	{ 0x2, 6 },
	{ 0x3, 8 },
	{ 0x4, 10 },
	{ 0x5, 12 },
	{ 0x6, 14 },
	{ 0x7, 16 },
	{ 0 }
};

static const struct clk_div_table ast2500_mac_div_table[] = {
	{ 0x0, 4 }, /* Yep, really. Aspeed confirmed this is correct */
	{ 0x1, 4 },
	{ 0x2, 6 },
	{ 0x3, 8 },
	{ 0x4, 10 },
	{ 0x5, 12 },
	{ 0x6, 14 },
	{ 0x7, 16 },
	{ 0 }
};

static const struct clk_div_table ast2400_div_table[] = {
	{ 0x0, 2 },
	{ 0x1, 4 },
	{ 0x2, 6 },
	{ 0x3, 8 },
	{ 0x4, 10 },
	{ 0x5, 12 },
	{ 0x6, 14 },
	{ 0x7, 16 },
	{ 0 }
};

static const struct clk_div_table ast2500_div_table[] = {
	{ 0x0, 4 },
	{ 0x1, 8 },
	{ 0x2, 12 },
	{ 0x3, 16 },
	{ 0x4, 20 },
	{ 0x5, 24 },
	{ 0x6, 28 },
	{ 0x7, 32 },
	{ 0 }
};

static u8 ast2600_resets[] = {
	/* SCU40 resets */
//	[ASPEED_RESET_GRAPHIC] = 26, ???
	[ASPEED_RESET_XDMA]	= 25,
	[ASPEED_RESET_MCTP]	= 24,
	[ASPEED_RESET_P2X]	= 24,	
//	reserved bit 23	
	[ASPEED_RESET_JTAG_MASTER] = 22,
//	reserved bit 21
//	reserved bit 20	
//	reserved bit 19	
	[ASPEED_RESET_MIC]	= 18,
//	reserved bit 17
	[ASPEED_RESET_SDHCI]	= ASPEED_RESET2_OFFSET + 16,
	[ASPEED_RESET_UHCI]	= 15,
	[ASPEED_RESET_EHCI_P1]	= 14,
	[ASPEED_RESET_CRT]		= 13,
	[ASPEED_RESET_MAC2]	= 12,
	[ASPEED_RESET_MAC1]	= 11,
//	reserved bit 10	
//	reserved bit 9 
//	[ASPEED_RESET_PCI_VGA]	= 8,	???
	[ASPEED_RESET_2D]	= 7,
	[ASPEED_RESET_VIDEO]	= 6,
//	reserved bit 5
	[ASPEED_RESET_HACE]	= 4,
	[ASPEED_RESET_EHCI_P2]	= 3,
//	reserved bit 2
	[ASPEED_RESET_AHB]	=  1,	
	[ASPEED_RESET_SRAM_CTRL]	=  0,

	/*
	 * SCU50 resets start at an offset to separate them from
	 */
//	[ASPEED_RESET_PCIE_DIR] = 21,
//	[ASPEED_RESET_PCIE] 	= 20,
	 
	 
//	[ASPEED_RESET_CRT1]	= ASPEED_RESET2_OFFSET + 5,
	
	[ASPEED_RESET_ADC]	= ASPEED_RESET2_OFFSET + 23,
//	[ASPEED_RESET_JTAG_MASTER2fs] = ASPEED_RESET2_OFFSET + 22,
//	[ASPEED_RESET_MAC4]	= ASPEED_RESET2_OFFSET + 21,
//	[ASPEED_RESET_MAC3]	= ASPEED_RESET2_OFFSET + 20,
//	reserved bit 6	
	[ASPEED_RESET_PWM]	= ASPEED_RESET2_OFFSET + 5,	 
	[ASPEED_RESET_PECI] = ASPEED_RESET2_OFFSET + 4,	
//	[ASPEED_RESET_MII] = ASPEED_RESET2_OFFSET + 3,
	[ASPEED_RESET_I2C]	= ASPEED_RESET2_OFFSET + 2,	
//	reserved bit 1
	[ASPEED_RESET_LPC]	= ASPEED_RESET2_OFFSET + 0,	
	[ASPEED_RESET_ESPI]	= ASPEED_RESET2_OFFSET + 0,
	[ASPEED_RESET_EMMC]	= 16,
};

static u8 aspeed_resets[] = {
	/* SCU04 resets */
	[ASPEED_RESET_ESPI]	= 5,
	[ASPEED_RESET_XDMA]	= 25,
	[ASPEED_RESET_MCTP]	= 24,
	[ASPEED_RESET_P2X]	= 24,	
	[ASPEED_RESET_ADC]	= 23,
	[ASPEED_RESET_JTAG_MASTER] = 22,
	[ASPEED_RESET_PCIE_DIR]	= 21,
	[ASPEED_RESET_PCIE]		= 20,
	[ASPEED_RESET_MIC]	= 18,
	[ASPEED_RESET_SDHCI]	= 16,
	[ASPEED_RESET_UHCI]	= 15,
	[ASPEED_RESET_EHCI_P1]	= 14,
	[ASPEED_RESET_CRT]		= 13,
	[ASPEED_RESET_MAC2]	= 12,
	[ASPEED_RESET_MAC1]	= 11,
	[ASPEED_RESET_PECI]	= 10,
	[ASPEED_RESET_PWM]	=  9,
	[ASPEED_RESET_2D]	= 7,
	[ASPEED_RESET_VIDEO]	= 6,
	[ASPEED_RESET_LPC]		= 5,
	[ASPEED_RESET_HACE]	= 4,
	[ASPEED_RESET_EHCI_P2]	= 3,
	[ASPEED_RESET_I2C]	=  2,
	[ASPEED_RESET_AHB]	=  1,
	[ASPEED_RESET_SRAM_CTRL]	=  0,

	/*
	 * SCUD4 resets start at an offset to separate them from
	 * the SCU04 resets.
	 */
	[ASPEED_RESET_CRT1]	= ASPEED_RESET2_OFFSET + 5,
};

/*************************************************************************/
#define SCU_HW_STRAP_VGA_SIZE_GET(x)		((x >> 2)& 0x3)

#define VGA_8M_DRAM			0
#define VGA_16M_DRAM			1
#define VGA_32M_DRAM			2
#define VGA_64M_DRAM			3

extern u32 ast_scu_get_vga_memsize(void)
{
	u32 size=0;

	switch(SCU_HW_STRAP_VGA_SIZE_GET(readl(scu_base + ASPEED_STRAP))) {
		case VGA_8M_DRAM:
			size = 8*1024*1024;
			break;
		case VGA_16M_DRAM:
			size = 16*1024*1024;
			break;
		case VGA_32M_DRAM:
			size = 32*1024*1024;
			break;
		case VGA_64M_DRAM:
			size = 64*1024*1024;
			break;
		default:
			printk("error vga size \n");
			break;
	}
	return size;
}

EXPORT_SYMBOL(ast_scu_get_vga_memsize);

/*************************************************************************/

static struct clk_hw *aspeed_ast2400_calc_pll(const char *name, u32 val)
{
	unsigned int mult, div;

	if (val & AST2400_HPLL_BYPASS_EN) {
		/* Pass through mode */
		mult = div = 1;
	} else {
		/* F = 24Mhz * (2-OD) * [(N + 2) / (D + 1)] */
		u32 n = (val >> 5) & 0x3f;
		u32 od = (val >> 4) & 0x1;
		u32 d = val & 0xf;

		mult = (2 - od) * (n + 2);
		div = d + 1;
	}
	return clk_hw_register_fixed_factor(NULL, name, "clkin", 0,
			mult, div);
};

static struct clk_hw *aspeed_ast2500_calc_pll(const char *name, u32 val)
{
	unsigned int mult, div;

	if (val & AST2500_HPLL_BYPASS_EN) {
		/* Pass through mode */
		mult = div = 1;
	} else {
		/* F = clkin * [(M+1) / (N+1)] / (P + 1) */
		u32 p = (val >> 13) & 0x3f;
		u32 m = (val >> 5) & 0xff;
		u32 n = val & 0x1f;

		mult = (m + 1) / (n + 1);
		div = p + 1;
	}

	return clk_hw_register_fixed_factor(NULL, name, "clkin", 0,
			mult, div);
}

struct aspeed_clk_soc_data {
	const struct clk_div_table *div_table;
	const struct clk_div_table *mac_div_table;
	const struct clk_div_table *eclk_div_table;
	struct clk_hw *(*calc_pll)(const char *name, u32 val);
	u32		reset_reg1;
	u32		reset_reg2;
	u32		clk_select_reg1;
	u32		clk_select_reg2;	
	int new_version;
	unsigned int nr_resets;
	u8		*reset_table;
};

static const struct aspeed_clk_soc_data ast2600_data = {
	.div_table = ast2500_div_table,
	.mac_div_table = ast2500_mac_div_table,
	.eclk_div_table = ast2500_eclk_div_table,	
	.calc_pll = aspeed_ast2500_calc_pll,
	.reset_reg1 = 0x40,
	.reset_reg2 = 0x50,
	.clk_select_reg1 = 0x300,
	.clk_select_reg2 = 0x310,
	.new_version = 1,
	.nr_resets = ARRAY_SIZE(ast2600_resets),
	.reset_table = ast2600_resets,
};

static const struct aspeed_clk_soc_data ast2500_data = {
	.div_table = ast2500_div_table,
	.mac_div_table = ast2500_mac_div_table,
	.eclk_div_table = ast2500_eclk_div_table,	
	.calc_pll = aspeed_ast2500_calc_pll,
	.reset_reg1 = 0x04,
	.reset_reg2 = 0xD4,
	.clk_select_reg1 = 0x08,
	.clk_select_reg2 = 0xD8,
	.new_version = 0,	
	.nr_resets = ARRAY_SIZE(aspeed_resets),
	.reset_table = aspeed_resets,
};

static const struct aspeed_clk_soc_data ast2400_data = {
	.div_table = ast2400_div_table,
	.mac_div_table = ast2400_div_table,
	.eclk_div_table = ast2400_eclk_div_table,
	.calc_pll = aspeed_ast2400_calc_pll,
	.reset_reg1 = 0x04,
	.reset_reg2 = 0x00,
	.clk_select_reg1 = 0x08,
	.clk_select_reg2 = 0x0,
	.new_version = 0,	
	.nr_resets = ARRAY_SIZE(aspeed_resets),
	.reset_table = aspeed_resets,
};

static int aspeed_clk_is_enabled(struct clk_hw *hw)
{
	struct aspeed_clk_gate *gate = to_aspeed_clk_gate(hw);
	u32 clk = BIT(gate->clock_idx);
	u32 rst = BIT(gate->reset_idx);
	u32 enval = (gate->flags & CLK_GATE_SET_TO_DISABLE) ? 0 : clk;
	u32 reg;

	/*
	 * If the IP is in reset, treat the clock as not enabled,
	 * this happens with some clocks such as the USB one when
	 * coming from cold reset. Without this, aspeed_clk_enable()
	 * will fail to lift the reset.
	 */
	if (gate->reset_idx >= 0) {
		regmap_read(gate->map, ASPEED_RESET_CTRL, &reg);
		if (reg & rst)
			return 0;
	}

	regmap_read(gate->map, ASPEED_CLK_STOP_CTRL, &reg);

	return ((reg & clk) == enval) ? 1 : 0;
}

static int aspeed_clk_enable(struct clk_hw *hw)
{
	struct aspeed_clk_gate *gate = to_aspeed_clk_gate(hw);
	unsigned long flags;
	u32 clk = BIT(gate->clock_idx);
	u32 rst = BIT(gate->reset_idx);
	u32 enval;

	spin_lock_irqsave(gate->lock, flags);

	if (aspeed_clk_is_enabled(hw)) {
		spin_unlock_irqrestore(gate->lock, flags);
		return 0;
	}

	if (gate->reset_idx >= 0) {
		/* Put IP in reset */
		regmap_update_bits(gate->map, ASPEED_RESET_CTRL, rst, rst);

		/* Delay 100us */
		udelay(100);
	}

	/* Enable clock */
	if(gate->clock_idx == aspeed_gates[ASPEED_CLK_GATE_SDEXTCLK].clock_idx) {
		/* sd ext clk */
		regmap_update_bits(gate->map, ASPEED_CLK_SELECTION, ASPEED_SDIO_CLK_EN, ASPEED_SDIO_CLK_EN);
		printk("enable sd card clk\n");
	} else {
		enval = (gate->flags & CLK_GATE_SET_TO_DISABLE) ? 0 : clk;
		regmap_update_bits(gate->map, ASPEED_CLK_STOP_CTRL, clk, enval);
	}

	if (gate->reset_idx >= 0) {
		/* A delay of 10ms is specified by the ASPEED docs */
		mdelay(10);

		/* Take IP out of reset */
		regmap_update_bits(gate->map, ASPEED_RESET_CTRL, rst, 0);
	}

	spin_unlock_irqrestore(gate->lock, flags);

	return 0;
}

static void aspeed_clk_disable(struct clk_hw *hw)
{
	struct aspeed_clk_gate *gate = to_aspeed_clk_gate(hw);
	unsigned long flags;
	u32 clk = BIT(gate->clock_idx);
	u32 enval;

	spin_lock_irqsave(gate->lock, flags);

	enval = (gate->flags & CLK_GATE_SET_TO_DISABLE) ? clk : 0;
	regmap_update_bits(gate->map, ASPEED_CLK_STOP_CTRL, clk, enval);

	spin_unlock_irqrestore(gate->lock, flags);
}

static const struct clk_ops aspeed_clk_gate_ops = {
	.enable = aspeed_clk_enable,
	.disable = aspeed_clk_disable,
	.is_enabled = aspeed_clk_is_enabled,
};

static int aspeed_g6_clk_is_enabled(struct clk_hw *hw)
{
	struct aspeed_clk_gate *gate = to_aspeed_clk_gate(hw);
	u32 clk = BIT(gate->clock_idx);
	u32 rst = BIT(gate->reset_idx);
	u32 enval = (gate->flags & CLK_GATE_SET_TO_DISABLE) ? 0 : clk;
	u32 reg;

	printk("aspeed_g6_clk_is_enabled gate->clock_idx %d, reset_idx %d\n", gate->clock_idx, gate->reset_idx);
	/*
	 * If the IP is in reset, treat the clock as not enabled,
	 * this happens with some clocks such as the USB one when
	 * coming from cold reset. Without this, aspeed_clk_enable()
	 * will fail to lift the reset.
	 */
	if (gate->reset_idx >= 0) {
		regmap_read(gate->map, ASPEED_G6_RESET_CTRL, &reg);
		if (reg & rst)
			return 0;
	}

	regmap_read(gate->map, ASPEED_G6_CLK_STOP_CTRL, &reg);

	return ((reg & clk) == enval) ? 1 : 0;
}

static int aspeed_g6_clk_enable(struct clk_hw *hw)
{
	struct aspeed_clk_gate *gate = to_aspeed_clk_gate(hw);
	unsigned long flags;
	u32 clk = BIT(gate->clock_idx);
	u32 rst = BIT(gate->reset_idx);
	u32 enval;

	printk("aspeed_g6_clk_enable gate->clock_idx %d, reset_idx %d\n", gate->clock_idx, gate->reset_idx);

	spin_lock_irqsave(gate->lock, flags);

	if (aspeed_g6_clk_is_enabled(hw)) {
		printk("is enable return \n");
		spin_unlock_irqrestore(gate->lock, flags);
		return 0;
	}

	if (gate->reset_idx >= 0) {
		/* Put IP in reset */
		printk("write reset rst %x\n", rst);
		regmap_update_bits(gate->map, ASPEED_G6_RESET_CTRL, rst, rst);

		/* Delay 100us */
		udelay(100);
	}

	/* Enable clock */
	if(gate->clock_idx == aspeed_gates[ASPEED_CLK_GATE_SDEXTCLK].clock_idx) {
		/* sd ext clk */
		regmap_update_bits(gate->map, ASPEED_G6_CLK_SELECTION, ASPEED_SDIO_CLK_EN, ASPEED_SDIO_CLK_EN);
		printk("enable sd card clk\n");
	} else {
		enval = (gate->flags & CLK_GATE_SET_TO_DISABLE) ? 0 : clk;
		printk("enval %x gate->flags %x enable g6 clk gate->clock_idx %d gate->reset_idx %d \n",enval, gate->flags, gate->clock_idx, gate->reset_idx);
		if(enval) {
			printk("write 80 to enable clk  %x \n", clk);
			regmap_update_bits(gate->map, ASPEED_G6_CLK_STOP_CTRL, clk, clk);
		} else {
			printk("write 84 clr to enable clk mask %x val %x\n", clk, enval);			
			regmap_write(gate->map, ASPEED_G6_CLK_STOP_CTRL + 0x04, clk);
		}
	}

	if (gate->reset_idx >= 0) {
		/* A delay of 10ms is specified by the ASPEED docs */
		mdelay(10);
		printk("clear reset rst %x\n", rst);
		/* Take IP out of reset */
		regmap_write(gate->map, ASPEED_G6_RESET_CTRL + 0x04, rst);
	}

	spin_unlock_irqrestore(gate->lock, flags);

	return 0;
}

static void aspeed_g6_clk_disable(struct clk_hw *hw)
{
	struct aspeed_clk_gate *gate = to_aspeed_clk_gate(hw);
	unsigned long flags;
	u32 clk = BIT(gate->clock_idx);
	u32 enval;

	printk("aspeed_g6_clk_disable gate->clock_idx %d, reset_idx %d\n", gate->clock_idx, gate->reset_idx);

	spin_lock_irqsave(gate->lock, flags);

	enval = (gate->flags & CLK_GATE_SET_TO_DISABLE) ? clk : 0;
	regmap_update_bits(gate->map, ASPEED_G6_CLK_STOP_CTRL, clk, enval);

	spin_unlock_irqrestore(gate->lock, flags);
}

static const struct clk_ops aspeed_g6_clk_gate_ops = {
	.enable = aspeed_g6_clk_enable,
	.disable = aspeed_g6_clk_disable,
	.is_enabled = aspeed_g6_clk_is_enabled,
};

/**
 * struct aspeed_reset - Aspeed reset controller
 * @map: regmap to access the containing system controller
 * @rcdev: reset controller device
 */
struct aspeed_reset {
	struct regmap			*map;
	struct reset_controller_dev	rcdev;
	u32		reset_reg1;
	u32		reset_reg2;
	u32		clk_select_reg1;
	u32		clk_select_reg2;
	int 	new_version;
	u8		*reset_table;
};

#define to_aspeed_reset(p) container_of((p), struct aspeed_reset, rcdev)

static int aspeed_reset_deassert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	struct aspeed_reset *ar = to_aspeed_reset(rcdev);
	u32 reg = ar->reset_reg1;
	u32 bit = ar->reset_table[id];

	if (bit >= ASPEED_RESET2_OFFSET) {
		bit -= ASPEED_RESET2_OFFSET;
		reg = ar->reset_reg2;
	}

	if(ar->new_version) {
		reg += 0x04;
		return regmap_update_bits(ar->map, reg, BIT(bit), BIT(bit));
	} else {
		return regmap_update_bits(ar->map, reg, BIT(bit), 0);
	}
}

static int aspeed_reset_assert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct aspeed_reset *ar = to_aspeed_reset(rcdev);
	u32 reg = ar->reset_reg1;
	u32 bit = ar->reset_table[id];

	if (bit >= ASPEED_RESET2_OFFSET) {
		bit -= ASPEED_RESET2_OFFSET;
		reg = ar->reset_reg2;
	}

	return regmap_update_bits(ar->map, reg, BIT(bit), BIT(bit));
}

static int aspeed_reset_status(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct aspeed_reset *ar = to_aspeed_reset(rcdev);
	u32 reg = ar->reset_reg1;
	u32 bit = ar->reset_table[id];
	int ret, val;

	if (bit >= ASPEED_RESET2_OFFSET) {
		bit -= ASPEED_RESET2_OFFSET;
		reg = ar->reset_reg2;
	}

	ret = regmap_read(ar->map, reg, &val);
	if (ret)
		return ret;

	return !!(val & BIT(bit));
}

static const struct reset_control_ops aspeed_reset_ops = {
	.assert = aspeed_reset_assert,
	.deassert = aspeed_reset_deassert,
	.status = aspeed_reset_status,
};

static struct clk_hw *aspeed_clk_hw_register_gate(struct device *dev,
		const char *name, const char *parent_name, unsigned long flags,
		struct regmap *map, u8 clock_idx, u8 reset_idx,
		u8 clk_gate_flags, spinlock_t *lock, int version)
{
	struct aspeed_clk_gate *gate;
	struct clk_init_data init;
	struct clk_hw *hw;
	int ret;

	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return ERR_PTR(-ENOMEM);

	init.name = name;

	if (version)
		init.ops = &aspeed_g6_clk_gate_ops;
	else
		init.ops = &aspeed_clk_gate_ops;
	
	init.flags = flags;
	init.parent_names = parent_name ? &parent_name : NULL;
	init.num_parents = parent_name ? 1 : 0;

	gate->map = map;
	gate->clock_idx = clock_idx;
	gate->reset_idx = reset_idx;
	gate->flags = clk_gate_flags;
	gate->lock = lock;
	gate->hw.init = &init;

	hw = &gate->hw;
	ret = clk_hw_register(dev, hw);
	if (ret) {
		kfree(gate);
		hw = ERR_PTR(ret);
	}

	return hw;
}

static int aspeed_clk_probe(struct platform_device *pdev)
{
	const struct aspeed_clk_soc_data *soc_data;
	struct device *dev = &pdev->dev;
	struct aspeed_reset *ar;
	struct regmap *map;
	struct clk_hw *hw;
	u32 val, rate;
	int i, ret;

	/* SoC generations share common layouts but have different divisors */
	soc_data = of_device_get_match_data(dev);
	if (!soc_data) {
		dev_err(dev, "no match data for platform\n");
		return -EINVAL;
	}

	map = syscon_node_to_regmap(dev->of_node);
	if (IS_ERR(map)) {
		dev_err(dev, "no syscon regmap\n");
		return PTR_ERR(map);
	}

	ar = devm_kzalloc(dev, sizeof(*ar), GFP_KERNEL);
	if (!ar)
		return -ENOMEM;

	ar->map = map;
	ar->reset_reg1 = soc_data->reset_reg1;
	ar->reset_reg2 = soc_data->reset_reg2;	
	ar->clk_select_reg1 = soc_data->clk_select_reg1;
	ar->clk_select_reg2 = soc_data->clk_select_reg2;	
	
	ar->new_version = soc_data->new_version;
	ar->reset_table = soc_data->reset_table;	
	printk("ar->new_version %d table size\n", ar->new_version);
	ar->rcdev.owner = THIS_MODULE;
	ar->rcdev.ops = &aspeed_reset_ops;
	ar->rcdev.of_node = dev->of_node;
	ar->rcdev.nr_resets = soc_data->nr_resets;

	ret = devm_reset_controller_register(dev, &ar->rcdev);
	if (ret) {
		dev_err(dev, "could not register reset controller\n");
		return ret;
	}

	/* UART clock div13 setting */
	regmap_read(map, ASPEED_MISC_CTRL, &val);
	if (val & UART_DIV13_EN)
		rate = 24000000 / 13;
	else
		rate = 24000000;
	/* TODO: Find the parent data for the uart clock */
	hw = clk_hw_register_fixed_rate(dev, "uart", NULL, 0, rate);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_clk_data->hws[ASPEED_CLK_UART] = hw;

	/*
	 * Memory controller (M-PLL) PLL. This clock is configured by the
	 * bootloader, and is exposed to Linux as a read-only clock rate.
	 */
	regmap_read(map, ASPEED_MPLL_PARAM, &val);
	hw = soc_data->calc_pll("mpll", val);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_clk_data->hws[ASPEED_CLK_MPLL] =	hw;

	if(ar->new_version) {
		/* EMMC clock divider (TODO: There's a gate too) */
#if 0	
		hw = clk_hw_register_divider_table(dev, "emmc", "hpll", 0,
				scu_base + 0x300, 12, 3, 0,
				soc_data->div_table,
				&aspeed_clk_lock);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
		aspeed_clk_data->hws[ASPEED_CLK_EMMC] = hw;

		/* SD/SDIO clock divider (TODO: There's a gate too) */
		hw = clk_hw_register_divider_table(dev, "sdio", "hpll", 0,
				scu_base + 0x310, 28, 3, 0,
				soc_data->div_table,
				&aspeed_clk_lock);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
		aspeed_clk_data->hws[ASPEED_CLK_SDIO] = hw;
		
#else		
		hw = clk_hw_register_fixed_rate(NULL, "emmc", NULL, 0, 100000000);
		aspeed_clk_data->hws[ASPEED_CLK_EMMC] = hw;

		hw = clk_hw_register_fixed_rate(NULL, "sdio", NULL, 0, 200000000);
		aspeed_clk_data->hws[ASPEED_CLK_SDIO] = hw;
		
#endif

	} else {
		/* SD/SDIO clock divider (TODO: There's a gate too) */
		hw = clk_hw_register_divider_table(dev, "sdio", "hpll", 0,
				scu_base + ASPEED_CLK_SELECTION, 12, 3, 0,
				soc_data->div_table,
				&aspeed_clk_lock);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
		aspeed_clk_data->hws[ASPEED_CLK_SDIO] = hw;
	}

	/* MAC AHB bus clock divider */
	hw = clk_hw_register_divider_table(dev, "mac", "hpll", 0,
			scu_base + ASPEED_CLK_SELECTION, 16, 3, 0,
			soc_data->mac_div_table,
			&aspeed_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_clk_data->hws[ASPEED_CLK_MAC] = hw;

#if 0
if(GET_CHIP_REVISION(ast_scu_read(AST_SCU_REVISION_ID)) == 4)
	ast_scu_write((ast_scu_read(AST_SCU_CLK_SEL) & ~(SCU_ECLK_SOURCE_MASK | SCU_CLK_VIDEO_SLOW_MASK | SCU_CLK_VIDEO_SLOW_EN)), AST_SCU_CLK_SEL);
else
	ast_scu_write((ast_scu_read(AST_SCU_CLK_SEL) & ~(SCU_ECLK_SOURCE_MASK | SCU_CLK_VIDEO_SLOW_EN)) | SCU_ECLK_SOURCE(2), AST_SCU_CLK_SEL);
#endif

	/* LPC Host (LHCLK) clock divider */
	hw = clk_hw_register_divider_table(dev, "lhclk", "hpll", 0,
			scu_base + ASPEED_CLK_SELECTION, 20, 3, 0,
			soc_data->div_table,
			&aspeed_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_clk_data->hws[ASPEED_CLK_LHCLK] = hw;

	/* P-Bus (BCLK) clock divider */
	hw = clk_hw_register_divider_table(dev, "bclk", "hpll", 0,
			scu_base + ASPEED_CLK_SELECTION_2, 0, 2, 0,
			soc_data->div_table,
			&aspeed_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_clk_data->hws[ASPEED_CLK_BCLK] = hw;

	/* Fixed 24MHz clock */
	hw = clk_hw_register_fixed_rate(NULL, "fixed-24m", "clkin",
					0, 24000000);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_clk_data->hws[ASPEED_CLK_24M] = hw;

#if 1
	/* Video Engine clock divider */
	hw = clk_hw_register_divider_table(dev, "eclk", NULL, 0,
			scu_base + ASPEED_CLK_SELECTION, 28, 3, 0,
			soc_data->eclk_div_table,
			&aspeed_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_clk_data->hws[ASPEED_CLK_ECLK] = hw;
#else

	hw = clk_hw_register_mux(dev, "eclk-mux", eclk_parent_names,
				 ARRAY_SIZE(eclk_parent_names), 0,
				 scu_base + ASPEED_CLK_SELECTION, 2, 0x3, 0,
				 &aspeed_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_clk_data->hws[ASPEED_CLK_ECLK_MUX] = hw;

	/* Video Engine clock divider */
	hw = clk_hw_register_divider_table(dev, "eclk", "eclk-mux", 0,
					   scu_base + ASPEED_CLK_SELECTION, 28,
					   3, 0, soc_data->eclk_div_table,
					   &aspeed_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	aspeed_clk_data->hws[ASPEED_CLK_ECLK] = hw;
#endif
	
	/*
	 * TODO: There are a number of clocks that not included in this driver
	 * as more information is required:
	 *   D2-PLL
	 *   D-PLL
	 *   YCLK
	 *   RGMII
	 *   RMII
	 *   UART[1..5] clock source mux
	 *   Video Engine (ECLK) mux and clock divider
	 */

	for (i = 0; i < ARRAY_SIZE(aspeed_gates); i++) {
		const struct aspeed_gate_data *gd = &aspeed_gates[i];
		u32 gate_flags;

		/* Special case: the USB port 1 clock (bit 14) is always
		 * working the opposite way from the other ones.
		 */
		gate_flags = (gd->clock_idx == 14) ? 0 : CLK_GATE_SET_TO_DISABLE;
		hw = aspeed_clk_hw_register_gate(dev,
				gd->name,
				gd->parent_name,
				gd->flags,
				map,
				gd->clock_idx,
				gd->reset_idx,
				gate_flags,
				&aspeed_clk_lock,
				soc_data->new_version);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
		aspeed_clk_data->hws[i] = hw;
	}

	return 0;
};

static const struct of_device_id aspeed_clk_dt_ids[] = {
	{ .compatible = "aspeed,ast2400-scu", .data = &ast2400_data },
	{ .compatible = "aspeed,ast2500-scu", .data = &ast2500_data },
	{ .compatible = "aspeed,ast2600-scu", .data = &ast2600_data },
	{ }
};

static struct platform_driver aspeed_clk_driver = {
	.probe  = aspeed_clk_probe,
	.driver = {
		.name = "aspeed-clk",
		.of_match_table = aspeed_clk_dt_ids,
		.suppress_bind_attrs = true,
	},
};

static int __init aspeed_clk_init(void)
{
	return platform_driver_register(&aspeed_clk_driver);
}
core_initcall(aspeed_clk_init);

static void __init aspeed_ast2400_cc(struct regmap *map)
{
	struct clk_hw *hw;
	u32 val, div, clkin, hpll;
	const u16 hpll_rates[][4] = {
		{384, 360, 336, 408},
		{400, 375, 350, 425},
	};
	int rate;

	/*
	 * CLKIN is the crystal oscillator, 24, 48 or 25MHz selected by
	 * strapping
	 */
	regmap_read(map, ASPEED_STRAP, &val);
	rate = (val >> 8) & 3;
	if (val & CLKIN_25MHZ_EN) {
		clkin = 25000000;
		hpll = hpll_rates[1][rate];
	} else if (val & AST2400_CLK_SOURCE_SEL) {
		clkin = 48000000;
		hpll = hpll_rates[0][rate];
	} else {
		clkin = 24000000;
		hpll = hpll_rates[0][rate];
	}
	hw = clk_hw_register_fixed_rate(NULL, "clkin", NULL, 0, clkin);
	pr_debug("clkin @%u MHz\n", clkin / 1000000);

	/*
	 * High-speed PLL clock derived from the crystal. This the CPU clock,
	 * and we assume that it is enabled. It can be configured through the
	 * HPLL_PARAM register, or set to a specified frequency by strapping.
	 */
	regmap_read(map, ASPEED_HPLL_PARAM, &val);
	if (val & AST2400_HPLL_PROGRAMMED)
		hw = aspeed_ast2400_calc_pll("hpll", val);
	else
		hw = clk_hw_register_fixed_rate(NULL, "hpll", "clkin", 0,
				hpll * 1000000);

	aspeed_clk_data->hws[ASPEED_CLK_HPLL] = hw;

	/*
	 * Strap bits 11:10 define the CPU/AHB clock frequency ratio (aka HCLK)
	 *   00: Select CPU:AHB = 1:1
	 *   01: Select CPU:AHB = 2:1
	 *   10: Select CPU:AHB = 4:1
	 *   11: Select CPU:AHB = 3:1
	 */
	regmap_read(map, ASPEED_STRAP, &val);
	val = (val >> 10) & 0x3;
	div = val + 1;
	if (div == 3)
		div = 4;
	else if (div == 4)
		div = 3;
	hw = clk_hw_register_fixed_factor(NULL, "ahb", "hpll", 0, 1, div);
	aspeed_clk_data->hws[ASPEED_CLK_AHB] = hw;

	/* APB clock clock selection register SCU08 (aka PCLK) */
	hw = clk_hw_register_divider_table(NULL, "apb", "hpll", 0,
			scu_base + ASPEED_CLK_SELECTION, 23, 3, 0,
			ast2400_div_table,
			&aspeed_clk_lock);
	aspeed_clk_data->hws[ASPEED_CLK_APB] = hw;
}

static void __init aspeed_ast2500_cc(struct regmap *map)
{
	struct clk_hw *hw;
	u32 val, freq, div;

	/* CLKIN is the crystal oscillator, 24 or 25MHz selected by strapping */
	regmap_read(map, ASPEED_STRAP, &val);
	if (val & CLKIN_25MHZ_EN)
		freq = 25000000;
	else
		freq = 24000000;
	hw = clk_hw_register_fixed_rate(NULL, "clkin", NULL, 0, freq);
	pr_debug("clkin @%u MHz\n", freq / 1000000);

	/*
	 * High-speed PLL clock derived from the crystal. This the CPU clock,
	 * and we assume that it is enabled
	 */
	regmap_read(map, ASPEED_HPLL_PARAM, &val);
	aspeed_clk_data->hws[ASPEED_CLK_HPLL] = aspeed_ast2500_calc_pll("hpll", val);

	/* Strap bits 11:9 define the AXI/AHB clock frequency ratio (aka HCLK)*/
	regmap_read(map, ASPEED_STRAP, &val);
	val = (val >> 9) & 0x7;
	WARN(val == 0, "strapping is zero: cannot determine ahb clock");
	div = 2 * (val + 1);
	hw = clk_hw_register_fixed_factor(NULL, "ahb", "hpll", 0, 1, div);
	aspeed_clk_data->hws[ASPEED_CLK_AHB] = hw;

	/* APB clock clock selection register SCU08 (aka PCLK) */
	regmap_read(map, ASPEED_CLK_SELECTION, &val);
	val = (val >> 23) & 0x7;
	div = 4 * (val + 1);
	hw = clk_hw_register_fixed_factor(NULL, "apb", "hpll", 0, 1, div);
	aspeed_clk_data->hws[ASPEED_CLK_APB] = hw;
};

static void __init aspeed_ast2600_cc(struct regmap *map)
{
	struct device_node *cpu_node;
	struct clk_hw *hw;
	u32 val, freq, div;

	printk("aspeed_ast2600_cc \n");

	cpu_node = of_find_node_by_name(NULL, "cpu");
	/* CLKIN is the crystal oscillator, always 25MHz selected, in FPGA xilinx is 25Mhz, altera is 24Mhz */

	if (of_device_is_compatible(cpu_node, "arm,arm1176jz-s"))
		freq = 24000000;
	else
		freq = 25000000;

	hw = clk_hw_register_fixed_rate(NULL, "clkin", NULL, 0, freq);
	pr_debug("clkin @%u MHz\n", freq / 1000000);

#if 1
	/*
	 * High-speed PLL clock derived from the crystal. This the CPU clock,
	 * and we assume that it is enabled
	 */
	hw = clk_hw_register_fixed_rate(NULL, "hpll", NULL, 0, freq);
	aspeed_clk_data->hws[ASPEED_CLK_HPLL] = hw;

	hw = clk_hw_register_fixed_rate(NULL, "ahb", NULL, 0, freq);
	aspeed_clk_data->hws[ASPEED_CLK_AHB] = hw;

	hw = clk_hw_register_fixed_rate(NULL, "apb", NULL, 0, freq);
	aspeed_clk_data->hws[ASPEED_CLK_APB] = hw;

#else
	/*
	 * High-speed PLL clock derived from the crystal. This the CPU clock,
	 * and we assume that it is enabled
	 */
	regmap_read(map, ASPEED_HPLL_PARAM, &val);
	aspeed_clk_data->hws[ASPEED_CLK_HPLL] = aspeed_ast2600_calc_pll("hpll", val);

	/* Strap bits 11:9 define the AXI/AHB clock frequency ratio (aka HCLK)*/
	regmap_read(map, ASPEED_STRAP, &val);
	val = (val >> 9) & 0x7;
	WARN(val == 0, "strapping is zero: cannot determine ahb clock");
	div = 2 * (val + 1);
	hw = clk_hw_register_fixed_factor(NULL, "ahb", "hpll", 0, 1, div);
	aspeed_clk_data->hws[ASPEED_CLK_AHB] = hw;

	/* APB clock clock selection register SCU08 (aka PCLK) */
	regmap_read(map, ASPEED_CLK_SELECTION, &val);
	val = (val >> 23) & 0x7;
	div = 4 * (val + 1);
	hw = clk_hw_register_fixed_factor(NULL, "apb", "hpll", 0, 1, div);
	aspeed_clk_data->hws[ASPEED_CLK_APB] = hw;
#endif
};

static void __init aspeed_cc_init(struct device_node *np)
{
	struct regmap *map;
	u32 val;
	int ret;
	int i;

	scu_base = of_iomap(np, 0);
	if (!scu_base)
		return;

	aspeed_clk_data = kzalloc(sizeof(*aspeed_clk_data) +
			sizeof(*aspeed_clk_data->hws) * ASPEED_NUM_CLKS,
			GFP_KERNEL);
	if (!aspeed_clk_data)
		return;

	/*
	 * This way all clocks fetched before the platform device probes,
	 * except those we assign here for early use, will be deferred.
	 */
	for (i = 0; i < ASPEED_NUM_CLKS; i++)
		aspeed_clk_data->hws[i] = ERR_PTR(-EPROBE_DEFER);

	map = syscon_node_to_regmap(np);
	if (IS_ERR(map)) {
		pr_err("no syscon regmap\n");
		return;
	}
	/*
	 * We check that the regmap works on this very first access,
	 * but as this is an MMIO-backed regmap, subsequent regmap
	 * access is not going to fail and we skip error checks from
	 * this point.
	 */
	ret = regmap_read(map, ASPEED_STRAP, &val);
	if (ret) {
		pr_err("failed to read strapping register\n");
		return;
	}

	if (of_device_is_compatible(np, "aspeed,ast2400-scu"))
		aspeed_ast2400_cc(map);
	else if (of_device_is_compatible(np, "aspeed,ast2500-scu"))
		aspeed_ast2500_cc(map);
	else if (of_device_is_compatible(np, "aspeed,ast2600-scu"))
		aspeed_ast2600_cc(map);	
	else
		pr_err("unknown platform, failed to add clocks\n");

	aspeed_clk_data->num = ASPEED_NUM_CLKS;
	ret = of_clk_add_hw_provider(np, of_clk_hw_onecell_get, aspeed_clk_data);
	if (ret)
		pr_err("failed to add DT provider: %d\n", ret);
};
CLK_OF_DECLARE_DRIVER(aspeed_cc_g6, "aspeed,ast2600-scu", aspeed_cc_init);
CLK_OF_DECLARE_DRIVER(aspeed_cc_g5, "aspeed,ast2500-scu", aspeed_cc_init);
CLK_OF_DECLARE_DRIVER(aspeed_cc_g4, "aspeed,ast2400-scu", aspeed_cc_init);
