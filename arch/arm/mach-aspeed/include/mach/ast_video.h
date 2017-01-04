 /********************************************************************************
* File Name     : drivers/video/ast_video.h
* Author         : Ryan Chen
* Description   : ASPEED Video Engine
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
*    1. 2012/12/27 Ryan Chen create this file
*        
*
********************************************************************************/
//VR08[2]
typedef enum ast_video_source {
	VIDEO_SOURCE_INT_VGA = 0,
	VIDEO_SOURCE_INT_CRT,
	VIDEO_SOURCE_EXT_ADC,
	VIDEO_SOURCE_EXT_DIGITAL,
} video_source;

//VR08[5]
typedef enum ast_vga_mode {
	VIDEO_VGA_DIRECT_MODE = 0,
	VIDEO_VGA_CAPTURE_MODE,
} vga_mode;

//VR08[4]
typedef enum ast_video_dis_en {
	VIDEO_EXT_DE_SIGNAL = 0,
	VIDEO_INT_DE_SIGNAL,
} display_enable;

typedef enum video_color_format {
	VIDEO_COLOR_RGB565 = 0,
	VIDEO_COLOR_RGB888,
	VIDEO_COLOR_YUV444,
	VIDEO_COLOR_YUV420,
} color_formate;

typedef enum vga_color_mode {
	VGA_NO_SIGNAL = 0,
	EGA_MODE,
	VGA_MODE,
	VGA_15BPP_MODE,
	VGA_16BPP_MODE,
	VGA_32BPP_MODE,
	VGA_CGA_MODE,
	VGA_TEXT_MODE,
} color_mode;

typedef enum video_stage {
	NONE,
	POLARITY,
	RESOLUTION,
	INIT,
	RUN,
} stage;

struct ast_video_plat_data {
	u32 (*get_clk)(void);
	void (*ctrl_reset)(void);
	void (*vga_display)(u8 enable);
	u8 (*get_vga_display)(void);
	u32 (*get_vga_base)(void);
//	compress_formate compress;
};
