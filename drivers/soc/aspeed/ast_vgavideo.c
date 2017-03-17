/********************************************************************************
* File Name     : ast_video.c
* Author         : Ryan Chen
* Description   : AST Video Engine Controller
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
*
*   Version      : 1.0
*   History      : 
*      1. 2013/04/30 Ryan Chen create this file 
*    
********************************************************************************/

#include <linux/slab.h>
#include <linux/sched.h>

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>

#include <linux/hwmon-sysfs.h>

#include <asm/io.h>
#include <asm/uaccess.h>
#include <plat/regs-video.h>
#include <mach/ast_video.h>
#include <mach/hardware.h>
#include <plat/aspeed.h>

#include <plat/ast-bmc-scu.h>
#include <plat/ast-sdmc.h>

#include <mach/irqs.h>
#include <mach/platform.h>

//#define CONFIG_AST_VIDEO_LOCK
#define CONFIG_AUTO_MODE

#define CONFIG_AST_VIDEO_DEBUG

#ifdef CONFIG_AST_VIDEO_DEBUG
	#define VIDEO_DBG(fmt, args...) printk(KERN_DEBUG "%s() " fmt,__FUNCTION__, ## args)	
#else
	#define VIDEO_DBG(fmt, args...)
#endif

/***********************************************************************/
struct ast_vgavideo_config
{
	u8	engine;					//0: engine 0, engine 1
	u8	compression_mode; 		//0:DCT, 1:DCT_VQ mix VQ-2 color, 2:DCT_VQ mix VQ-4 color		9:
	u8	compression_format;		//0:ASPEED 1:JPEG	
	u8	capture_format;			//0:CCIR601-2 YUV, 1:JPEG YUV, 2:RGB for ASPEED mode only, 3:Gray 

	u8	YUV420_mode;			//0:YUV444, 1:YUV420
	u8	Visual_Lossless;
	u8	Y_JPEGTableSelector;
	u8	AdvanceTableSelector;
	u8	AutoMode;
};

struct ast_auto_mode
{
	u8	engine_idx;					//set 0: engine 0, engine 1
	u8	differential;					//set 0: full, 1:diff frame
	u8	mode_change;				//get 0: no, 1:change
	u32	total_size;					//get 
	u32	block_count;					//get 
};

struct ast_scaling
{
	u8	engine;					//0: engine 0, engine 1
	u8	enable;
	u16	x;
	u16	y;
};

struct ast_mode_detection
{
	unsigned char		result;		//0: pass, 1: fail
	unsigned short	src_x;
	unsigned short	src_y;
};

//IOCTL ..
#define VIDEOIOC_BASE       'V'

#define AST_VIDEO_IOC_GET_VGA_SIGNAL			_IOR(VIDEOIOC_BASE, 0x1, unsigned char)
#define AST_VIDEO_GET_MEM_SIZE_IOCRX			_IOR(VIDEOIOC_BASE, 0x2, unsigned long)
#define AST_VIDEO_GET_JPEG_OFFSET_IOCRX		_IOR(VIDEOIOC_BASE, 0x3, unsigned long)
#define AST_VIDEO_VGA_MODE_DETECTION			_IOWR(VIDEOIOC_BASE, 0x4, struct ast_mode_detection*)

#define AST_VIDEO_ENG_CONFIG					_IOW(VIDEOIOC_BASE, 0x5, struct ast_vgavideo_config*)
#define AST_VIDEO_SET_SCALING					_IOW(VIDEOIOC_BASE, 0x6, struct ast_scaling*)

#define AST_VIDEO_AUTOMODE_TRIGGER			_IOWR(VIDEOIOC_BASE, 0x7, struct ast_auto_mode*)
#define AST_VIDEO_CAPTURE_TRIGGER				_IOWR(VIDEOIOC_BASE, 0x8, unsigned long)
#define AST_VIDEO_COMPRESSION_TRIGGER		_IOWR(VIDEOIOC_BASE, 0x9, unsigned long)

#define AST_VIDEO_SET_VGA_DISPLAY				_IOW(VIDEOIOC_BASE, 0xa, int)

/***********************************************************************/
typedef struct {
	u16	HorizontalActive;
	u16	VerticalActive;
	u16	RefreshRateIndex;
	u32    PixelClock;
} INTERNAL_MODE;

INTERNAL_MODE Internal_Mode[] = {
// 1024x768
  {1024, 768, 0, 65000},
  {1024, 768, 1, 65000},
  {1024, 768, 2, 75000},
  {1024, 768, 3, 78750},
  {1024, 768, 4, 94500},
// 1280x1024
  {1280, 1024, 0, 108000},
  {1280, 1024, 1, 108000},
  {1280, 1024, 2, 135000},
  {1280, 1024, 3, 157500},      
// 1600x1200
  {1600, 1200, 0, 162000},
  {1600, 1200, 1, 162000},
  {1600, 1200, 2, 175500},
  {1600, 1200, 3, 189000},
  {1600, 1200, 4, 202500},
  {1600, 1200, 5, 229500},
// 1920x1200 reduce blank
  {1920, 1200, 0, 157000},
  {1920, 1200, 1, 157000},
};

struct fbinfo
{
	u16		x;
	u16		y;
	u8	color_mode;	//0:NON, 1:EGA, 2:VGA, 3:15bpp, 4:16bpp, 5:32bpp
	u32	PixelClock;
};

//For Socket Transfer head formate ..
struct compress_header {
	u32 data_len;
	u32 block_changed;
	u16	user_width;
	u16	user_height;
	u8	first_frame;
	u8	compress_type;
	u8	trigger_mode;
	u8	data_format;
	u8	mode;
	u8	VQMode;
	u8	Y_JPEGTableSelector;
	u8	UV_JPEGTableSelector;
	u8	AdvanceTableSelector;
	u8	Visual_Lossless;
};

struct ast_vgavideo_data {
	struct device		*misc_dev;
	void __iomem		*reg_base;			/* virtual */
	u32 				vga_base;
	int 	irq;				//Video IRQ number 
//	compress_header	
	struct compress_header	compress_mode;
	void __iomem            *stream_phy;            /* phy */
	void __iomem            *stream_virt;           /* virt */
	void __iomem             *buff0_phy;             /* phy */
	void __iomem                 *buff0_virt;            /* virt */
	void __iomem            *buff1_phy;             /* phy */
	void __iomem                *buff1_virt;            /* virt */
	void __iomem            *bcd_phy;               /* phy */
	void __iomem                *bcd_virt;              /* virt */
	void __iomem            *jpeg_phy;              /* phy */
	void __iomem                *jpeg_virt;             /* virt */
	void __iomem            *jpeg_buf0_phy;              /* phy */
	void __iomem                *jpeg_buf0_virt;             /* virt */
	phys_addr_t             *jpeg_tbl_phy;          /* phy */
	u32                             *jpeg_tbl_virt;         /* virt */

	//config 
	u8	scaling;
		
//JPEG 
	u32		video_mem_size;			/* phy size*/		
	u32		video_jpeg_offset;			/* assigned jpeg memory size*/
	u8 mode_change;	
	struct completion	mode_detect_complete;
	struct completion	automode_complete;		
	struct completion	capture_complete;
	struct completion	compression_complete;		
	
	wait_queue_head_t 	queue;	

	u32 flag;
	wait_queue_head_t 	video_wq;	

	u32 thread_flag;
	struct task_struct 		*thread_task;



	struct fbinfo					src_fbinfo;
	struct fbinfo					dest_fbinfo;
	struct completion				complete;	
	u32		sts;
	u8		direct_mode;
	u8		stage;
	u32 	bandwidth;
	struct mutex lock;	

        bool is_open;

	
};

//  RC4 structure
struct rc4_state
{
    int x;
    int y;
    int m[256];
};
 

static inline void
ast_vgavideo_write(struct ast_vgavideo_data *ast_video, u32 val, u32 reg)
{
//	VIDEO_DBG("write offset: %x, val: %x \n",reg,val);
#ifdef CONFIG_AST_VIDEO_LOCK
	//unlock 
	writel(VIDEO_PROTECT_UNLOCK, ast_video->reg_base);
	writel(val, ast_video->reg_base + reg);
	//lock
	writel(0xaa,ast_video->reg_base);	
#else
	//Video is lock after reset, need always unlock 
	//unlock 
	writel(VIDEO_PROTECT_UNLOCK, ast_video->reg_base);
	writel(val, ast_video->reg_base + reg);
#endif	
}

static inline u32
ast_vgavideo_read(struct ast_vgavideo_data *ast_video, u32 reg)
{
	u32 val = readl(ast_video->reg_base + reg);
//	VIDEO_DBG("read offset: %x, val: %x \n",reg,val);
	return val;
}

/************************************************ JPEG ***************************************************************************************/
void ast_init_jpeg_table(struct ast_vgavideo_data *ast_video)
{
	int i=0;
	int base=0;
	//JPEG header default value:
	for(i = 0; i<12; i++) {
		base = (1024*i);
		ast_video->jpeg_tbl_virt[base + 0] = 0xE0FFD8FF;
		ast_video->jpeg_tbl_virt[base + 1] = 0x464A1000;
		ast_video->jpeg_tbl_virt[base + 2] = 0x01004649;
		ast_video->jpeg_tbl_virt[base + 3] = 0x60000101;
		ast_video->jpeg_tbl_virt[base + 4] = 0x00006000;
		ast_video->jpeg_tbl_virt[base + 5] = 0x0F00FEFF;
		ast_video->jpeg_tbl_virt[base + 6] = 0x00002D05;
		ast_video->jpeg_tbl_virt[base + 7] = 0x00000000;
		ast_video->jpeg_tbl_virt[base + 8] = 0x00000000;
		ast_video->jpeg_tbl_virt[base + 9] = 0x00DBFF00;
		ast_video->jpeg_tbl_virt[base + 44] = 0x081100C0;
		ast_video->jpeg_tbl_virt[base + 45] = 0x00000000;
		ast_video->jpeg_tbl_virt[base + 47] = 0x03011102;
		ast_video->jpeg_tbl_virt[base + 48] = 0xC4FF0111;
		ast_video->jpeg_tbl_virt[base + 49] = 0x00001F00;
		ast_video->jpeg_tbl_virt[base + 50] = 0x01010501;
		ast_video->jpeg_tbl_virt[base + 51] = 0x01010101;
		ast_video->jpeg_tbl_virt[base + 52] = 0x00000000;
		ast_video->jpeg_tbl_virt[base + 53] = 0x00000000;
		ast_video->jpeg_tbl_virt[base + 54] = 0x04030201;
		ast_video->jpeg_tbl_virt[base + 55] = 0x08070605;
		ast_video->jpeg_tbl_virt[base + 56] = 0xFF0B0A09;
		ast_video->jpeg_tbl_virt[base + 57] = 0x10B500C4;
		ast_video->jpeg_tbl_virt[base + 58] = 0x03010200;
		ast_video->jpeg_tbl_virt[base + 59] = 0x03040203;
		ast_video->jpeg_tbl_virt[base + 60] = 0x04040505;
		ast_video->jpeg_tbl_virt[base + 61] = 0x7D010000;
		ast_video->jpeg_tbl_virt[base + 62] = 0x00030201;
		ast_video->jpeg_tbl_virt[base + 63] = 0x12051104;
		ast_video->jpeg_tbl_virt[base + 64] = 0x06413121;
		ast_video->jpeg_tbl_virt[base + 65] = 0x07615113;
		ast_video->jpeg_tbl_virt[base + 66] = 0x32147122;
		ast_video->jpeg_tbl_virt[base + 67] = 0x08A19181;
		ast_video->jpeg_tbl_virt[base + 68] = 0xC1B14223;
		ast_video->jpeg_tbl_virt[base + 69] = 0xF0D15215;
		ast_video->jpeg_tbl_virt[base + 70] = 0x72623324;
		ast_video->jpeg_tbl_virt[base + 71] = 0x160A0982;
		ast_video->jpeg_tbl_virt[base + 72] = 0x1A191817;
		ast_video->jpeg_tbl_virt[base + 73] = 0x28272625;
		ast_video->jpeg_tbl_virt[base + 74] = 0x35342A29;
		ast_video->jpeg_tbl_virt[base + 75] = 0x39383736;
		ast_video->jpeg_tbl_virt[base + 76] = 0x4544433A;
		ast_video->jpeg_tbl_virt[base + 77] = 0x49484746;
		ast_video->jpeg_tbl_virt[base + 78] = 0x5554534A;
		ast_video->jpeg_tbl_virt[base + 79] = 0x59585756;
		ast_video->jpeg_tbl_virt[base + 80] = 0x6564635A;
		ast_video->jpeg_tbl_virt[base + 81] = 0x69686766;
		ast_video->jpeg_tbl_virt[base + 82] = 0x7574736A;
		ast_video->jpeg_tbl_virt[base + 83] = 0x79787776;
		ast_video->jpeg_tbl_virt[base + 84] = 0x8584837A;
		ast_video->jpeg_tbl_virt[base + 85] = 0x89888786;
		ast_video->jpeg_tbl_virt[base + 86] = 0x9493928A;
		ast_video->jpeg_tbl_virt[base + 87] = 0x98979695;
		ast_video->jpeg_tbl_virt[base + 88] = 0xA3A29A99;
		ast_video->jpeg_tbl_virt[base + 89] = 0xA7A6A5A4;
		ast_video->jpeg_tbl_virt[base + 90] = 0xB2AAA9A8;
		ast_video->jpeg_tbl_virt[base + 91] = 0xB6B5B4B3;
		ast_video->jpeg_tbl_virt[base + 92] = 0xBAB9B8B7;
		ast_video->jpeg_tbl_virt[base + 93] = 0xC5C4C3C2;
		ast_video->jpeg_tbl_virt[base + 94] = 0xC9C8C7C6;
		ast_video->jpeg_tbl_virt[base + 95] = 0xD4D3D2CA;
		ast_video->jpeg_tbl_virt[base + 96] = 0xD8D7D6D5;
		ast_video->jpeg_tbl_virt[base + 97] = 0xE2E1DAD9;
		ast_video->jpeg_tbl_virt[base + 98] = 0xE6E5E4E3;
		ast_video->jpeg_tbl_virt[base + 99] = 0xEAE9E8E7;
		ast_video->jpeg_tbl_virt[base + 100] = 0xF4F3F2F1;
		ast_video->jpeg_tbl_virt[base + 101] = 0xF8F7F6F5;
		ast_video->jpeg_tbl_virt[base + 102] = 0xC4FFFAF9;
		ast_video->jpeg_tbl_virt[base + 103] = 0x00011F00;
		ast_video->jpeg_tbl_virt[base + 104] = 0x01010103;
		ast_video->jpeg_tbl_virt[base + 105] = 0x01010101;
		ast_video->jpeg_tbl_virt[base + 106] = 0x00000101;
		ast_video->jpeg_tbl_virt[base + 107] = 0x00000000;
		ast_video->jpeg_tbl_virt[base + 108] = 0x04030201;
		ast_video->jpeg_tbl_virt[base + 109] = 0x08070605;
		ast_video->jpeg_tbl_virt[base + 110] = 0xFF0B0A09;
		ast_video->jpeg_tbl_virt[base + 111] = 0x11B500C4;
		ast_video->jpeg_tbl_virt[base + 112] = 0x02010200;
		ast_video->jpeg_tbl_virt[base + 113] = 0x04030404;
		ast_video->jpeg_tbl_virt[base + 114] = 0x04040507;
		ast_video->jpeg_tbl_virt[base + 115] = 0x77020100;
		ast_video->jpeg_tbl_virt[base + 116] = 0x03020100;
		ast_video->jpeg_tbl_virt[base + 117] = 0x21050411;
		ast_video->jpeg_tbl_virt[base + 118] = 0x41120631;
		ast_video->jpeg_tbl_virt[base + 119] = 0x71610751;
		ast_video->jpeg_tbl_virt[base + 120] = 0x81322213;
		ast_video->jpeg_tbl_virt[base + 121] = 0x91421408;
		ast_video->jpeg_tbl_virt[base + 122] = 0x09C1B1A1;
		ast_video->jpeg_tbl_virt[base + 123] = 0xF0523323;
		ast_video->jpeg_tbl_virt[base + 124] = 0xD1726215;
		ast_video->jpeg_tbl_virt[base + 125] = 0x3424160A;
		ast_video->jpeg_tbl_virt[base + 126] = 0x17F125E1;
		ast_video->jpeg_tbl_virt[base + 127] = 0x261A1918;
		ast_video->jpeg_tbl_virt[base + 128] = 0x2A292827;
		ast_video->jpeg_tbl_virt[base + 129] = 0x38373635;
		ast_video->jpeg_tbl_virt[base + 130] = 0x44433A39;
		ast_video->jpeg_tbl_virt[base + 131] = 0x48474645;
		ast_video->jpeg_tbl_virt[base + 132] = 0x54534A49;
		ast_video->jpeg_tbl_virt[base + 133] = 0x58575655;
		ast_video->jpeg_tbl_virt[base + 134] = 0x64635A59;
		ast_video->jpeg_tbl_virt[base + 135] = 0x68676665;
		ast_video->jpeg_tbl_virt[base + 136] = 0x74736A69;
		ast_video->jpeg_tbl_virt[base + 137] = 0x78777675;
		ast_video->jpeg_tbl_virt[base + 138] = 0x83827A79;
		ast_video->jpeg_tbl_virt[base + 139] = 0x87868584;
		ast_video->jpeg_tbl_virt[base + 140] = 0x928A8988;
		ast_video->jpeg_tbl_virt[base + 141] = 0x96959493;
		ast_video->jpeg_tbl_virt[base + 142] = 0x9A999897;
		ast_video->jpeg_tbl_virt[base + 143] = 0xA5A4A3A2;
		ast_video->jpeg_tbl_virt[base + 144] = 0xA9A8A7A6;
		ast_video->jpeg_tbl_virt[base + 145] = 0xB4B3B2AA;
		ast_video->jpeg_tbl_virt[base + 146] = 0xB8B7B6B5;
		ast_video->jpeg_tbl_virt[base + 147] = 0xC3C2BAB9;
		ast_video->jpeg_tbl_virt[base + 148] = 0xC7C6C5C4;
		ast_video->jpeg_tbl_virt[base + 149] = 0xD2CAC9C8;
		ast_video->jpeg_tbl_virt[base + 150] = 0xD6D5D4D3;
		ast_video->jpeg_tbl_virt[base + 151] = 0xDAD9D8D7;
		ast_video->jpeg_tbl_virt[base + 152] = 0xE5E4E3E2;
		ast_video->jpeg_tbl_virt[base + 153] = 0xE9E8E7E6;
		ast_video->jpeg_tbl_virt[base + 154] = 0xF4F3F2EA;
		ast_video->jpeg_tbl_virt[base + 155] = 0xF8F7F6F5;
		ast_video->jpeg_tbl_virt[base + 156] = 0xDAFFFAF9;
		ast_video->jpeg_tbl_virt[base + 157] = 0x01030C00;
		ast_video->jpeg_tbl_virt[base + 158] = 0x03110200;
		ast_video->jpeg_tbl_virt[base + 159] = 0x003F0011;

		//Table 0
		if (i==0) {
			ast_video->jpeg_tbl_virt[base + 10] = 0x0D140043;
			ast_video->jpeg_tbl_virt[base + 11] = 0x0C0F110F;
			ast_video->jpeg_tbl_virt[base + 12] = 0x11101114;
			ast_video->jpeg_tbl_virt[base + 13] = 0x17141516;
			ast_video->jpeg_tbl_virt[base + 14] = 0x1E20321E;
			ast_video->jpeg_tbl_virt[base + 15] = 0x3D1E1B1B;
			ast_video->jpeg_tbl_virt[base + 16] = 0x32242E2B;
			ast_video->jpeg_tbl_virt[base + 17] = 0x4B4C3F48;
			ast_video->jpeg_tbl_virt[base + 18] = 0x44463F47;
			ast_video->jpeg_tbl_virt[base + 19] = 0x61735A50;
			ast_video->jpeg_tbl_virt[base + 20] = 0x566C5550;
			ast_video->jpeg_tbl_virt[base + 21] = 0x88644644;
			ast_video->jpeg_tbl_virt[base + 22] = 0x7A766C65;
			ast_video->jpeg_tbl_virt[base + 23] = 0x4D808280;
			ast_video->jpeg_tbl_virt[base + 24] = 0x8C978D60;
			ast_video->jpeg_tbl_virt[base + 25] = 0x7E73967D;
			ast_video->jpeg_tbl_virt[base + 26] = 0xDBFF7B80;
			ast_video->jpeg_tbl_virt[base + 27] = 0x1F014300;
			ast_video->jpeg_tbl_virt[base + 28] = 0x272D2121;
			ast_video->jpeg_tbl_virt[base + 29] = 0x3030582D;
			ast_video->jpeg_tbl_virt[base + 30] = 0x697BB958;
			ast_video->jpeg_tbl_virt[base + 31] = 0xB8B9B97B;
			ast_video->jpeg_tbl_virt[base + 32] = 0xB9B8A6A6;
			ast_video->jpeg_tbl_virt[base + 33] = 0xB9B9B9B9;
			ast_video->jpeg_tbl_virt[base + 34] = 0xB9B9B9B9;
			ast_video->jpeg_tbl_virt[base + 35] = 0xB9B9B9B9;
			ast_video->jpeg_tbl_virt[base + 36] = 0xB9B9B9B9;
			ast_video->jpeg_tbl_virt[base + 37] = 0xB9B9B9B9;
			ast_video->jpeg_tbl_virt[base + 38] = 0xB9B9B9B9;
			ast_video->jpeg_tbl_virt[base + 39] = 0xB9B9B9B9;
			ast_video->jpeg_tbl_virt[base + 40] = 0xB9B9B9B9;
			ast_video->jpeg_tbl_virt[base + 41] = 0xB9B9B9B9;
			ast_video->jpeg_tbl_virt[base + 42] = 0xB9B9B9B9;
			ast_video->jpeg_tbl_virt[base + 43] = 0xFFB9B9B9;
		}
		//Table 1
		if (i==1) {
			ast_video->jpeg_tbl_virt[base + 10] = 0x0C110043;
			ast_video->jpeg_tbl_virt[base + 11] = 0x0A0D0F0D;
			ast_video->jpeg_tbl_virt[base + 12] = 0x0F0E0F11;
			ast_video->jpeg_tbl_virt[base + 13] = 0x14111213;
			ast_video->jpeg_tbl_virt[base + 14] = 0x1A1C2B1A;
			ast_video->jpeg_tbl_virt[base + 15] = 0x351A1818;
			ast_video->jpeg_tbl_virt[base + 16] = 0x2B1F2826;
			ast_video->jpeg_tbl_virt[base + 17] = 0x4142373F;
			ast_video->jpeg_tbl_virt[base + 18] = 0x3C3D373E;
			ast_video->jpeg_tbl_virt[base + 19] = 0x55644E46;
			ast_video->jpeg_tbl_virt[base + 20] = 0x4B5F4A46;
			ast_video->jpeg_tbl_virt[base + 21] = 0x77573D3C;
			ast_video->jpeg_tbl_virt[base + 22] = 0x6B675F58;
			ast_video->jpeg_tbl_virt[base + 23] = 0x43707170;
			ast_video->jpeg_tbl_virt[base + 24] = 0x7A847B54;
			ast_video->jpeg_tbl_virt[base + 25] = 0x6E64836D;
			ast_video->jpeg_tbl_virt[base + 26] = 0xDBFF6C70;
			ast_video->jpeg_tbl_virt[base + 27] = 0x1B014300;
			ast_video->jpeg_tbl_virt[base + 28] = 0x22271D1D;
			ast_video->jpeg_tbl_virt[base + 29] = 0x2A2A4C27;
			ast_video->jpeg_tbl_virt[base + 30] = 0x5B6BA04C;
			ast_video->jpeg_tbl_virt[base + 31] = 0xA0A0A06B;
			ast_video->jpeg_tbl_virt[base + 32] = 0xA0A0A0A0;
			ast_video->jpeg_tbl_virt[base + 33] = 0xA0A0A0A0;
			ast_video->jpeg_tbl_virt[base + 34] = 0xA0A0A0A0;
			ast_video->jpeg_tbl_virt[base + 35] = 0xA0A0A0A0;
			ast_video->jpeg_tbl_virt[base + 36] = 0xA0A0A0A0;
			ast_video->jpeg_tbl_virt[base + 37] = 0xA0A0A0A0;
			ast_video->jpeg_tbl_virt[base + 38] = 0xA0A0A0A0;
			ast_video->jpeg_tbl_virt[base + 39] = 0xA0A0A0A0;
			ast_video->jpeg_tbl_virt[base + 40] = 0xA0A0A0A0;
			ast_video->jpeg_tbl_virt[base + 41] = 0xA0A0A0A0;
			ast_video->jpeg_tbl_virt[base + 42] = 0xA0A0A0A0;
			ast_video->jpeg_tbl_virt[base + 43] = 0xFFA0A0A0;
		}
		//Table 2
		if (i==2) {
			ast_video->jpeg_tbl_virt[base + 10] = 0x090E0043;
			ast_video->jpeg_tbl_virt[base + 11] = 0x090A0C0A;
			ast_video->jpeg_tbl_virt[base + 12] = 0x0C0B0C0E;
			ast_video->jpeg_tbl_virt[base + 13] = 0x110E0F10;
			ast_video->jpeg_tbl_virt[base + 14] = 0x15172415;
			ast_video->jpeg_tbl_virt[base + 15] = 0x2C151313;
			ast_video->jpeg_tbl_virt[base + 16] = 0x241A211F;
			ast_video->jpeg_tbl_virt[base + 17] = 0x36372E34;
			ast_video->jpeg_tbl_virt[base + 18] = 0x31322E33;
			ast_video->jpeg_tbl_virt[base + 19] = 0x4653413A;
			ast_video->jpeg_tbl_virt[base + 20] = 0x3E4E3D3A;
			ast_video->jpeg_tbl_virt[base + 21] = 0x62483231;
			ast_video->jpeg_tbl_virt[base + 22] = 0x58564E49;
			ast_video->jpeg_tbl_virt[base + 23] = 0x385D5E5D;
			ast_video->jpeg_tbl_virt[base + 24] = 0x656D6645;
			ast_video->jpeg_tbl_virt[base + 25] = 0x5B536C5A;
			ast_video->jpeg_tbl_virt[base + 26] = 0xDBFF595D;
			ast_video->jpeg_tbl_virt[base + 27] = 0x16014300;
			ast_video->jpeg_tbl_virt[base + 28] = 0x1C201818;
			ast_video->jpeg_tbl_virt[base + 29] = 0x22223F20;
			ast_video->jpeg_tbl_virt[base + 30] = 0x4B58853F;
			ast_video->jpeg_tbl_virt[base + 31] = 0x85858558;
			ast_video->jpeg_tbl_virt[base + 32] = 0x85858585;
			ast_video->jpeg_tbl_virt[base + 33] = 0x85858585;
			ast_video->jpeg_tbl_virt[base + 34] = 0x85858585;
			ast_video->jpeg_tbl_virt[base + 35] = 0x85858585;
			ast_video->jpeg_tbl_virt[base + 36] = 0x85858585;
			ast_video->jpeg_tbl_virt[base + 37] = 0x85858585;
			ast_video->jpeg_tbl_virt[base + 38] = 0x85858585;
			ast_video->jpeg_tbl_virt[base + 39] = 0x85858585;
			ast_video->jpeg_tbl_virt[base + 40] = 0x85858585;
			ast_video->jpeg_tbl_virt[base + 41] = 0x85858585;
			ast_video->jpeg_tbl_virt[base + 42] = 0x85858585;
			ast_video->jpeg_tbl_virt[base + 43] = 0xFF858585;
		}
		//Table 3
		if (i==3) {
			ast_video->jpeg_tbl_virt[base + 10] = 0x070B0043;
			ast_video->jpeg_tbl_virt[base + 11] = 0x07080A08;
			ast_video->jpeg_tbl_virt[base + 12] = 0x0A090A0B;
			ast_video->jpeg_tbl_virt[base + 13] = 0x0D0B0C0C;
			ast_video->jpeg_tbl_virt[base + 14] = 0x11121C11;
			ast_video->jpeg_tbl_virt[base + 15] = 0x23110F0F;
			ast_video->jpeg_tbl_virt[base + 16] = 0x1C141A19;
			ast_video->jpeg_tbl_virt[base + 17] = 0x2B2B2429;
			ast_video->jpeg_tbl_virt[base + 18] = 0x27282428;
			ast_video->jpeg_tbl_virt[base + 19] = 0x3842332E;
			ast_video->jpeg_tbl_virt[base + 20] = 0x313E302E;
			ast_video->jpeg_tbl_virt[base + 21] = 0x4E392827;
			ast_video->jpeg_tbl_virt[base + 22] = 0x46443E3A;
			ast_video->jpeg_tbl_virt[base + 23] = 0x2C4A4A4A;
			ast_video->jpeg_tbl_virt[base + 24] = 0x50565137;
			ast_video->jpeg_tbl_virt[base + 25] = 0x48425647;
			ast_video->jpeg_tbl_virt[base + 26] = 0xDBFF474A;
			ast_video->jpeg_tbl_virt[base + 27] = 0x12014300;
			ast_video->jpeg_tbl_virt[base + 28] = 0x161A1313;
			ast_video->jpeg_tbl_virt[base + 29] = 0x1C1C331A;
			ast_video->jpeg_tbl_virt[base + 30] = 0x3D486C33;
			ast_video->jpeg_tbl_virt[base + 31] = 0x6C6C6C48;
			ast_video->jpeg_tbl_virt[base + 32] = 0x6C6C6C6C;
			ast_video->jpeg_tbl_virt[base + 33] = 0x6C6C6C6C;
			ast_video->jpeg_tbl_virt[base + 34] = 0x6C6C6C6C;
			ast_video->jpeg_tbl_virt[base + 35] = 0x6C6C6C6C;
			ast_video->jpeg_tbl_virt[base + 36] = 0x6C6C6C6C;
			ast_video->jpeg_tbl_virt[base + 37] = 0x6C6C6C6C;
			ast_video->jpeg_tbl_virt[base + 38] = 0x6C6C6C6C;
			ast_video->jpeg_tbl_virt[base + 39] = 0x6C6C6C6C;
			ast_video->jpeg_tbl_virt[base + 40] = 0x6C6C6C6C;
			ast_video->jpeg_tbl_virt[base + 41] = 0x6C6C6C6C;
			ast_video->jpeg_tbl_virt[base + 42] = 0x6C6C6C6C;
			ast_video->jpeg_tbl_virt[base + 43] = 0xFF6C6C6C;
		}
		//Table 4
		if (i==4) {
			ast_video->jpeg_tbl_virt[base + 10] = 0x06090043;
			ast_video->jpeg_tbl_virt[base + 11] = 0x05060706;
			ast_video->jpeg_tbl_virt[base + 12] = 0x07070709;
			ast_video->jpeg_tbl_virt[base + 13] = 0x0A09090A;
			ast_video->jpeg_tbl_virt[base + 14] = 0x0D0E160D;
			ast_video->jpeg_tbl_virt[base + 15] = 0x1B0D0C0C;
			ast_video->jpeg_tbl_virt[base + 16] = 0x16101413;
			ast_video->jpeg_tbl_virt[base + 17] = 0x21221C20;
			ast_video->jpeg_tbl_virt[base + 18] = 0x1E1F1C20;
			ast_video->jpeg_tbl_virt[base + 19] = 0x2B332824;
			ast_video->jpeg_tbl_virt[base + 20] = 0x26302624;
			ast_video->jpeg_tbl_virt[base + 21] = 0x3D2D1F1E;
			ast_video->jpeg_tbl_virt[base + 22] = 0x3735302D;
			ast_video->jpeg_tbl_virt[base + 23] = 0x22393A39;
			ast_video->jpeg_tbl_virt[base + 24] = 0x3F443F2B;
			ast_video->jpeg_tbl_virt[base + 25] = 0x38334338;
			ast_video->jpeg_tbl_virt[base + 26] = 0xDBFF3739;
			ast_video->jpeg_tbl_virt[base + 27] = 0x0D014300;
			ast_video->jpeg_tbl_virt[base + 28] = 0x11130E0E;
			ast_video->jpeg_tbl_virt[base + 29] = 0x15152613;
			ast_video->jpeg_tbl_virt[base + 30] = 0x2D355026;
			ast_video->jpeg_tbl_virt[base + 31] = 0x50505035;
			ast_video->jpeg_tbl_virt[base + 32] = 0x50505050;
			ast_video->jpeg_tbl_virt[base + 33] = 0x50505050;
			ast_video->jpeg_tbl_virt[base + 34] = 0x50505050;
			ast_video->jpeg_tbl_virt[base + 35] = 0x50505050;
			ast_video->jpeg_tbl_virt[base + 36] = 0x50505050;
			ast_video->jpeg_tbl_virt[base + 37] = 0x50505050;
			ast_video->jpeg_tbl_virt[base + 38] = 0x50505050;
			ast_video->jpeg_tbl_virt[base + 39] = 0x50505050;
			ast_video->jpeg_tbl_virt[base + 40] = 0x50505050;
			ast_video->jpeg_tbl_virt[base + 41] = 0x50505050;
			ast_video->jpeg_tbl_virt[base + 42] = 0x50505050;
			ast_video->jpeg_tbl_virt[base + 43] = 0xFF505050;
		}
		//Table 5
		if (i==5) {
			ast_video->jpeg_tbl_virt[base + 10] = 0x04060043;
			ast_video->jpeg_tbl_virt[base + 11] = 0x03040504;
			ast_video->jpeg_tbl_virt[base + 12] = 0x05040506;
			ast_video->jpeg_tbl_virt[base + 13] = 0x07060606;
			ast_video->jpeg_tbl_virt[base + 14] = 0x09090F09;
			ast_video->jpeg_tbl_virt[base + 15] = 0x12090808;
			ast_video->jpeg_tbl_virt[base + 16] = 0x0F0A0D0D;
			ast_video->jpeg_tbl_virt[base + 17] = 0x16161315;
			ast_video->jpeg_tbl_virt[base + 18] = 0x14151315;
			ast_video->jpeg_tbl_virt[base + 19] = 0x1D221B18;
			ast_video->jpeg_tbl_virt[base + 20] = 0x19201918;
			ast_video->jpeg_tbl_virt[base + 21] = 0x281E1514;
			ast_video->jpeg_tbl_virt[base + 22] = 0x2423201E;
			ast_video->jpeg_tbl_virt[base + 23] = 0x17262726;
			ast_video->jpeg_tbl_virt[base + 24] = 0x2A2D2A1C;
			ast_video->jpeg_tbl_virt[base + 25] = 0x25222D25;
			ast_video->jpeg_tbl_virt[base + 26] = 0xDBFF2526;
			ast_video->jpeg_tbl_virt[base + 27] = 0x09014300;
			ast_video->jpeg_tbl_virt[base + 28] = 0x0B0D0A0A;
			ast_video->jpeg_tbl_virt[base + 29] = 0x0E0E1A0D;
			ast_video->jpeg_tbl_virt[base + 30] = 0x1F25371A;
			ast_video->jpeg_tbl_virt[base + 31] = 0x37373725;
			ast_video->jpeg_tbl_virt[base + 32] = 0x37373737;
			ast_video->jpeg_tbl_virt[base + 33] = 0x37373737;
			ast_video->jpeg_tbl_virt[base + 34] = 0x37373737;
			ast_video->jpeg_tbl_virt[base + 35] = 0x37373737;
			ast_video->jpeg_tbl_virt[base + 36] = 0x37373737;
			ast_video->jpeg_tbl_virt[base + 37] = 0x37373737;
			ast_video->jpeg_tbl_virt[base + 38] = 0x37373737;
			ast_video->jpeg_tbl_virt[base + 39] = 0x37373737;
			ast_video->jpeg_tbl_virt[base + 40] = 0x37373737;
			ast_video->jpeg_tbl_virt[base + 41] = 0x37373737;
			ast_video->jpeg_tbl_virt[base + 42] = 0x37373737;
			ast_video->jpeg_tbl_virt[base + 43] = 0xFF373737;
		}
		//Table 6
		if (i==6) {
			ast_video->jpeg_tbl_virt[base + 10] = 0x02030043;
			ast_video->jpeg_tbl_virt[base + 11] = 0x01020202;
			ast_video->jpeg_tbl_virt[base + 12] = 0x02020203;
			ast_video->jpeg_tbl_virt[base + 13] = 0x03030303;
			ast_video->jpeg_tbl_virt[base + 14] = 0x04040704;
			ast_video->jpeg_tbl_virt[base + 15] = 0x09040404;
			ast_video->jpeg_tbl_virt[base + 16] = 0x07050606;
			ast_video->jpeg_tbl_virt[base + 17] = 0x0B0B090A;
			ast_video->jpeg_tbl_virt[base + 18] = 0x0A0A090A;
			ast_video->jpeg_tbl_virt[base + 19] = 0x0E110D0C;
			ast_video->jpeg_tbl_virt[base + 20] = 0x0C100C0C;
			ast_video->jpeg_tbl_virt[base + 21] = 0x140F0A0A;
			ast_video->jpeg_tbl_virt[base + 22] = 0x1211100F;
			ast_video->jpeg_tbl_virt[base + 23] = 0x0B131313;
			ast_video->jpeg_tbl_virt[base + 24] = 0x1516150E;
			ast_video->jpeg_tbl_virt[base + 25] = 0x12111612;
			ast_video->jpeg_tbl_virt[base + 26] = 0xDBFF1213;
			ast_video->jpeg_tbl_virt[base + 27] = 0x04014300;
			ast_video->jpeg_tbl_virt[base + 28] = 0x05060505;
			ast_video->jpeg_tbl_virt[base + 29] = 0x07070D06;
			ast_video->jpeg_tbl_virt[base + 30] = 0x0F121B0D;
			ast_video->jpeg_tbl_virt[base + 31] = 0x1B1B1B12;
			ast_video->jpeg_tbl_virt[base + 32] = 0x1B1B1B1B;
			ast_video->jpeg_tbl_virt[base + 33] = 0x1B1B1B1B;
			ast_video->jpeg_tbl_virt[base + 34] = 0x1B1B1B1B;
			ast_video->jpeg_tbl_virt[base + 35] = 0x1B1B1B1B;
			ast_video->jpeg_tbl_virt[base + 36] = 0x1B1B1B1B;
			ast_video->jpeg_tbl_virt[base + 37] = 0x1B1B1B1B;
			ast_video->jpeg_tbl_virt[base + 38] = 0x1B1B1B1B;
			ast_video->jpeg_tbl_virt[base + 39] = 0x1B1B1B1B;
			ast_video->jpeg_tbl_virt[base + 40] = 0x1B1B1B1B;
			ast_video->jpeg_tbl_virt[base + 41] = 0x1B1B1B1B;
			ast_video->jpeg_tbl_virt[base + 42] = 0x1B1B1B1B;
			ast_video->jpeg_tbl_virt[base + 43] = 0xFF1B1B1B;
		}
		//Table 7
		if (i==7) {
			ast_video->jpeg_tbl_virt[base + 10] = 0x01020043;
			ast_video->jpeg_tbl_virt[base + 11] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 12] = 0x01010102;
			ast_video->jpeg_tbl_virt[base + 13] = 0x02020202;
			ast_video->jpeg_tbl_virt[base + 14] = 0x03030503;
			ast_video->jpeg_tbl_virt[base + 15] = 0x06030202;
			ast_video->jpeg_tbl_virt[base + 16] = 0x05030404;
			ast_video->jpeg_tbl_virt[base + 17] = 0x07070607;
			ast_video->jpeg_tbl_virt[base + 18] = 0x06070607;
			ast_video->jpeg_tbl_virt[base + 19] = 0x090B0908;
			ast_video->jpeg_tbl_virt[base + 20] = 0x080A0808;
			ast_video->jpeg_tbl_virt[base + 21] = 0x0D0A0706;
			ast_video->jpeg_tbl_virt[base + 22] = 0x0C0B0A0A;
			ast_video->jpeg_tbl_virt[base + 23] = 0x070C0D0C;
			ast_video->jpeg_tbl_virt[base + 24] = 0x0E0F0E09;
			ast_video->jpeg_tbl_virt[base + 25] = 0x0C0B0F0C;
			ast_video->jpeg_tbl_virt[base + 26] = 0xDBFF0C0C;
			ast_video->jpeg_tbl_virt[base + 27] = 0x03014300;
			ast_video->jpeg_tbl_virt[base + 28] = 0x03040303;
			ast_video->jpeg_tbl_virt[base + 29] = 0x04040804;
			ast_video->jpeg_tbl_virt[base + 30] = 0x0A0C1208;
			ast_video->jpeg_tbl_virt[base + 31] = 0x1212120C;
			ast_video->jpeg_tbl_virt[base + 32] = 0x12121212;
			ast_video->jpeg_tbl_virt[base + 33] = 0x12121212;
			ast_video->jpeg_tbl_virt[base + 34] = 0x12121212;
			ast_video->jpeg_tbl_virt[base + 35] = 0x12121212;
			ast_video->jpeg_tbl_virt[base + 36] = 0x12121212;
			ast_video->jpeg_tbl_virt[base + 37] = 0x12121212;
			ast_video->jpeg_tbl_virt[base + 38] = 0x12121212;
			ast_video->jpeg_tbl_virt[base + 39] = 0x12121212;
			ast_video->jpeg_tbl_virt[base + 40] = 0x12121212;
			ast_video->jpeg_tbl_virt[base + 41] = 0x12121212;
			ast_video->jpeg_tbl_virt[base + 42] = 0x12121212;
			ast_video->jpeg_tbl_virt[base + 43] = 0xFF121212;
		}
		//Table 8
		if (i==8) {
			ast_video->jpeg_tbl_virt[base + 10] = 0x01020043;
			ast_video->jpeg_tbl_virt[base + 11] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 12] = 0x01010102;
			ast_video->jpeg_tbl_virt[base + 13] = 0x02020202;
			ast_video->jpeg_tbl_virt[base + 14] = 0x03030503;
			ast_video->jpeg_tbl_virt[base + 15] = 0x06030202;
			ast_video->jpeg_tbl_virt[base + 16] = 0x05030404;
			ast_video->jpeg_tbl_virt[base + 17] = 0x07070607;
			ast_video->jpeg_tbl_virt[base + 18] = 0x06070607;
			ast_video->jpeg_tbl_virt[base + 19] = 0x090B0908;
			ast_video->jpeg_tbl_virt[base + 20] = 0x080A0808;
			ast_video->jpeg_tbl_virt[base + 21] = 0x0D0A0706;
			ast_video->jpeg_tbl_virt[base + 22] = 0x0C0B0A0A;
			ast_video->jpeg_tbl_virt[base + 23] = 0x070C0D0C;
			ast_video->jpeg_tbl_virt[base + 24] = 0x0E0F0E09;
			ast_video->jpeg_tbl_virt[base + 25] = 0x0C0B0F0C;
			ast_video->jpeg_tbl_virt[base + 26] = 0xDBFF0C0C;
			ast_video->jpeg_tbl_virt[base + 27] = 0x02014300;
			ast_video->jpeg_tbl_virt[base + 28] = 0x03030202;
			ast_video->jpeg_tbl_virt[base + 29] = 0x04040703;
			ast_video->jpeg_tbl_virt[base + 30] = 0x080A0F07;
			ast_video->jpeg_tbl_virt[base + 31] = 0x0F0F0F0A;
			ast_video->jpeg_tbl_virt[base + 32] = 0x0F0F0F0F;
			ast_video->jpeg_tbl_virt[base + 33] = 0x0F0F0F0F;
			ast_video->jpeg_tbl_virt[base + 34] = 0x0F0F0F0F;
			ast_video->jpeg_tbl_virt[base + 35] = 0x0F0F0F0F;
			ast_video->jpeg_tbl_virt[base + 36] = 0x0F0F0F0F;
			ast_video->jpeg_tbl_virt[base + 37] = 0x0F0F0F0F;
			ast_video->jpeg_tbl_virt[base + 38] = 0x0F0F0F0F;
			ast_video->jpeg_tbl_virt[base + 39] = 0x0F0F0F0F;
			ast_video->jpeg_tbl_virt[base + 40] = 0x0F0F0F0F;
			ast_video->jpeg_tbl_virt[base + 41] = 0x0F0F0F0F;
			ast_video->jpeg_tbl_virt[base + 42] = 0x0F0F0F0F;
			ast_video->jpeg_tbl_virt[base + 43] = 0xFF0F0F0F;
		}
		//Table 9
		if (i==9) {
			ast_video->jpeg_tbl_virt[base + 10] = 0x01010043;
			ast_video->jpeg_tbl_virt[base + 11] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 12] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 13] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 14] = 0x02020302;
			ast_video->jpeg_tbl_virt[base + 15] = 0x04020202;
			ast_video->jpeg_tbl_virt[base + 16] = 0x03020303;
			ast_video->jpeg_tbl_virt[base + 17] = 0x05050405;
			ast_video->jpeg_tbl_virt[base + 18] = 0x05050405;
			ast_video->jpeg_tbl_virt[base + 19] = 0x07080606;
			ast_video->jpeg_tbl_virt[base + 20] = 0x06080606;
			ast_video->jpeg_tbl_virt[base + 21] = 0x0A070505;
			ast_video->jpeg_tbl_virt[base + 22] = 0x09080807;
			ast_video->jpeg_tbl_virt[base + 23] = 0x05090909;
			ast_video->jpeg_tbl_virt[base + 24] = 0x0A0B0A07;
			ast_video->jpeg_tbl_virt[base + 25] = 0x09080B09;
			ast_video->jpeg_tbl_virt[base + 26] = 0xDBFF0909;
			ast_video->jpeg_tbl_virt[base + 27] = 0x02014300;
			ast_video->jpeg_tbl_virt[base + 28] = 0x02030202;
			ast_video->jpeg_tbl_virt[base + 29] = 0x03030503;
			ast_video->jpeg_tbl_virt[base + 30] = 0x07080C05;
			ast_video->jpeg_tbl_virt[base + 31] = 0x0C0C0C08;
			ast_video->jpeg_tbl_virt[base + 32] = 0x0C0C0C0C;
			ast_video->jpeg_tbl_virt[base + 33] = 0x0C0C0C0C;
			ast_video->jpeg_tbl_virt[base + 34] = 0x0C0C0C0C;
			ast_video->jpeg_tbl_virt[base + 35] = 0x0C0C0C0C;
			ast_video->jpeg_tbl_virt[base + 36] = 0x0C0C0C0C;
			ast_video->jpeg_tbl_virt[base + 37] = 0x0C0C0C0C;
			ast_video->jpeg_tbl_virt[base + 38] = 0x0C0C0C0C;
			ast_video->jpeg_tbl_virt[base + 39] = 0x0C0C0C0C;
			ast_video->jpeg_tbl_virt[base + 40] = 0x0C0C0C0C;
			ast_video->jpeg_tbl_virt[base + 41] = 0x0C0C0C0C;
			ast_video->jpeg_tbl_virt[base + 42] = 0x0C0C0C0C;
			ast_video->jpeg_tbl_virt[base + 43] = 0xFF0C0C0C;
		}
		//Table 10
		if (i==10) {
			ast_video->jpeg_tbl_virt[base + 10] = 0x01010043;
			ast_video->jpeg_tbl_virt[base + 11] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 12] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 13] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 14] = 0x01010201;
			ast_video->jpeg_tbl_virt[base + 15] = 0x03010101;
			ast_video->jpeg_tbl_virt[base + 16] = 0x02010202;
			ast_video->jpeg_tbl_virt[base + 17] = 0x03030303;
			ast_video->jpeg_tbl_virt[base + 18] = 0x03030303;
			ast_video->jpeg_tbl_virt[base + 19] = 0x04050404;
			ast_video->jpeg_tbl_virt[base + 20] = 0x04050404;
			ast_video->jpeg_tbl_virt[base + 21] = 0x06050303;
			ast_video->jpeg_tbl_virt[base + 22] = 0x06050505;
			ast_video->jpeg_tbl_virt[base + 23] = 0x03060606;
			ast_video->jpeg_tbl_virt[base + 24] = 0x07070704;
			ast_video->jpeg_tbl_virt[base + 25] = 0x06050706;
			ast_video->jpeg_tbl_virt[base + 26] = 0xDBFF0606;
			ast_video->jpeg_tbl_virt[base + 27] = 0x01014300;
			ast_video->jpeg_tbl_virt[base + 28] = 0x01020101;
			ast_video->jpeg_tbl_virt[base + 29] = 0x02020402;
			ast_video->jpeg_tbl_virt[base + 30] = 0x05060904;
			ast_video->jpeg_tbl_virt[base + 31] = 0x09090906;
			ast_video->jpeg_tbl_virt[base + 32] = 0x09090909;
			ast_video->jpeg_tbl_virt[base + 33] = 0x09090909;
			ast_video->jpeg_tbl_virt[base + 34] = 0x09090909;
			ast_video->jpeg_tbl_virt[base + 35] = 0x09090909;
			ast_video->jpeg_tbl_virt[base + 36] = 0x09090909;
			ast_video->jpeg_tbl_virt[base + 37] = 0x09090909;
			ast_video->jpeg_tbl_virt[base + 38] = 0x09090909;
			ast_video->jpeg_tbl_virt[base + 39] = 0x09090909;
			ast_video->jpeg_tbl_virt[base + 40] = 0x09090909;
			ast_video->jpeg_tbl_virt[base + 41] = 0x09090909;
			ast_video->jpeg_tbl_virt[base + 42] = 0x09090909;
			ast_video->jpeg_tbl_virt[base + 43] = 0xFF090909;
		}
		//Table 11
		if (i==11) {
			ast_video->jpeg_tbl_virt[base + 10] = 0x01010043;
			ast_video->jpeg_tbl_virt[base + 11] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 12] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 13] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 14] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 15] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 16] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 17] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 18] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 19] = 0x02020202;
			ast_video->jpeg_tbl_virt[base + 20] = 0x02020202;
			ast_video->jpeg_tbl_virt[base + 21] = 0x03020101;
			ast_video->jpeg_tbl_virt[base + 22] = 0x03020202;
			ast_video->jpeg_tbl_virt[base + 23] = 0x01030303;
			ast_video->jpeg_tbl_virt[base + 24] = 0x03030302;
			ast_video->jpeg_tbl_virt[base + 25] = 0x03020303;
			ast_video->jpeg_tbl_virt[base + 26] = 0xDBFF0403;
			ast_video->jpeg_tbl_virt[base + 27] = 0x01014300;
			ast_video->jpeg_tbl_virt[base + 28] = 0x01010101;
			ast_video->jpeg_tbl_virt[base + 29] = 0x01010201;
			ast_video->jpeg_tbl_virt[base + 30] = 0x03040602;
			ast_video->jpeg_tbl_virt[base + 31] = 0x06060604;
			ast_video->jpeg_tbl_virt[base + 32] = 0x06060606;
			ast_video->jpeg_tbl_virt[base + 33] = 0x06060606;
			ast_video->jpeg_tbl_virt[base + 34] = 0x06060606;
			ast_video->jpeg_tbl_virt[base + 35] = 0x06060606;
			ast_video->jpeg_tbl_virt[base + 36] = 0x06060606;
			ast_video->jpeg_tbl_virt[base + 37] = 0x06060606;
			ast_video->jpeg_tbl_virt[base + 38] = 0x06060606;
			ast_video->jpeg_tbl_virt[base + 39] = 0x06060606;
			ast_video->jpeg_tbl_virt[base + 40] = 0x06060606;
			ast_video->jpeg_tbl_virt[base + 41] = 0x06060606;
			ast_video->jpeg_tbl_virt[base + 42] = 0x06060606;
			ast_video->jpeg_tbl_virt[base + 43] = 0xFF060606;
		}
	}


}

static u8 ast_get_vga_signal(struct ast_vgavideo_data *ast_video)
{
	u32 VR34C, VR350, VR35C;
	u8	color_mode;
	
	VR35C = ast_vgavideo_read(ast_video, AST_VIDEO_SCRATCH_35C);
	VR35C &= 0xff000000;

	if(VR35C & (SCRATCH_VGA_PWR_STS_HSYNC | SCRATCH_VGA_PWR_STS_VSYNC)) {
		VIDEO_DBG("No VGA Signal : PWR STS %x \n", VR35C);
		return VGA_NO_SIGNAL;
	} else if (VR35C == SCRATCH_VGA_MASK_REG) {
		VIDEO_DBG("No VGA Signal : MASK %x \n", VR35C);
		return VGA_NO_SIGNAL;
	} else if (VR35C & SCRATCH_VGA_SCREEN_OFF) {
		VIDEO_DBG("No VGA Signal : Screen off %x \n", VR35C);
		return VGA_NO_SIGNAL;		
	} else if (!(VR35C & (SCRATCH_VGA_ATTRIBTE_INDEX_BIT5 | SCRATCH_VGA_MASK_REG | SCRATCH_VGA_CRT_RST | SCRATCH_VGA_RESET | SCRATCH_VGA_ENABLE ))) {
		VIDEO_DBG("NO VGA Signal : unknow %x \n", VR35C);
		return VGA_NO_SIGNAL;
	} else {
		VIDEO_DBG("VGA Signal VR35C %x \n", VR35C);
		VR350 = ast_vgavideo_read(ast_video, AST_VIDEO_SCRATCH_350);
		if(SCRATCH_VGA_GET_MODE_HEADER(VR350) == 0xA8) {
			color_mode = SCRATCH_VGA_GET_NEW_COLOR_MODE(VR350);
		} else {
			VR34C = ast_vgavideo_read(ast_video, AST_VIDEO_SCRATCH_34C);	
			if(SCRATCH_VGA_GET_COLOR_MODE(VR34C) >= VGA_15BPP_MODE) 
				color_mode = SCRATCH_VGA_GET_COLOR_MODE(VR34C);
			else
				color_mode = SCRATCH_VGA_GET_COLOR_MODE(VR34C);
		}
		if(color_mode == 0) {
			VIDEO_DBG("EGA Mode \n");
			ast_video->src_fbinfo.color_mode = EGA_MODE;
			return EGA_MODE;
		} else if (color_mode == 1) {
			VIDEO_DBG("VGA Mode \n");
			ast_video->src_fbinfo.color_mode = VGA_MODE;			
			return VGA_MODE;
		} else if (color_mode == 2) {
			VIDEO_DBG("15BPP Mode \n");
			ast_video->src_fbinfo.color_mode = VGA_15BPP_MODE;			
			return VGA_15BPP_MODE;
		} else if (color_mode == 3) {
			VIDEO_DBG("16BPP Mode \n");
			ast_video->src_fbinfo.color_mode = VGA_16BPP_MODE;						
			return VGA_16BPP_MODE;
		} else if (color_mode == 4) {
			VIDEO_DBG("32BPP Mode \n");
			ast_video->src_fbinfo.color_mode = VGA_32BPP_MODE;						
			return VGA_32BPP_MODE;
		} else {
			printk("TODO ... unknow ..\n");
			ast_video->src_fbinfo.color_mode = VGA_MODE;						
			return VGA_MODE;
		}
	
	}		
}

static void ast_vgavideo_set_eng_config(struct ast_vgavideo_data *ast_video, struct ast_vgavideo_config *video_config)
{
	int i, base=0;
	u32 ctrl = 0;
	u32 compress_ctrl = 0x00080000;

	VIDEO_DBG("\n");

	switch(video_config->engine) {
		case 0:
			ctrl = ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL);
			break;
		case 1:
			ctrl = ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL);
			break;
	}
		

	if(video_config->AutoMode) {
		ctrl |= VIDEO_AUTO_COMPRESS;
		
	} else {
		ctrl &= ~VIDEO_AUTO_COMPRESS;
	}

	ast_vgavideo_write(ast_video, VIDEO_COMPRESS_COMPLETE | VIDEO_CAPTURE_COMPLETE | VIDEO_MODE_DETECT_WDT, AST_VIDEO_INT_EN);

	if(video_config->compression_format) {
		ctrl |= VIDEO_COMPRESS_JPEG_MODE;	
	} else {
		ctrl &= ~VIDEO_COMPRESS_JPEG_MODE;
	}

	ctrl &= ~VIDEO_COMPRESS_FORMAT_MASK;
		
	if(video_config->YUV420_mode) {
		ctrl |= VIDEO_COMPRESS_FORMAT(YUV420);	
	} 

	switch(video_config->compression_mode) {
		case 0:	//DCT only
			compress_ctrl |= VIDEO_DCT_ONLY_ENCODE;
			break;
		case 1:	//DCT VQ mix 2-color
			compress_ctrl &= ~(VIDEO_4COLOR_VQ_ENCODE |VIDEO_DCT_ONLY_ENCODE);	
			break;
		case 2:	//DCT VQ mix 4-color
			compress_ctrl |= VIDEO_4COLOR_VQ_ENCODE;
			break;
		default:
			printk("error for compression mode~~~~\n");
			break;
	}

    if (video_config->Visual_Lossless) {
		compress_ctrl |= VIDEO_HQ_ENABLE;
		compress_ctrl |= VIDEO_HQ_DCT_LUM(video_config->AdvanceTableSelector);	
		compress_ctrl |= VIDEO_HQ_DCT_CHROM((video_config->AdvanceTableSelector + 16));		
    } else 
		compress_ctrl &= ~VIDEO_HQ_ENABLE;

	switch(video_config->engine) {
		case 0:
			ast_vgavideo_write(ast_video, ctrl, AST_VIDEO_SEQ_CTRL);
			ast_vgavideo_write(ast_video, compress_ctrl | VIDEO_DCT_LUM(video_config->Y_JPEGTableSelector) | VIDEO_DCT_CHROM(video_config->Y_JPEGTableSelector + 16), AST_VIDEO_COMPRESS_CTRL);
			break;
		case 1:			
			ast_vgavideo_write(ast_video, ctrl, AST_VM_SEQ_CTRL);
			ast_vgavideo_write(ast_video, compress_ctrl | VIDEO_DCT_LUM(video_config->Y_JPEGTableSelector) | VIDEO_DCT_CHROM(video_config->Y_JPEGTableSelector + 16), AST_VM_COMPRESS_CTRL);
			break;
	}

	if(video_config->compression_format == 1) {
		for(i = 0; i<12; i++) {
			base = (1024*i);
			if(video_config->YUV420_mode)	//yuv420
				ast_video->jpeg_tbl_virt[base + 46] = 0x00220103; //for YUV420 mode
			else 
				ast_video->jpeg_tbl_virt[base + 46] = 0x00110103; //for YUV444 mode)
		}
	}

	
}

static void ast_vgavideo_set_0_scaling(struct ast_vgavideo_data *ast_video, struct ast_scaling *scaling)
{
	u32 scan_line, v_factor, h_factor;
	u32 ctrl = ast_vgavideo_read(ast_video, AST_VIDEO_CTRL);
	//no scaling
	ctrl &= ~VIDEO_CTRL_DWN_SCALING_MASK;
	
	if(scaling->enable) {
		if((ast_video->src_fbinfo.x == scaling->x) && (ast_video->src_fbinfo.y == scaling->y)) {
			ast_vgavideo_write(ast_video, 0x00200000, AST_VIDEO_SCALING0);
			ast_vgavideo_write(ast_video, 0x00200000, AST_VIDEO_SCALING1);
			ast_vgavideo_write(ast_video, 0x00200000, AST_VIDEO_SCALING2);
			ast_vgavideo_write(ast_video, 0x00200000, AST_VIDEO_SCALING3);
			//compression x,y
			ast_vgavideo_write(ast_video, VIDEO_COMPRESS_H(ast_video->src_fbinfo.x) | VIDEO_COMPRESS_V(ast_video->src_fbinfo.y), AST_VIDEO_COMPRESS_WIN);		
			ast_vgavideo_write(ast_video, 0x10001000, AST_VIDEO_SCAL_FACTOR);						
		} else {
			//Down-Scaling
			VIDEO_DBG("Scaling Enable\n");
			//Calculate scaling factor D / S = 4096 / Factor  ======> Factor = (S / D) * 4096
			h_factor = ((ast_video->src_fbinfo.x - 1) * 4096) / (scaling->x - 1);
			if (h_factor < 4096)
				h_factor = 4096;
			if ((h_factor * (scaling->x - 1)) != (ast_video->src_fbinfo.x - 1) * 4096) 
				h_factor += 1;
			
			//Calculate scaling factor D / S = 4096 / Factor	======> Factor = (S / D) * 4096
			v_factor = ((ast_video->src_fbinfo.y - 1) * 4096) / (scaling->y - 1);
			if (v_factor < 4096)
				v_factor = 4096;
			if ((v_factor * (scaling->y - 1)) != (ast_video->src_fbinfo.y - 1) * 4096)
				v_factor += 1;
			
#ifndef AST_SOC_G5
			ctrl |= VIDEO_CTRL_DWN_SCALING_ENABLE_LINE_BUFFER; 
#endif
			if(ast_video->src_fbinfo.x <= scaling->x * 2) {
				ast_vgavideo_write(ast_video, 0x00101000, AST_VIDEO_SCALING0);
				ast_vgavideo_write(ast_video, 0x00101000, AST_VIDEO_SCALING1);
				ast_vgavideo_write(ast_video, 0x00101000, AST_VIDEO_SCALING2);
				ast_vgavideo_write(ast_video, 0x00101000, AST_VIDEO_SCALING3);
			} else {
				ast_vgavideo_write(ast_video, 0x08080808, AST_VIDEO_SCALING0);
				ast_vgavideo_write(ast_video, 0x08080808, AST_VIDEO_SCALING1);
				ast_vgavideo_write(ast_video, 0x08080808, AST_VIDEO_SCALING2);
				ast_vgavideo_write(ast_video, 0x08080808, AST_VIDEO_SCALING3);
			}
			//compression x,y
			ast_vgavideo_write(ast_video, VIDEO_COMPRESS_H(scaling->x) | VIDEO_COMPRESS_V(scaling->y), AST_VIDEO_COMPRESS_WIN);

			VIDEO_DBG("Scaling factor : v : %d , h : %d \n",v_factor, h_factor);
			ast_vgavideo_write(ast_video, VIDEO_V_SCAL_FACTOR(v_factor) | VIDEO_H_SCAL_FACTOR(h_factor), AST_VIDEO_SCAL_FACTOR);
		}
	} else {// 1:1
		VIDEO_DBG("Scaling Disable \n");
		v_factor = 4096;
		h_factor = 4096;
		ast_vgavideo_write(ast_video, 0x00200000, AST_VIDEO_SCALING0);
		ast_vgavideo_write(ast_video, 0x00200000, AST_VIDEO_SCALING1);
		ast_vgavideo_write(ast_video, 0x00200000, AST_VIDEO_SCALING2);
		ast_vgavideo_write(ast_video, 0x00200000, AST_VIDEO_SCALING3);
		//compression x,y
		ast_vgavideo_write(ast_video, VIDEO_COMPRESS_H(ast_video->src_fbinfo.x) | VIDEO_COMPRESS_V(ast_video->src_fbinfo.y), AST_VIDEO_COMPRESS_WIN);	
		
		ast_vgavideo_write(ast_video, 0x10001000, AST_VIDEO_SCAL_FACTOR);
	}
	ast_vgavideo_write(ast_video, ctrl, AST_VIDEO_CTRL);

	//capture x y 
#ifdef AST_SOC_G5 //A1 issue fix
	if(ast_video->src_fbinfo.x == 1680) {
		ast_vgavideo_write(ast_video, VIDEO_CAPTURE_H(1728) | VIDEO_CAPTURE_V(ast_video->src_fbinfo.y), AST_VIDEO_CAPTURE_WIN);		
	} else {
		ast_vgavideo_write(ast_video, VIDEO_CAPTURE_H(ast_video->src_fbinfo.x) |	VIDEO_CAPTURE_V(ast_video->src_fbinfo.y)	, AST_VIDEO_CAPTURE_WIN);	
	}
#else	
	ast_vgavideo_write(ast_video, VIDEO_CAPTURE_H(ast_video->src_fbinfo.x) | 	VIDEO_CAPTURE_V(ast_video->src_fbinfo.y), AST_VIDEO_CAPTURE_WIN);
#endif


	if((ast_video->src_fbinfo.x % 8) == 0)
		ast_vgavideo_write(ast_video, ast_video->src_fbinfo.x * 4, AST_VIDEO_SOURCE_SCAN_LINE);
	else {
		scan_line = ast_video->src_fbinfo.x;
		scan_line = scan_line + 16 - (scan_line % 16);
		scan_line = scan_line * 4;
		ast_vgavideo_write(ast_video, scan_line, AST_VIDEO_SOURCE_SCAN_LINE);
	}
	
}

static void ast_vgavideo_set_1_scaling(struct ast_vgavideo_data *ast_video, struct ast_scaling *scaling)
{
	u32 v_factor, h_factor;

	if(scaling->enable) {
		if((ast_video->src_fbinfo.x == scaling->x) && (ast_video->src_fbinfo.y == scaling->y)) {
			ast_vgavideo_write(ast_video, VIDEO_COMPRESS_H(ast_video->src_fbinfo.x) | VIDEO_COMPRESS_V(ast_video->src_fbinfo.y), AST_VM_COMPRESS_WIN);			
			ast_vgavideo_write(ast_video, 0x10001000, AST_VM_SCAL_FACTOR); 
		} else {
			//Down-Scaling
			VIDEO_DBG("Scaling Enable\n");
			//Calculate scaling factor D / S = 4096 / Factor  ======> Factor = (S / D) * 4096
			h_factor = ((ast_video->src_fbinfo.x - 1) * 4096) / (scaling->x - 1);
			if (h_factor < 4096)
				h_factor = 4096;
			if ((h_factor * (scaling->x - 1)) != (ast_video->src_fbinfo.x - 1) * 4096) 
				h_factor += 1;
			
			//Calculate scaling factor D / S = 4096 / Factor	======> Factor = (S / D) * 4096
			v_factor = ((ast_video->src_fbinfo.y - 1) * 4096) / (scaling->y - 1);
			if (v_factor < 4096)
				v_factor = 4096;
			if ((v_factor * (scaling->y - 1)) != (ast_video->src_fbinfo.y - 1) * 4096)
				v_factor += 1;
			
			//compression x,y
			ast_vgavideo_write(ast_video, VIDEO_COMPRESS_H(scaling->x) | VIDEO_COMPRESS_V(scaling->y), AST_VM_COMPRESS_WIN);
			ast_vgavideo_write(ast_video, VIDEO_V_SCAL_FACTOR(v_factor) | VIDEO_H_SCAL_FACTOR(h_factor), AST_VM_SCAL_FACTOR);
		}
	} else {// 1:1
		VIDEO_DBG("Scaling Disable \n");
		ast_vgavideo_write(ast_video, VIDEO_COMPRESS_H(ast_video->src_fbinfo.x) | VIDEO_COMPRESS_V(ast_video->src_fbinfo.y), AST_VM_COMPRESS_WIN);	
		ast_vgavideo_write(ast_video, 0x10001000, AST_VM_SCAL_FACTOR);	
	}

	//capture x y 
#ifdef AST_SOC_G5 //A1 issue fix
	if(ast_video->src_fbinfo.x == 1680) {
		ast_vgavideo_write(ast_video, VIDEO_CAPTURE_H(1728) | VIDEO_CAPTURE_V(ast_video->src_fbinfo.y), AST_VM_CAPTURE_WIN);		
	} else {
		ast_vgavideo_write(ast_video, VIDEO_CAPTURE_H(ast_video->src_fbinfo.x) | VIDEO_CAPTURE_V(ast_video->src_fbinfo.y), AST_VM_CAPTURE_WIN);	
	}
#else	
	ast_vgavideo_write(ast_video, VIDEO_CAPTURE_H(ast_video->src_fbinfo.x) | VIDEO_CAPTURE_V(ast_video->src_fbinfo.y)	, AST_VM_CAPTURE_WIN);
#endif

	
}

static void ast_vgavideo_mode_detect_trigger(struct ast_vgavideo_data *ast_video)
{
	VIDEO_DBG("\n");

	if(!(ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) & VIDEO_CAPTURE_BUSY)) {
		printk("ERROR ~~ Capture Eng busy !! 0x04 : %x \n", ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL));

	}

	init_completion(&ast_video->mode_detect_complete);

	ast_vgavideo_write(ast_video, VIDEO_MODE_DETECT_RDY, AST_VIDEO_INT_EN);
	
	ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) & ~(VIDEO_DETECT_TRIGGER | VIDEO_INPUT_MODE_CHG_WDT), AST_VIDEO_SEQ_CTRL);
	
	ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) | VIDEO_DETECT_TRIGGER, AST_VIDEO_SEQ_CTRL);			

	wait_for_completion_interruptible(&ast_video->mode_detect_complete);
	
	ast_vgavideo_write(ast_video, 0, AST_VIDEO_INT_EN);	

}

static void ast_vgavideo_vga_mode_detect(struct ast_vgavideo_data *ast_video, struct ast_mode_detection *mode_detect)
{
	u32 H_Start, H_End, V_Start, V_End;
	u32 H_Temp = 0, V_Temp = 0, RefreshRateIndex, ColorDepthIndex;
	u32 VGA_Scratch_Register_350, VGA_Scratch_Register_354,VGA_Scratch_Register_34C, Color_Depth, Mode_Clock;
	u8 Direct_Mode;

	VIDEO_DBG("\n");

	//set input signal  and Check polarity (video engine prefers negative signal)		
	ast_vgavideo_write(ast_video, (ast_vgavideo_read(ast_video, AST_VIDEO_PASS_CTRL) &
					~(VIDEO_DIRT_FATCH | VIDEO_EXT_ADC_ATTRIBUTE)) |
					VIDEO_INTERNAL_DE | 
					VIDEO_SO_VSYNC_POLARITY | VIDEO_SO_HSYNC_POLARITY,
					AST_VIDEO_PASS_CTRL);

	ast_vgavideo_mode_detect_trigger(ast_video);

	//Enable Watchdog detection
	ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) | VIDEO_INPUT_MODE_CHG_WDT, AST_VIDEO_SEQ_CTRL);

Redo:
	//for store lock
	ast_vgavideo_mode_detect_trigger(ast_video);

	H_Start = VIDEO_GET_HSYNC_LEFT(ast_vgavideo_read(ast_video, AST_VIDEO_H_DETECT_STS));
	H_End = VIDEO_GET_HSYNC_RIGHT(ast_vgavideo_read(ast_video, AST_VIDEO_H_DETECT_STS));

	V_Start = VIDEO_GET_VSYNC_TOP(ast_vgavideo_read(ast_video, AST_VIDEO_V_DETECT_STS));
	V_End = VIDEO_GET_VSYNC_BOTTOM(ast_vgavideo_read(ast_video, AST_VIDEO_V_DETECT_STS));


	//Check if cable quality is too bad. If it is bad then we use 0x65 as threshold
	//Because RGB data is arrived slower than H-sync, V-sync. We have to read more times to confirm RGB data is arrived
	if ((abs(H_Temp - H_Start) > 1) || ((H_Start <= 1) || (V_Start <= 1) || (H_Start == 0xFFF) || (V_Start == 0xFFF))) {
			H_Temp = VIDEO_GET_HSYNC_LEFT(ast_vgavideo_read(ast_video, AST_VIDEO_H_DETECT_STS));
			V_Temp = VIDEO_GET_VSYNC_TOP(ast_vgavideo_read(ast_video, AST_VIDEO_V_DETECT_STS));
			goto Redo;
	}

//	VIDEO_DBG("H S: %d, E: %d, V S: %d, E: %d \n", H_Start, H_End, V_Start, V_End);

	ast_vgavideo_write(ast_video, VIDEO_HSYNC_PIXEL_FIRST_SET(H_Start - 1) | VIDEO_HSYNC_PIXEL_LAST_SET(H_End), AST_VIDEO_TIMING_H);
	ast_vgavideo_write(ast_video, VIDEO_VSYNC_PIXEL_FIRST_SET(V_Start) | VIDEO_VSYNC_PIXEL_LAST_SET(V_End + 1), AST_VIDEO_TIMING_V);

	ast_video->src_fbinfo.x = (H_End - H_Start) + 1;
	ast_video->src_fbinfo.y = (V_End - V_Start) + 1;

	VIDEO_DBG("screen mode x:%d, y:%d \n", ast_video->src_fbinfo.x, ast_video->src_fbinfo.y);
	
	mode_detect->src_x = ast_video->src_fbinfo.x;
	mode_detect->src_y = ast_video->src_fbinfo.y;

	VGA_Scratch_Register_350 = ast_vgavideo_read(ast_video, AST_VIDEO_SCRATCH_350);
	VGA_Scratch_Register_34C = ast_vgavideo_read(ast_video, AST_VIDEO_SCRATCH_34C);
	VGA_Scratch_Register_354 = ast_vgavideo_read(ast_video, AST_VIDEO_SCRATCH_354);

	ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_PASS_CTRL) &
					~(VIDEO_SO_VSYNC_POLARITY | VIDEO_SO_HSYNC_POLARITY),
					AST_VIDEO_PASS_CTRL);


	if (((VGA_Scratch_Register_350 & 0xff00) >> 8) == 0xA8) {
		//Driver supports to write display information in scratch register
//		printk("Wide Screen Information \n");
		/*
		Index 0x94: (VIDEO:1E70:0354)
		D[7:0]: HDE D[7:0]
		Index 0x95: (VIDEO:1E70:0355)
		D[7:0]: HDE D[15:8]
		Index 0x96: (VIDEO:1E70:0356)
		D[7:0]: VDE D[7:0]
		Index 0x97: (VIDEO:1E70:0357)
		D[7:0]: VDE D[15:8] 
		*/

		Color_Depth = ((VGA_Scratch_Register_350 & 0xff0000) >> 16); //VGA's Color Depth is 0 when real color depth is less than 8
		Mode_Clock = ((VGA_Scratch_Register_350 & 0xff000000) >> 24);
		if (Color_Depth < 15) {
//			printk("Color Depth is not 16bpp or higher\n");
			Direct_Mode = 0;
		} else {
//			printk("Color Depth is 16bpp or higher\n");
			Direct_Mode = 1;
		}
	} else { //Original mode information
		//Judge if bandwidth is not enough then enable direct mode in internal VGA
		/* Index 0x8E: (VIDEO:1E70:034E)
		Mode ID Resolution Notes
		0x2E 640x480
		0x30 800x600
		0x31 1024x768
		0x32 1280x1024
		0x33 1600x1200
		0x34 1920x1200
		0x35 1280x800
		0x36 1440x900
		0x37 1680x1050
		0x38 1920x1080
		0x39 1366x768
		0x3A 1600x900
		0x3B 1152x864
		0x50 320x240
		0x51 400x300
		0x52 512x384
		0x6A 800x600		
		*/
		
		RefreshRateIndex = (VGA_Scratch_Register_34C >> 8) & 0x0F;
		ColorDepthIndex = (VGA_Scratch_Register_34C >> 4) & 0x0F;
//		printk("Orignal mode information \n");
		if((ColorDepthIndex == 0xe) || (ColorDepthIndex == 0xf)) {
			Direct_Mode = 0;
		} else {
			if(ColorDepthIndex > 2) {
				if((ast_video->src_fbinfo.x * ast_video->src_fbinfo.y) > (1024 * 768))
						Direct_Mode = 1;
				else
						Direct_Mode = 0;
			} else {
				Direct_Mode = 0;
			}
		}
	}

	if(Direct_Mode) {
		ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_PASS_CTRL) | VIDEO_DIRT_FATCH | VIDEO_AUTO_FATCH, AST_VIDEO_PASS_CTRL); 		
		
		ast_vgavideo_write(ast_video, ast_video->vga_base, AST_VIDEO_DIRECT_BASE);	
		
		ast_vgavideo_write(ast_video, VIDEO_FETCH_TIMING(0) | VIDEO_FETCH_LINE_OFFSET(ast_video->src_fbinfo.x * 4)	, AST_VIDEO_DIRECT_CTRL);		
		
	} else {
		ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_PASS_CTRL) & ~VIDEO_DIRT_FATCH, AST_VIDEO_PASS_CTRL); 
	}

	//should enable WDT detection every after mode detection 
	ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) | VIDEO_INPUT_MODE_CHG_WDT, AST_VIDEO_SEQ_CTRL);

}

/*return compression size */
static void ast_vgavideo_auto_mode_trigger(struct ast_vgavideo_data *ast_video, struct ast_auto_mode *auto_mode)
{
	int timeout = 0;
	
	VIDEO_DBG("\n");

	if(ast_video->mode_change) {
		auto_mode->mode_change = ast_video->mode_change;
		ast_video->mode_change = 0;
		return;
	}

	switch(auto_mode->engine_idx) {
		case 0:
			init_completion(&ast_video->automode_complete);

			if(auto_mode->differential) 
				ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_BCD_CTRL) | VIDEO_BCD_CHG_EN, AST_VIDEO_BCD_CTRL);
			else
				ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_BCD_CTRL) & ~VIDEO_BCD_CHG_EN, AST_VIDEO_BCD_CTRL);

			ast_vgavideo_write(ast_video, (ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) & ~(VIDEO_CAPTURE_TRIGGER | VIDEO_COMPRESS_FORCE_IDLE | VIDEO_COMPRESS_TRIGGER)) | VIDEO_AUTO_COMPRESS, AST_VIDEO_SEQ_CTRL);
			//If CPU is too fast, pleas read back and trigger 
			ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) | VIDEO_COMPRESS_TRIGGER | VIDEO_CAPTURE_TRIGGER, AST_VIDEO_SEQ_CTRL);
			
			timeout = wait_for_completion_interruptible_timeout(&ast_video->automode_complete, HZ/2);
			
			if (timeout == 0) { 
				printk("compression timeout sts %x \n", ast_vgavideo_read(ast_video, AST_VIDEO_INT_STS));
				auto_mode->total_size = 0;
				auto_mode->block_count = 0;
			} else {
				auto_mode->total_size = ast_vgavideo_read(ast_video, AST_VIDEO_COMPRESS_DATA_COUNT);
				auto_mode->block_count = ast_vgavideo_read(ast_video, AST_VIDEO_COMPRESS_BLOCK_COUNT) >> 16;
			}
			
			break;
		case 1:
//			init_completion(&ast_video->automode_vm_complete);
			if(auto_mode->differential) {
				ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VM_BCD_CTRL) | VIDEO_BCD_CHG_EN, AST_VM_BCD_CTRL);
			} else {
				ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VM_BCD_CTRL) & ~VIDEO_BCD_CHG_EN, AST_VM_BCD_CTRL);
			}
			ast_vgavideo_write(ast_video, (ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL) & ~(VIDEO_CAPTURE_TRIGGER | VIDEO_COMPRESS_TRIGGER)) | VIDEO_AUTO_COMPRESS , AST_VM_SEQ_CTRL);

			ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL) | VIDEO_CAPTURE_TRIGGER | VIDEO_COMPRESS_TRIGGER, AST_VM_SEQ_CTRL);
			udelay(10);
//AST_G5 Issue in isr bit 19, so use polling mode for wait engine idle

#if 1		
			timeout = 0;
			while(1) {
				timeout++; 				
				if((ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL) & 0x50000) == 0x50000) 
					break;

				mdelay(1);	
				if(timeout>100)
					break;
			}

			if (timeout>=100) {
				printk("Engine hang time out \n");
				auto_mode->total_size = 0;
				auto_mode->block_count = 0;

			} else {
				auto_mode->total_size = ast_vgavideo_read(ast_video, AST_VM_COMPRESS_FRAME_END);
				auto_mode->block_count = ast_vgavideo_read(ast_video, AST_VM_COMPRESS_BLOCK_COUNT);
			}

//			printk("0 isr %x \n", ast_vgavideo_read(ast_video, AST_VIDEO_INT_STS));
			//must clear it 
			ast_vgavideo_write(ast_video, (ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL) & ~(VIDEO_CAPTURE_TRIGGER | VIDEO_COMPRESS_TRIGGER)) , AST_VM_SEQ_CTRL);
//			printk("1 isr %x \n", ast_vgavideo_read(ast_video, AST_VIDEO_INT_STS));
#else
			timeout = wait_for_completion_interruptible_timeout(&ast_video->automode_vm_complete, 10*HZ);
			
			if (timeout == 0) { 
				printk("compression timeout sts %x \n", ast_vgavideo_read(ast_video, AST_VIDEO_INT_STS));
				return 0;
			} else {
				printk("%x size = %x \n", ast_vgavideo_read(ast_video, 0x270), ast_vgavideo_read(ast_video, AST_VM_COMPRESS_FRAME_END));
				return ast_vgavideo_read(ast_video, AST_VM_COMPRESS_FRAME_END);
			}
#endif			
			break;
	}

	if(ast_video->mode_change) {
		auto_mode->mode_change = ast_video->mode_change;
		ast_video->mode_change = 0;
	}

}

static void ast_vgavideo_mode_detect_info(struct ast_vgavideo_data *ast_video)

{
	u32 H_Start, H_End, V_Start, V_End;

	H_Start = VIDEO_GET_HSYNC_LEFT(ast_vgavideo_read(ast_video, AST_VIDEO_H_DETECT_STS));
	H_End = VIDEO_GET_HSYNC_RIGHT(ast_vgavideo_read(ast_video, AST_VIDEO_H_DETECT_STS));
	
	V_Start = VIDEO_GET_VSYNC_TOP(ast_vgavideo_read(ast_video, AST_VIDEO_V_DETECT_STS));	
	V_End = VIDEO_GET_VSYNC_BOTTOM(ast_vgavideo_read(ast_video, AST_VIDEO_V_DETECT_STS)); 	

	VIDEO_DBG("Get H_Start = %d, H_End = %d, V_Start = %d, V_End = %d\n", H_Start, H_End, V_Start, V_End);

	ast_video->src_fbinfo.x = (H_End - H_Start) + 1;
	ast_video->src_fbinfo.y = (V_End - V_Start) + 1;
	VIDEO_DBG("source : x = %d, y = %d , color mode = %x \n", ast_video->src_fbinfo.x, ast_video->src_fbinfo.y,ast_video->src_fbinfo.color_mode);
}


static irqreturn_t ast_vgavideo_isr(int this_irq, void *dev_id)
{
	u32 status;
	u32 swap0, swap1; 
	struct ast_vgavideo_data *ast_video = dev_id;

	status = ast_vgavideo_read(ast_video, AST_VIDEO_INT_STS);

	VIDEO_DBG("%x \n", status);

	if(status & VIDEO_MODE_DETECT_RDY) {
		ast_vgavideo_write(ast_video, VIDEO_MODE_DETECT_RDY, AST_VIDEO_INT_STS);
		complete(&ast_video->mode_detect_complete);
	}

	if(status & VIDEO_MODE_DETECT_WDT) {
		ast_video->mode_change = 1;
		VIDEO_DBG("change 1\n");
		ast_vgavideo_write(ast_video, VIDEO_MODE_DETECT_WDT, AST_VIDEO_INT_STS);
	}

	if(ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) & VIDEO_AUTO_COMPRESS) {
		if((status & (VIDEO_COMPRESS_COMPLETE | VIDEO_CAPTURE_COMPLETE)) ==  (VIDEO_COMPRESS_COMPLETE | VIDEO_CAPTURE_COMPLETE)) {
			ast_vgavideo_write(ast_video, VIDEO_COMPRESS_COMPLETE | VIDEO_CAPTURE_COMPLETE, AST_VIDEO_INT_STS);
			swap0 = ast_vgavideo_read(ast_video, AST_VIDEO_SOURCE_BUFF0);
			swap1 = ast_vgavideo_read(ast_video, AST_VIDEO_SOURCE_BUFF1);
			ast_vgavideo_write(ast_video, swap1, AST_VIDEO_SOURCE_BUFF0);
			ast_vgavideo_write(ast_video, swap0, AST_VIDEO_SOURCE_BUFF1);
			VIDEO_DBG("auto mode complete \n");
			complete(&ast_video->automode_complete);
		}	
	} else {
		if (status & VIDEO_COMPRESS_COMPLETE) {
			ast_vgavideo_write(ast_video, VIDEO_COMPRESS_COMPLETE, AST_VIDEO_INT_STS);
			VIDEO_DBG("compress complete \n");		
			complete(&ast_video->compression_complete);
		}
		if (status & VIDEO_CAPTURE_COMPLETE) {
			ast_vgavideo_write(ast_video, VIDEO_CAPTURE_COMPLETE, AST_VIDEO_INT_STS);
			VIDEO_DBG("capture complete \n");				
			swap0 = ast_vgavideo_read(ast_video, AST_VIDEO_SOURCE_BUFF0);
			swap1 = ast_vgavideo_read(ast_video, AST_VIDEO_SOURCE_BUFF1);
			ast_vgavideo_write(ast_video, swap1, AST_VIDEO_SOURCE_BUFF0);
			ast_vgavideo_write(ast_video, swap0, AST_VIDEO_SOURCE_BUFF1);			
			complete(&ast_video->capture_complete);
		}
	}

    return IRQ_HANDLED;
}

static void ast_vgavideo_ctrl_init(struct ast_vgavideo_data *ast_video)
{
	VIDEO_DBG("\n");

	ast_vgavideo_write(ast_video, (u32)ast_video->buff0_phy, AST_VIDEO_SOURCE_BUFF0);
	ast_vgavideo_write(ast_video, (u32)ast_video->buff1_phy, AST_VIDEO_SOURCE_BUFF1);
	ast_vgavideo_write(ast_video, (u32)ast_video->bcd_phy, AST_VIDEO_BCD_BUFF);
	ast_vgavideo_write(ast_video, (u32)ast_video->stream_phy, AST_VIDEO_STREAM_BUFF);
	ast_vgavideo_write(ast_video, (u32)ast_video->jpeg_tbl_phy, AST_VIDEO_JPEG_HEADER_BUFF);
	ast_vgavideo_write(ast_video, (u32)ast_video->jpeg_tbl_phy, AST_VM_JPEG_HEADER_BUFF);		
	ast_vgavideo_write(ast_video, (u32)ast_video->jpeg_buf0_phy, AST_VM_SOURCE_BUFF0);
	ast_vgavideo_write(ast_video, (u32)ast_video->jpeg_phy, AST_VM_COMPRESS_BUFF);
	ast_vgavideo_write(ast_video, 0, AST_VIDEO_COMPRESS_READ);

	//clr int sts
	ast_vgavideo_write(ast_video, 0xffffffff, AST_VIDEO_INT_STS);
	ast_vgavideo_write(ast_video, 0, AST_VIDEO_BCD_CTRL);

	// =============================  JPEG init ===========================================
	ast_init_jpeg_table(ast_video);
	ast_vgavideo_write(ast_video,  VM_STREAM_PKT_SIZE(STREAM_3MB), AST_VM_STREAM_SIZE);
	ast_vgavideo_write(ast_video,  0x00080000 | VIDEO_DCT_LUM(4) | VIDEO_DCT_CHROM(4 + 16) | VIDEO_DCT_ONLY_ENCODE, AST_VM_COMPRESS_CTRL);

	//WriteMMIOLong(0x1e700238, 0x00000000);
	//WriteMMIOLong(0x1e70023c, 0x00000000);

	ast_vgavideo_write(ast_video, 0x00001E00, AST_VM_SOURCE_SCAN_LINE); //buffer pitch
	ast_vgavideo_write(ast_video, 0x00000000, 0x268);
	ast_vgavideo_write(ast_video, 0x00001234, 0x280);

	ast_vgavideo_write(ast_video, 0x00000000, AST_VM_PASS_CTRL);
	ast_vgavideo_write(ast_video, 0x00000000, AST_VM_BCD_CTRL);

	// ===============================================================================


	//Specification define bit 12:13 must always 0;
	ast_vgavideo_write(ast_video, (ast_vgavideo_read(ast_video, AST_VIDEO_PASS_CTRL) & 
				~(VIDEO_DUAL_EDGE_MODE | VIDEO_18BIT_SINGLE_EDGE)) |
				VIDEO_DVO_INPUT_DELAY(0x4), 
				AST_VIDEO_PASS_CTRL); 

	ast_vgavideo_write(ast_video, VIDEO_STREAM_PKT_N(STREAM_32_PKTS) | 
				VIDEO_STREAM_PKT_SIZE(STREAM_128KB), AST_VIDEO_STREAM_SIZE);


	//rc4 init reset ..
	ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_CTRL) | VIDEO_CTRL_RC4_RST , AST_VIDEO_CTRL);
	ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_CTRL) & ~VIDEO_CTRL_RC4_RST , AST_VIDEO_CTRL);

	//CRC/REDUCE_BIT register clear
	ast_vgavideo_write(ast_video, 0, AST_VIDEO_CRC1);
	ast_vgavideo_write(ast_video, 0, AST_VIDEO_CRC2);
	ast_vgavideo_write(ast_video, 0, AST_VIDEO_DATA_TRUNCA);
	ast_vgavideo_write(ast_video, 0, AST_VIDEO_COMPRESS_READ);

	ast_vgavideo_write(ast_video, (ast_vgavideo_read(ast_video, AST_VIDEO_MODE_DETECT) & 0xff) |
									VIDEO_MODE_HOR_TOLER(6) |
									VIDEO_MODE_VER_TOLER(6) |
									VIDEO_MODE_HOR_STABLE(2) |
									VIDEO_MODE_VER_STABLE(2) |
									VIDEO_MODE_EDG_THROD(0x65)
									, AST_VIDEO_MODE_DETECT);	
}

static long ast_vgavideo_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	int ret = 1;
	struct miscdevice *c = fp->private_data;
	struct ast_vgavideo_data *ast_video = dev_get_drvdata(c->this_device);
	struct ast_scaling set_scaling;
	struct ast_vgavideo_config video_config;
	
	int vga_enable = 0;
	struct ast_mode_detection mode_detection;	
	struct ast_auto_mode auto_mode;	
	void __user *argp = (void __user *)arg;


	switch (cmd) {
		case AST_VIDEO_IOC_GET_VGA_SIGNAL:
			ret = put_user(ast_get_vga_signal(ast_video), (unsigned long __user *)arg);
			break;
		case AST_VIDEO_GET_MEM_SIZE_IOCRX:
			ret = __put_user(ast_video->video_mem_size, (unsigned long __user *)arg);
			break;
		case AST_VIDEO_GET_JPEG_OFFSET_IOCRX:
			ret = __put_user(ast_video->video_jpeg_offset, (unsigned long __user *)arg);
			break;
		case AST_VIDEO_VGA_MODE_DETECTION:
			ret = copy_from_user(&mode_detection, argp, sizeof(struct ast_mode_detection));
			ast_vgavideo_vga_mode_detect(ast_video, &mode_detection);
			ret = copy_to_user(argp, &mode_detection, sizeof(struct ast_mode_detection));
			break;	
		case AST_VIDEO_ENG_CONFIG:
			ret = copy_from_user(&video_config, argp, sizeof(struct ast_vgavideo_config));

			ast_vgavideo_set_eng_config(ast_video, &video_config);
			break;
		case AST_VIDEO_SET_SCALING:
			ret = copy_from_user(&set_scaling, argp, sizeof(struct ast_scaling));
			switch(set_scaling.engine) {
				case 0:
					ast_vgavideo_set_0_scaling(ast_video, &set_scaling);
					break;
				case 1:
					ast_vgavideo_set_1_scaling(ast_video, &set_scaling);
					break;
			}
			break;
		case AST_VIDEO_AUTOMODE_TRIGGER:
			ret = copy_from_user(&auto_mode, argp, sizeof(struct ast_auto_mode));
			ast_vgavideo_auto_mode_trigger(ast_video, &auto_mode);
			ret = copy_to_user(argp, &auto_mode, sizeof(struct ast_auto_mode));
			break;
		case AST_VIDEO_CAPTURE_TRIGGER:
			break;
		case AST_VIDEO_COMPRESSION_TRIGGER:
			break;
		case AST_VIDEO_SET_VGA_DISPLAY:
			ret = __get_user(vga_enable, (int __user *)arg);
			ast_scu_set_vga_display(vga_enable);
			break;
		default:
			ret = 3;
			break;
	}
	return ret;

}

/** @note munmap handler is done by vma close handler */
static int ast_vgavideo_mmap(struct file * file, struct vm_area_struct * vma)
{
        struct miscdevice *c = file->private_data;
        struct ast_vgavideo_data *ast_video = dev_get_drvdata(c->this_device);
        size_t size = vma->vm_end - vma->vm_start;
        vma->vm_private_data = ast_video;

        if (PAGE_ALIGN(size) > ast_video->video_mem_size) {
                        printk(KERN_ERR "required length exceed the size "
                                   "of physical sram (%x)\n", ast_video->video_mem_size);
                        return -EAGAIN;
        }

        if ((ast_video->stream_phy + (vma->vm_pgoff << PAGE_SHIFT) + size)
                > (ast_video->stream_phy + ast_video->video_mem_size)) {
                        printk(KERN_ERR "required sram range exceed the size "
                                   "of phisical sram\n");
                        return -EAGAIN;
        }

        vma->vm_flags |= VM_IO;
        vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

        if (io_remap_pfn_range(vma, vma->vm_start,
                        ((u32)ast_video->stream_phy >> PAGE_SHIFT),
                        size,
                        vma->vm_page_prot)) {
                printk(KERN_ERR "remap_pfn_range faile at %s()\n", __func__);
                return -EAGAIN;
        }

        return 0;
}

static int ast_vgavideo_open(struct inode *inode, struct file *file)
{
        struct miscdevice *c = file->private_data;
        struct ast_vgavideo_data *ast_video = dev_get_drvdata(c->this_device);

        VIDEO_DBG("\n");

        ast_video->is_open = true;

        return 0;

}

static int ast_vgavideo_release(struct inode *inode, struct file *file)
{
        struct miscdevice *c = file->private_data;
        struct ast_vgavideo_data *ast_video = dev_get_drvdata(c->this_device);

        VIDEO_DBG("\n");

        ast_video->is_open = false;
        return 0;
}

static const struct file_operations ast_vgavideo_fops = {
	.owner 			= THIS_MODULE,
	.llseek 			= no_llseek,
	.unlocked_ioctl 	= ast_vgavideo_ioctl,
	.open 			= ast_vgavideo_open,
	.release 			= ast_vgavideo_release,
	.mmap 			= ast_vgavideo_mmap,
};

struct miscdevice ast_vgavideo_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ast-vgavideo",
	.fops = &ast_vgavideo_fops,
};

/************************************************** SYS FS **************************************************************/
static ssize_t show_vga_display(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d: %s\n", ast_scu_get_vga_display(), ast_scu_get_vga_display()? "Enable":"Disable");
}

static ssize_t store_vga_display(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	u32 val;	

	val = simple_strtoul(buf, NULL, 10);

	if(val)
		ast_scu_set_vga_display(1);
	else
		ast_scu_set_vga_display(0);
	
	return count;
}

static DEVICE_ATTR(vga_display, S_IRUGO | S_IWUSR, show_vga_display, store_vga_display); 

static ssize_t store_video_reset(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	u32 val;	
	struct ast_vgavideo_data *ast_video = dev_get_drvdata(dev);

	val = simple_strtoul(buf, NULL, 10);

	if(val) {
		ast_scu_reset_video();
		//rc4 init reset ..
		ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_CTRL) | VIDEO_CTRL_RC4_RST , AST_VIDEO_CTRL);
		ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_CTRL) & ~VIDEO_CTRL_RC4_RST , AST_VIDEO_CTRL);
		ast_vgavideo_ctrl_init(ast_video);
	}
	
	return count;
}

static DEVICE_ATTR(video_reset, S_IRUGO | S_IWUSR, NULL, store_video_reset); 

static ssize_t show_video_mode_detect(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int ret = 0;
	struct ast_vgavideo_data *ast_video = dev_get_drvdata(dev);

	if (ret < 0)
		return ret;

	ast_vgavideo_mode_detect_info(ast_video);

	return sprintf(buf, "%i\n", ret);
}

static ssize_t store_video_mode_detect(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	u32 val;	
	struct ast_vgavideo_data *ast_video = dev_get_drvdata(dev);

	val = simple_strtoul(buf, NULL, 10);

	if(val)
		ast_vgavideo_mode_detect_trigger(ast_video);
	
	return count;
}

static DEVICE_ATTR(video_mode_detect, S_IRUGO | S_IWUSR, show_video_mode_detect, store_video_mode_detect); 

static struct attribute *ast_vgavideo_attributes[] = {
	&dev_attr_vga_display.attr,
	&dev_attr_video_reset.attr,
	&dev_attr_video_mode_detect.attr,
#if 0	
	&dev_attr_video_jpeg_enc.dev_attr.attr,
	&dev_attr_video_src_x.dev_attr.attr,
	&dev_attr_video_src_y.dev_attr.attr,
	&dev_attr_video_scaling_en.dev_attr.attr,
	&dev_attr_video_dwn_x.dev_attr.attr,
	&dev_attr_video_dwn_y.dev_attr.attr,
	&dev_attr_video_rc4_en.dev_attr.attr,
	&dev_attr_video_rc4_key.dev_attr.attr,
#endif	
	NULL
};

static const struct attribute_group video_attribute_group = {
	.attrs = ast_vgavideo_attributes
};

/**************************   Vudeo SYSFS  **********************************************************/
enum ast_vgavideo_trigger_mode {
	VIDEO_CAPTURE_MODE = 0,
	VIDEO_COMPRESSION_MODE,
	VIDEO_BUFFER_MODE,
};

static u8 ast_get_trigger_mode(struct ast_vgavideo_data *ast_video, u8 eng_idx) 
{
	//VR0004[3:5] 00:capture/compression/buffer
	u32 mode=0;
	switch(eng_idx) {
		case 0:
			mode = ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) & (VIDEO_CAPTURE_MULTI_FRAME | VIDEO_AUTO_COMPRESS);
			if(mode == 0) {
				return VIDEO_CAPTURE_MODE;
			} else if(mode == VIDEO_AUTO_COMPRESS) {
				return VIDEO_COMPRESSION_MODE;
			} else if(mode == (VIDEO_CAPTURE_MULTI_FRAME | VIDEO_AUTO_COMPRESS)) {
				return VIDEO_BUFFER_MODE;
			} else {
				printk("ERROR Mode \n");
			}
		case 1:
			mode = ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL) & (VIDEO_CAPTURE_MULTI_FRAME | VIDEO_AUTO_COMPRESS);
			if(mode == 0) {
				return VIDEO_CAPTURE_MODE;
			} else if(mode == VIDEO_AUTO_COMPRESS) {
				return VIDEO_COMPRESSION_MODE;
			} else if(mode == (VIDEO_CAPTURE_MULTI_FRAME | VIDEO_AUTO_COMPRESS)) {
				return VIDEO_BUFFER_MODE;
			} else {
				printk("ERROR Mode \n");
			}
			break;
	}
	
	return mode;
}

static void ast_set_trigger_mode(struct ast_vgavideo_data *ast_video, u8 eng_idx, u8 mode) 
{
	//VR0004[3:5] 00/01/11:capture/frame/stream
	switch(eng_idx) {
		case 0:	//video 1 
			if(mode == VIDEO_CAPTURE_MODE) {
				ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) & ~(VIDEO_CAPTURE_MULTI_FRAME | VIDEO_AUTO_COMPRESS), AST_VIDEO_SEQ_CTRL);
			} else if (mode == VIDEO_COMPRESSION_MODE) {
				ast_vgavideo_write(ast_video, (ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) | VIDEO_AUTO_COMPRESS) & ~(VIDEO_CAPTURE_MULTI_FRAME) , AST_VIDEO_SEQ_CTRL);
			} else if (mode == VIDEO_BUFFER_MODE) {
				ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) | VIDEO_CAPTURE_MULTI_FRAME | VIDEO_AUTO_COMPRESS ,AST_VIDEO_SEQ_CTRL);	
			} else {
				printk("ERROR Mode \n");
			}
			break;
		case 1:	//video M
			if(mode == VIDEO_CAPTURE_MODE) {
				ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL) & ~(VIDEO_CAPTURE_MULTI_FRAME | VIDEO_AUTO_COMPRESS), AST_VM_SEQ_CTRL);
			} else if (mode == VIDEO_COMPRESSION_MODE) {
				ast_vgavideo_write(ast_video, (ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL) | VIDEO_AUTO_COMPRESS) & ~(VIDEO_CAPTURE_MULTI_FRAME) , AST_VM_SEQ_CTRL);
			} else if (mode == VIDEO_BUFFER_MODE) {
				ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL) | VIDEO_CAPTURE_MULTI_FRAME | VIDEO_AUTO_COMPRESS ,AST_VM_SEQ_CTRL);	
			} else {
				printk("ERROR Mode \n");
			}
			break;
	}
}

static u8 ast_get_compress_yuv_mode(struct ast_vgavideo_data *ast_video, u8 eng_idx) 
{
	switch(eng_idx) {
		case 0:
			return VIDEO_GET_COMPRESS_FORMAT(ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL));
			break;
		case 1:
			return VIDEO_GET_COMPRESS_FORMAT(ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL));
			break;
	}
	return 0;
}

static void ast_set_compress_yuv_mode(struct ast_vgavideo_data *ast_video, u8 eng_idx, u8 yuv_mode) 
{
	int i, base=0;

	switch(eng_idx) {
		case 0:	//video 1 
			if(yuv_mode) 	//YUV420
				ast_vgavideo_write(ast_video, (ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) & ~VIDEO_COMPRESS_FORMAT_MASK) | VIDEO_COMPRESS_FORMAT(YUV420) , AST_VIDEO_SEQ_CTRL);
			else
				ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) & ~VIDEO_COMPRESS_FORMAT_MASK , AST_VIDEO_SEQ_CTRL);
			break;
		case 1:	//video M
			if(yuv_mode) 	//YUV420
				ast_vgavideo_write(ast_video, (ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL) & ~VIDEO_COMPRESS_FORMAT_MASK) | VIDEO_COMPRESS_FORMAT(YUV420) , AST_VM_SEQ_CTRL);
			else
				ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL) & ~VIDEO_COMPRESS_FORMAT_MASK, AST_VM_SEQ_CTRL);

			for(i = 0; i<12; i++) {
				base = (1024*i);
				if(yuv_mode)	//yuv420
					ast_video->jpeg_tbl_virt[base + 46] = 0x00220103; //for YUV420 mode
				else 
					ast_video->jpeg_tbl_virt[base + 46] = 0x00110103; //for YUV444 mode)
			}
			
			break;
	}
}

static u8 ast_get_compress_jpeg_mode(struct ast_vgavideo_data *ast_video, u8 eng_idx) 
{
	switch(eng_idx) {
		case 0:
			if(ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) & VIDEO_COMPRESS_JPEG_MODE)
				return 1;
			else
				return 0;
			break;
		case 1:
			if(ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL) & VIDEO_COMPRESS_JPEG_MODE)
				return 1;
			else
				return 0;
			break;
	}	
	return 0;
}

static void ast_set_compress_jpeg_mode(struct ast_vgavideo_data *ast_video, u8 eng_idx, u8 jpeg_mode) 
{
	switch(eng_idx) {
		case 0:	//video 1 
			if(jpeg_mode) 	
				ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) | VIDEO_COMPRESS_JPEG_MODE, AST_VIDEO_SEQ_CTRL);
			else
				ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VIDEO_SEQ_CTRL) & ~VIDEO_COMPRESS_JPEG_MODE , AST_VIDEO_SEQ_CTRL);
			break;
		case 1:	//video M
			if(jpeg_mode) 	
				ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL) | VIDEO_COMPRESS_JPEG_MODE, AST_VM_SEQ_CTRL);
			else
				ast_vgavideo_write(ast_video, ast_vgavideo_read(ast_video, AST_VM_SEQ_CTRL) & ~VIDEO_COMPRESS_JPEG_MODE , AST_VM_SEQ_CTRL);			
			break;
	}
}

static ssize_t 
ast_store_compress(struct device *dev, struct device_attribute *attr, const char *sysfsbuf, size_t count)
{
	u32 input_val;
	struct ast_vgavideo_data *ast_video = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sensor_attr = to_sensor_dev_attr_2(attr);

	input_val = simple_strtoul(sysfsbuf, NULL, 10);
//	input_val = StrToHex(sysfsbuf);		
	//sensor_attr->index : ch#
	//sensor_attr->nr : attr#
	switch(sensor_attr->nr) 
	{
		case 0:	//compress mode
			ast_set_trigger_mode(ast_video, sensor_attr->index, input_val);
			break;
		case 1: //yuv mode
			ast_set_compress_yuv_mode(ast_video, sensor_attr->index, input_val);
			break;
		case 2: //jpeg/aspeed mode
			ast_set_compress_jpeg_mode(ast_video, sensor_attr->index, input_val);
			break;
		default:
			return -EINVAL;
			break;
	}

	return count;
}

static ssize_t 
ast_show_compress(struct device *dev, struct device_attribute *attr, char *sysfsbuf)
{
	struct ast_vgavideo_data *ast_video = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sensor_attr = to_sensor_dev_attr_2(attr);

	//sensor_attr->index : ch#
	//sensor_attr->nr : attr#
	switch(sensor_attr->nr) 
	{
		case 0:
			return sprintf(sysfsbuf, "%d [0:Single, 1:Frame, 2:Stream]\n", ast_get_trigger_mode(ast_video, sensor_attr->index));
			break;
		case 1: 
			return sprintf(sysfsbuf, "%d:%s \n", ast_get_compress_yuv_mode(ast_video, sensor_attr->index), ast_get_compress_yuv_mode(ast_video, sensor_attr->index) ? "YUV420":"YUV444");
			break;			
		case 2: 
			return sprintf(sysfsbuf, "%d:%s \n", ast_get_compress_jpeg_mode(ast_video, sensor_attr->index), ast_get_compress_jpeg_mode(ast_video, sensor_attr->index) ? "JPEG":"ASPEED");
			break;
		default:
			return -EINVAL;
			break;
	}
	return -EINVAL;
}

#define sysfs_compress(index) \
static SENSOR_DEVICE_ATTR_2(compress##index##_trigger_mode, S_IRUGO | S_IWUSR, \
	ast_show_compress, ast_store_compress, 0, index); \
static SENSOR_DEVICE_ATTR_2(compress##index##_yuv, S_IRUGO | S_IWUSR, \
	ast_show_compress, ast_store_compress, 1, index); \
static SENSOR_DEVICE_ATTR_2(compress##index##_jpeg, S_IRUGO | S_IWUSR, \
	ast_show_compress, ast_store_compress, 2, index); \
\
static struct attribute *compress##index##_attributes[] = { \
	&sensor_dev_attr_compress##index##_trigger_mode.dev_attr.attr, \
	&sensor_dev_attr_compress##index##_yuv.dev_attr.attr, \
	&sensor_dev_attr_compress##index##_jpeg.dev_attr.attr, \
	NULL \
};

sysfs_compress(0);
sysfs_compress(1);
/************************************************** SYS FS Capture ***********************************************************/
static const struct attribute_group compress_attribute_groups[] = {
	{ .attrs = compress0_attributes },
	{ .attrs = compress1_attributes },	
};

/************************************************** SYS FS End ***********************************************************/
static int ast_vgavideo_probe(struct platform_device *pdev)
{
	struct resource *res0, *res1;
	int ret=0;
	int i;
	struct ast_vgavideo_data *ast_video;
	u32 vga_mem_size, mem_size;

 	res0 = platform_get_resource(pdev, IORESOURCE_MEM, 0);
 	if (NULL == res0) {
		dev_err(&pdev->dev, "cannot get IORESOURCE_MEM\n");

		printk("cannot get IORESOURCE_MEM\n");
		ret = -ENOENT;
		goto out;
	}

	if (!request_mem_region(res0->start, resource_size(res0), res0->name)) {
		dev_err(&pdev->dev, "cannot reserved region\n");
		ret = -ENXIO;
		goto out;
	}

	if(!(ast_video = kzalloc(sizeof(struct ast_vgavideo_data), GFP_KERNEL))) {
		return -ENOMEM;
		goto out;
        }

	mem_size = ast_sdmc_get_mem_size();
	vga_mem_size = ast_scu_get_vga_memsize();

	ast_video->vga_base = mem_size - vga_mem_size;
	ast_video->reg_base = ioremap(res0->start, resource_size(res0));
	if (!ast_video->reg_base) {
		ret = -EIO;
		goto out_region0;
	}
	
	res1 = platform_get_resource(pdev, IORESOURCE_DMA, 0);
	if (!res1)
		return -ENODEV;

	if (!request_mem_region(res1->start, resource_size(res1), res1->name)) {
		dev_err(&pdev->dev, "cannot reserved region\n");
		ret = -ENXIO;
		goto out_region0;
	}

	//Phy assign
	ast_video->video_mem_size = resource_size(res1);
	VIDEO_DBG("video_mem_size %d MB\n",ast_video->video_mem_size/1024/1024);
	
	ast_video->stream_phy = (phys_addr_t *) res1->start;
	ast_video->buff0_phy = (phys_addr_t *) (res1->start + 0x400000);  //4M : size 10MB
	ast_video->buff1_phy = (phys_addr_t *) (res1->start + 0xe00000);  //14M : size 10MB
	ast_video->bcd_phy = (phys_addr_t *) (res1->start + 0x1800000);   //24M : size 1MB
	ast_video->jpeg_buf0_phy = (phys_addr_t *) (res1->start + 0x1900000);  //25MB: size 10 MB
	ast_video->video_jpeg_offset = 0x2300000;						//TODO define 
	ast_video->jpeg_phy = (phys_addr_t *) (res1->start + 0x2300000);  //35MB: size 4 MB
	ast_video->jpeg_tbl_phy = (phys_addr_t *) (res1->start + 0x2700000);      //39MB: size 1 MB

	VIDEO_DBG("\nstream_phy: %x, buff0_phy: %x, buff1_phy:%x, bcd_phy:%x \njpeg_phy:%x, jpeg_tbl_phy:%x \n",
	        (u32)ast_video->stream_phy, (u32)ast_video->buff0_phy, (u32)ast_video->buff1_phy, (u32)ast_video->bcd_phy, (u32)ast_video->jpeg_phy, (u32)ast_video->jpeg_tbl_phy);

	//virt assign
	ast_video->stream_virt = ioremap(res1->start, resource_size(res1));
	if (!ast_video->stream_virt) {
	        ret = -EIO;
	        goto out_region1;
	}

	ast_video->buff0_virt = (u32)ast_video->stream_virt + 0x400000; //4M : size 10MB
	ast_video->buff1_virt = (u32)ast_video->stream_virt + 0xe00000; //14M : size 10MB
	ast_video->bcd_virt = (u32)ast_video->stream_virt + 0x1800000;  //24M : size 4MB
	ast_video->jpeg_buf0_virt = res1->start + 0x1900000;  //25MB: size x MB
	ast_video->jpeg_virt = (u32)ast_video->stream_virt + 0x2300000; //35MB: size 4 MB
	ast_video->jpeg_tbl_virt = (u32)ast_video->stream_virt + 0x2700000;     //39MB: size 1 MB

	VIDEO_DBG("\nstream_virt: %x, buff0_virt: %x, buff1_virt:%x, bcd_virt:%x \njpeg_virt:%x, jpeg_tbl_virt:%x \n",
	        (u32)ast_video->stream_virt, (u32)ast_video->buff0_virt, (u32)ast_video->buff1_virt, (u32)ast_video->bcd_virt, (u32)ast_video->jpeg_virt, (u32)ast_video->jpeg_tbl_virt);

	memset(ast_video->stream_virt, 0, resource_size(res1));	

	ast_video->irq = platform_get_irq(pdev, 0);
	if (ast_video->irq < 0) {
		dev_err(&pdev->dev, "no irq specified\n");
		ret = -ENOENT;
		goto out_region1;
	}

	// default config 
	ast_video->scaling = 0;
	
	ret = misc_register(&ast_vgavideo_misc);
	if (ret){		
		printk(KERN_ERR "VIDEO : failed to request interrupt\n");
		goto out_irq;
	}

	ret = sysfs_create_group(&pdev->dev.kobj, &video_attribute_group);
	if (ret)
		goto out_irq;

	for(i=0;i<2;i++) {
		ret = sysfs_create_group(&pdev->dev.kobj, &compress_attribute_groups[i]);
		if (ret)
			goto out_irq;
	}

	platform_set_drvdata(pdev, ast_video);
	dev_set_drvdata(ast_vgavideo_misc.this_device, ast_video);

	ast_vgavideo_ctrl_init(ast_video);

	ret = request_irq(ast_video->irq, ast_vgavideo_isr, IRQF_SHARED, "ast-vgavideo", ast_video);
	if (ret) {
		printk(KERN_INFO "VIDEO: Failed request irq %d\n", ast_video->irq);
		goto out_region1;
	}

#if 0
       ast_video->thread_task = kthread_create(ast_vgavideo_thread, (void *) ast_video, "ast-video-kthread");
        if (IS_ERR(ast_video->thread_task)) {
                printk("ast video cannot create kthread\n");
                ret = PTR_ERR(ast_video->thread_task);
                goto out_irq;
        }

	VIDEO_DBG("kthread pid: %d\n", ast_video->thread_task->pid);
#endif			
		
	printk(KERN_INFO "ast_video: driver successfully loaded.\n");

	return 0;

out_irq:
	free_irq(ast_video->irq, NULL);

out_region1:
	release_mem_region(res1->start, res1->end - res1->start + 1);	

out_region0:
	release_mem_region(res0->start, res0->end - res0->start + 1);
	
out:
	printk(KERN_WARNING "applesmc: driver init failed (ret=%d)!\n", ret);
	return ret;

}

static int ast_vgavideo_remove(struct platform_device *pdev)
{
	struct resource *res0, *res1;
	struct ast_vgavideo_data *ast_video = platform_get_drvdata(pdev);
	VIDEO_DBG("ast_vgavideo_remove\n");

	misc_deregister(&ast_vgavideo_misc);

	free_irq(ast_video->irq, ast_video);

	res0 = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	iounmap(ast_video->reg_base);

	release_mem_region(res0->start, res0->end - res0->start + 1);

	res1 = platform_get_resource(pdev, IORESOURCE_DMA, 0);

	iounmap(ast_video->stream_virt);

	release_mem_region(res1->start, res1->end - res1->start + 1);

	return 0;	
}

#ifdef CONFIG_PM
static int 
ast_vgavideo_suspend(struct platform_device *pdev, pm_message_t state)
{
	printk("ast_vgavideo_suspend : TODO \n");
	return 0;
}

static int 
ast_vgavideo_resume(struct platform_device *pdev)
{
	return 0;
}

#else
#define ast_vgavideo_suspend        NULL
#define ast_vgavideo_resume         NULL
#endif

static struct platform_driver ast_vgavideo_driver = {
	.probe = ast_vgavideo_probe,
	.remove = ast_vgavideo_remove,
	.suspend = ast_vgavideo_suspend, /* optional but recommended */
	.resume = ast_vgavideo_resume,   /* optional but recommended */
	.driver = {
		.name = "ast_vgavideo",
		.owner  = THIS_MODULE,
			
	},
};

static struct platform_device *ast_vgavideo_device;

static int __init ast_vgavideo_init(void)
{
	int ret;
	static const struct resource video_resources[] = {
		[0] = {
			.start = AST_VIDEO_BASE,
			.end = AST_VIDEO_BASE + SZ_2K - 1,
			.flags = IORESOURCE_MEM,
		},
		[1] = {
			.start = IRQ_VIDEO,
			.end = IRQ_VIDEO,
			.flags = IORESOURCE_IRQ,
		},
		[2] = {
			.start = AST_VIDEO_MEM,
			.end = AST_VIDEO_MEM + AST_VIDEO_MEM_SIZE - 1,
			.flags = IORESOURCE_DMA,
		},	
	};

	ret = platform_driver_register(&ast_vgavideo_driver);

	ast_scu_init_video(0);
	ast_scu_multi_func_video();

	if (!ret) {
		ast_vgavideo_device = platform_device_register_simple("ast_vgavideo", 0,
								video_resources, ARRAY_SIZE(video_resources));
		if (IS_ERR(ast_vgavideo_device)) {
			platform_driver_unregister(&ast_vgavideo_driver);
			ret = PTR_ERR(ast_vgavideo_device);
		}
	}

	return ret;
}

static void __exit ast_vgavideo_exit(void)
{
	platform_device_unregister(ast_vgavideo_device);
	platform_driver_unregister(&ast_vgavideo_driver);
}


module_init(ast_vgavideo_init);
module_exit(ast_vgavideo_exit);

MODULE_AUTHOR("Ryan Chen <ryan_chen@aspeedtech.com>");
MODULE_DESCRIPTION("AST Video Engine driver");
MODULE_LICENSE("GPL");