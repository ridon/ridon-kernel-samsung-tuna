/*
 * hdmi_ti_4xxx_ip.c
 *
 * HDMI TI81xx, TI38xx, TI OMAP4 etc IP driver Library
 * Copyright (C) 2010-2011 Texas Instruments Incorporated - http://www.ti.com/
 * Authors: Yong Zhi
 *	Mythri pk <mythripk@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/omapfb.h>

#include "hdmi_ti_4xxx_ip.h"

static inline void hdmi_write_reg(void __iomem *base_addr,
				const struct hdmi_reg idx, u32 val)
{
	__raw_writel(val, base_addr + idx.idx);
}

static inline u32 hdmi_read_reg(void __iomem *base_addr,
				const struct hdmi_reg idx)
{
	return __raw_readl(base_addr + idx.idx);
}

static inline void __iomem *hdmi_wp_base(struct hdmi_ip_data *ip_data)
{
	return (void __iomem *) (ip_data->base_wp);
}

static inline void __iomem *hdmi_phy_base(struct hdmi_ip_data *ip_data)
{
	return (void __iomem *) (ip_data->base_wp + ip_data->hdmi_phy_offset);
}

static inline void __iomem *hdmi_pll_base(struct hdmi_ip_data *ip_data)
{
	return (void __iomem *)	(ip_data->base_wp + ip_data->hdmi_pll_offset);
}

static inline void __iomem *hdmi_av_base(struct hdmi_ip_data *ip_data)
{
	return (void __iomem *)
			(ip_data->base_wp + ip_data->hdmi_core_av_offset);
}

static inline void __iomem *hdmi_core_sys_base(struct hdmi_ip_data *ip_data)
{
	return (void __iomem *)
			(ip_data->base_wp + ip_data->hdmi_core_sys_offset);
}

static inline int hdmi_wait_for_bit_change(void __iomem *base_addr,
				const struct hdmi_reg idx,
				int b2, int b1, u32 val)
{
	u32 t = 0;
	while (val != REG_GET(base_addr, idx, b2, b1)) {
		udelay(1);
		if (t++ > 10000)
			return !val;
	}
	return val;
}

static int hdmi_pll_init(struct hdmi_ip_data *ip_data,
		enum hdmi_clk_refsel refsel, int dcofreq,
		struct hdmi_pll_info *fmt, u16 sd)
{
	u32 r;

	/* PLL start always use manual mode */
	REG_FLD_MOD(hdmi_pll_base(ip_data), PLLCTRL_PLL_CONTROL, 0x0, 0, 0);

	r = hdmi_read_reg(hdmi_pll_base(ip_data), PLLCTRL_CFG1);
	r = FLD_MOD(r, fmt->regm, 20, 9); /* CFG1_PLL_REGM */
	r = FLD_MOD(r, fmt->regn, 8, 1);  /* CFG1_PLL_REGN */

	hdmi_write_reg(hdmi_pll_base(ip_data), PLLCTRL_CFG1, r);

	r = hdmi_read_reg(hdmi_pll_base(ip_data), PLLCTRL_CFG2);

	r = FLD_MOD(r, 0x0, 12, 12); /* PLL_HIGHFREQ divide by 2 */
	r = FLD_MOD(r, 0x1, 13, 13); /* PLL_REFEN */
	r = FLD_MOD(r, 0x0, 14, 14); /* PHY_CLKINEN de-assert during locking */

	if (dcofreq) {
		/* divider programming for frequency beyond 1000Mhz */
		REG_FLD_MOD(hdmi_pll_base(ip_data), PLLCTRL_CFG3, sd, 17, 10);
		r = FLD_MOD(r, 0x4, 3, 1); /* 1000MHz and 2000MHz */
	} else {
		r = FLD_MOD(r, 0x2, 3, 1); /* 500MHz and 1000MHz */
	}

	hdmi_write_reg(hdmi_pll_base(ip_data), PLLCTRL_CFG2, r);

	r = hdmi_read_reg(hdmi_pll_base(ip_data), PLLCTRL_CFG4);
	r = FLD_MOD(r, fmt->regm2, 24, 18);
	r = FLD_MOD(r, fmt->regmf, 17, 0);

	hdmi_write_reg(hdmi_pll_base(ip_data), PLLCTRL_CFG4, r);

	/* go now */
	REG_FLD_MOD(hdmi_pll_base(ip_data), PLLCTRL_PLL_GO, 0x1, 0, 0);

	/* wait for bit change */
	if (hdmi_wait_for_bit_change(hdmi_pll_base(ip_data), PLLCTRL_PLL_GO,
							0, 0, 1) != 1) {
		pr_err("PLL GO bit not set\n");
		return -ETIMEDOUT;
	}

	/* Wait till the lock bit is set in PLL status */
	if (hdmi_wait_for_bit_change(hdmi_pll_base(ip_data),
				PLLCTRL_PLL_STATUS, 1, 1, 1) != 1) {
		pr_err("cannot lock PLL\n");
		pr_err("CFG1 0x%x\n",
			hdmi_read_reg(hdmi_pll_base(ip_data), PLLCTRL_CFG1));
		pr_err("CFG2 0x%x\n",
			hdmi_read_reg(hdmi_pll_base(ip_data), PLLCTRL_CFG2));
		pr_err("CFG4 0x%x\n",
			hdmi_read_reg(hdmi_pll_base(ip_data), PLLCTRL_CFG4));
		return -ETIMEDOUT;
	}

	pr_debug("PLL locked!\n");

	return 0;
}

/* PHY_PWR_CMD */
static int hdmi_set_phy_pwr(struct hdmi_ip_data *ip_data, enum hdmi_phy_pwr val)
{
	/* Command for power control of HDMI PHY */
	REG_FLD_MOD(hdmi_wp_base(ip_data), HDMI_WP_PWR_CTRL, val, 7, 6);

	/* Status of the power control of HDMI PHY */
	if (hdmi_wait_for_bit_change(hdmi_wp_base(ip_data),
				HDMI_WP_PWR_CTRL, 5, 4, val) != val) {
		pr_err("Failed to set PHY power mode to %d\n", val);
		return -ETIMEDOUT;
	}

	return 0;
}

/* PLL_PWR_CMD */
int hdmi_ti_4xxx_set_pll_pwr(struct hdmi_ip_data *ip_data, enum hdmi_pll_pwr val)
{
	/* Command for power control of HDMI PLL */
	REG_FLD_MOD(hdmi_wp_base(ip_data), HDMI_WP_PWR_CTRL, val, 3, 2);

	/* wait till PHY_PWR_STATUS is set */
if (hdmi_wait_for_bit_change(hdmi_wp_base(ip_data), HDMI_WP_PWR_CTRL,
						1, 0, val) != val) {
		pr_err("Failed to set PLL_PWR_STATUS\n");
		return -ETIMEDOUT;
	}

	return 0;
}
EXPORT_SYMBOL(hdmi_ti_4xxx_set_pll_pwr);

static int hdmi_pll_reset(struct hdmi_ip_data *ip_data)
{
	/* SYSRESET  controlled by power FSM */
	REG_FLD_MOD(hdmi_pll_base(ip_data), PLLCTRL_PLL_CONTROL, 0x0, 3, 3);

	/* READ 0x0 reset is in progress */
	if (hdmi_wait_for_bit_change(hdmi_pll_base(ip_data),
				PLLCTRL_PLL_STATUS, 0, 0, 1) != 1) {
		pr_err("Failed to sysreset PLL\n");
		return -ETIMEDOUT;
	}

	return 0;
}

int hdmi_ti_4xxx_pll_program(struct hdmi_ip_data *ip_data,
				struct hdmi_pll_info *fmt)
{
	u16 r = 0;
	enum hdmi_clk_refsel refsel;

	r = hdmi_ti_4xxx_set_pll_pwr(ip_data, HDMI_PLLPWRCMD_ALLOFF);
	if (r)
		return r;

	r = hdmi_ti_4xxx_set_pll_pwr(ip_data, HDMI_PLLPWRCMD_BOTHON_ALLCLKS);
	if (r)
		return r;

	r = hdmi_pll_reset(ip_data);
	if (r)
		return r;

	refsel = HDMI_REFSEL_SYSCLK;

	r = hdmi_pll_init(ip_data, refsel, fmt->dcofreq, fmt, fmt->regsd);
	if (r)
		return r;

	return 0;
}

int hdmi_ti_4xxx_phy_init(struct hdmi_ip_data *ip_data)
{
	u16 r = 0;

	r = hdmi_set_phy_pwr(ip_data, HDMI_PHYPWRCMD_LDOON);
	if (r)
		return r;

	r = hdmi_set_phy_pwr(ip_data, HDMI_PHYPWRCMD_TXON);
	if (r)
		return r;

	/*
	 * Read address 0 in order to get the SCP reset done completed
	 * Dummy access performed to make sure reset is done
	 */
	hdmi_read_reg(hdmi_phy_base(ip_data), HDMI_TXPHY_TX_CTRL);

	/*
	 * Write to phy address 0 to configure the clock
	 * use HFBITCLK write HDMI_TXPHY_TX_CONTROL_FREQOUT field
	 */
	REG_FLD_MOD(hdmi_phy_base(ip_data), HDMI_TXPHY_TX_CTRL, 0x1, 31, 30);

	/* Write to phy address 1 to start HDMI line (TXVALID and TMDSCLKEN) */
	hdmi_write_reg(hdmi_phy_base(ip_data),
					HDMI_TXPHY_DIGITAL_CTRL, 0xF0000000);

	/* Write to phy address 3 to change the polarity control */
	REG_FLD_MOD(hdmi_phy_base(ip_data),
					HDMI_TXPHY_PAD_CFG_CTRL, 0x1, 27, 27);

	return 0;
}

void hdmi_ti_4xxx_phy_off(struct hdmi_ip_data *ip_data)
{
	hdmi_set_phy_pwr(ip_data, HDMI_PHYPWRCMD_OFF);
}
EXPORT_SYMBOL(hdmi_ti_4xxx_phy_init);
EXPORT_SYMBOL(hdmi_ti_4xxx_phy_off);

static int hdmi_core_ddc_edid(struct hdmi_ip_data *ip_data,
						u8 *pedid, int ext)
{
	u32 i, j;
	char checksum = 0;
	u32 offset = 0;

	/* Turn on CLK for DDC */
	REG_FLD_MOD(hdmi_av_base(ip_data), HDMI_CORE_AV_DPD, 0x7, 2, 0);

	/*
	 * SW HACK : Without the Delay DDC(i2c bus) reads 0 values /
	 * right shifted values( The behavior is not consistent and seen only
	 * with some TV's)
	 */
	usleep_range(800, 1000);

	if (!ext) {
		/* Clk SCL Devices */
		REG_FLD_MOD(hdmi_core_sys_base(ip_data),
						HDMI_CORE_DDC_CMD, 0xA, 3, 0);

		/* HDMI_CORE_DDC_STATUS_IN_PROG */
		if (hdmi_wait_for_bit_change(hdmi_core_sys_base(ip_data),
					HDMI_CORE_DDC_STATUS, 4, 4, 0) != 0) {
			pr_err("Failed to program DDC\n");
			return -ETIMEDOUT;
		}

		/* Clear FIFO */
		REG_FLD_MOD(hdmi_core_sys_base(ip_data)
						, HDMI_CORE_DDC_CMD, 0x9, 3, 0);

		/* HDMI_CORE_DDC_STATUS_IN_PROG */
		if (hdmi_wait_for_bit_change(hdmi_core_sys_base(ip_data),
					HDMI_CORE_DDC_STATUS, 4, 4, 0) != 0) {
			pr_err("Failed to program DDC\n");
			return -ETIMEDOUT;
		}

	} else {
		if (ext % 2 != 0)
			offset = 0x80;
	}

	/* Load Segment Address Register */
	REG_FLD_MOD(hdmi_core_sys_base(ip_data),
					HDMI_CORE_DDC_SEGM, ext/2, 7, 0);

	/* Load Slave Address Register */
	REG_FLD_MOD(hdmi_core_sys_base(ip_data),
					HDMI_CORE_DDC_ADDR, 0xA0 >> 1, 7, 1);

	/* Load Offset Address Register */
	REG_FLD_MOD(hdmi_core_sys_base(ip_data),
					HDMI_CORE_DDC_OFFSET, offset, 7, 0);

	/* Load Byte Count */
	REG_FLD_MOD(hdmi_core_sys_base(ip_data),
					HDMI_CORE_DDC_COUNT1, 0x80, 7, 0);
	REG_FLD_MOD(hdmi_core_sys_base(ip_data),
					HDMI_CORE_DDC_COUNT2, 0x0, 1, 0);

	/* Set DDC_CMD */
	if (ext)
		REG_FLD_MOD(hdmi_core_sys_base(ip_data),
					HDMI_CORE_DDC_CMD, 0x4, 3, 0);
	else
		REG_FLD_MOD(hdmi_core_sys_base(ip_data),
					HDMI_CORE_DDC_CMD, 0x2, 3, 0);

	/* HDMI_CORE_DDC_STATUS_BUS_LOW */
	if (REG_GET(hdmi_core_sys_base(ip_data),
					HDMI_CORE_DDC_STATUS, 6, 6) == 1) {
		pr_err("I2C Bus Low?\n");
		return -EIO;
	}
	/* HDMI_CORE_DDC_STATUS_NO_ACK */
	if (REG_GET(hdmi_core_sys_base(ip_data),
					HDMI_CORE_DDC_STATUS, 5, 5) == 1) {
		pr_err("I2C No Ack\n");
		return -EIO;
	}

	i = ext * 128;
	j = 0;
	while (((REG_GET(hdmi_core_sys_base(ip_data),
			HDMI_CORE_DDC_STATUS, 4, 4) == 1) ||
			(REG_GET(hdmi_core_sys_base(ip_data),
			HDMI_CORE_DDC_STATUS, 2, 2) == 0)) && j < 128) {

		if (REG_GET(hdmi_core_sys_base(ip_data)
					, HDMI_CORE_DDC_STATUS, 2, 2) == 0) {
			/* FIFO not empty */
			pedid[i++] = REG_GET(hdmi_core_sys_base(ip_data),
						HDMI_CORE_DDC_DATA, 7, 0);
			j++;
		}
	}

	for (j = 0; j < 128; j++)
		checksum += pedid[j];

	if (checksum != 0) {
		pr_err("E-EDID checksum failed!!\n");
		return -EIO;
	}

	return 0;
}

int read_ti_4xxx_edid(struct hdmi_ip_data *ip_data, u8 *pedid, u16 max_length)
{
	int r = 0, n = 0, i = 0;
	int max_ext_blocks = (max_length / 128) - 1;

	r = hdmi_core_ddc_edid(ip_data, pedid, 0);
	if (r) {
		return r;
	} else {
		n = pedid[0x7e];

		/*
		 * README: need to comply with max_length set by the caller.
		 * Better implementation should be to allocate necessary
		 * memory to store EDID according to nb_block field found
		 * in first block
		 */
		if (n > max_ext_blocks)
			n = max_ext_blocks;

		for (i = 1; i <= n; i++) {
			r = hdmi_core_ddc_edid(ip_data, pedid, i);
			if (r)
				return r;
		}
	}
	return 0;
}
EXPORT_SYMBOL(read_ti_4xxx_edid);

static void hdmi_core_init(enum hdmi_deep_color_mode deep_color,
			struct hdmi_core_video_config *video_cfg,
			struct hdmi_core_infoframe_avi *avi_cfg,
			struct hdmi_core_packet_enable_repeat *repeat_cfg)
{
	pr_debug("Enter hdmi_core_init\n");

	/* video core */
	switch (deep_color) {
	case HDMI_DEEP_COLOR_30BIT:
		video_cfg->ip_bus_width = HDMI_INPUT_10BIT;
		video_cfg->op_dither_truc = HDMI_OUTPUTTRUNCATION_10BIT;
		video_cfg->deep_color_pkt = HDMI_DEEPCOLORPACKECTENABLE;
		video_cfg->pkt_mode = HDMI_PACKETMODE30BITPERPIXEL;
		break;
	case HDMI_DEEP_COLOR_36BIT:
		video_cfg->ip_bus_width = HDMI_INPUT_12BIT;
		video_cfg->op_dither_truc = HDMI_OUTPUTTRUNCATION_12BIT;
		video_cfg->deep_color_pkt = HDMI_DEEPCOLORPACKECTENABLE;
		video_cfg->pkt_mode = HDMI_PACKETMODE36BITPERPIXEL;
		break;
	case HDMI_DEEP_COLOR_24BIT:
	default:
		video_cfg->ip_bus_width = HDMI_INPUT_8BIT;
		video_cfg->op_dither_truc = HDMI_OUTPUTTRUNCATION_8BIT;
		video_cfg->deep_color_pkt = HDMI_DEEPCOLORPACKECTDISABLE;
		video_cfg->pkt_mode = HDMI_PACKETMODERESERVEDVALUE;
		break;
	}

	video_cfg->hdmi_dvi = HDMI_DVI;
	video_cfg->tclk_sel_clkmult = HDMI_FPLL10IDCK;

	/* info frame */
	avi_cfg->db1_format = 0;
	avi_cfg->db1_active_info = 0;
	avi_cfg->db1_bar_info_dv = 0;
	avi_cfg->db1_scan_info = 0;
	avi_cfg->db2_colorimetry = 0;
	avi_cfg->db2_aspect_ratio = 0;
	avi_cfg->db2_active_fmt_ar = 0;
	avi_cfg->db3_itc = 0;
	avi_cfg->db3_ec = 0;
	avi_cfg->db3_q_range = 0;
	avi_cfg->db3_nup_scaling = 0;
	avi_cfg->db4_videocode = 0;
	avi_cfg->db5_pixel_repeat = 0;
	avi_cfg->db6_7_line_eoftop = 0 ;
	avi_cfg->db8_9_line_sofbottom = 0;
	avi_cfg->db10_11_pixel_eofleft = 0;
	avi_cfg->db12_13_pixel_sofright = 0;

	/* packet enable and repeat */
	repeat_cfg->audio_pkt = 0;
	repeat_cfg->audio_pkt_repeat = 0;
	repeat_cfg->avi_infoframe = 0;
	repeat_cfg->avi_infoframe_repeat = 0;
	repeat_cfg->gen_cntrl_pkt = 0;
	repeat_cfg->gen_cntrl_pkt_repeat = 0;
	repeat_cfg->generic_pkt = 0;
	repeat_cfg->generic_pkt_repeat = 0;
}

static void hdmi_core_powerdown_disable(struct hdmi_ip_data *ip_data)
{
	pr_debug("Enter hdmi_core_powerdown_disable\n");
	REG_FLD_MOD(hdmi_core_sys_base(ip_data), HDMI_CORE_CTRL1, 0x0, 0, 0);
}

static void hdmi_core_swreset_release(struct hdmi_ip_data *ip_data)
{
	pr_debug("Enter hdmi_core_swreset_release\n");
	REG_FLD_MOD(hdmi_core_sys_base(ip_data), HDMI_CORE_SYS_SRST, 0x0, 0, 0);
}

static void hdmi_core_swreset_assert(struct hdmi_ip_data *ip_data)
{
	pr_debug("Enter hdmi_core_swreset_assert\n");
	REG_FLD_MOD(hdmi_core_sys_base(ip_data), HDMI_CORE_SYS_SRST, 0x1, 0, 0);
}

/* HDMI_CORE_VIDEO_CONFIG */
static void hdmi_core_video_config(struct hdmi_ip_data *ip_data,
				struct hdmi_core_video_config *cfg)
{
	u32 r = 0;

	/* sys_ctrl1 default configuration not tunable */
	r = hdmi_read_reg(hdmi_core_sys_base(ip_data), HDMI_CORE_CTRL1);
	r = FLD_MOD(r, HDMI_CORE_CTRL1_VEN_FOLLOWVSYNC, 5, 5);
	r = FLD_MOD(r, HDMI_CORE_CTRL1_HEN_FOLLOWHSYNC, 4, 4);
	r = FLD_MOD(r, HDMI_CORE_CTRL1_BSEL_24BITBUS, 2, 2);
	r = FLD_MOD(r, HDMI_CORE_CTRL1_EDGE_RISINGEDGE, 1, 1);
	hdmi_write_reg(hdmi_core_sys_base(ip_data), HDMI_CORE_CTRL1, r);

	REG_FLD_MOD(hdmi_core_sys_base(ip_data),
			HDMI_CORE_SYS_VID_ACEN, cfg->ip_bus_width, 7, 6);

	/* Vid_Mode */
	r = hdmi_read_reg(hdmi_core_sys_base(ip_data), HDMI_CORE_SYS_VID_MODE);

	/* dither truncation configuration */
	if (cfg->op_dither_truc > HDMI_OUTPUTTRUNCATION_12BIT) {
		r = FLD_MOD(r, cfg->op_dither_truc - 3, 7, 6);
		r = FLD_MOD(r, 1, 5, 5);
	} else {
		r = FLD_MOD(r, cfg->op_dither_truc, 7, 6);
		r = FLD_MOD(r, 0, 5, 5);
	}
	hdmi_write_reg(hdmi_core_sys_base(ip_data), HDMI_CORE_SYS_VID_MODE, r);

	/* HDMI_Ctrl */
	r = hdmi_read_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_HDMI_CTRL);
	r = FLD_MOD(r, cfg->deep_color_pkt, 6, 6);
	r = FLD_MOD(r, cfg->pkt_mode, 5, 3);
	r = FLD_MOD(r, cfg->hdmi_dvi, 0, 0);
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_HDMI_CTRL, r);

	/* TMDS_CTRL */
	REG_FLD_MOD(hdmi_core_sys_base(ip_data),
			HDMI_CORE_SYS_TMDS_CTRL, cfg->tclk_sel_clkmult, 6, 5);
}

static void hdmi_core_aux_infoframe_avi_config(struct hdmi_ip_data *ip_data,
		struct hdmi_core_infoframe_avi info_avi)
{
	u32 val;
	char sum = 0, checksum = 0;

	sum += 0x82 + 0x002 + 0x00D;
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_TYPE, 0x082);
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_VERS, 0x002);
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_LEN, 0x00D);

	val = (info_avi.db1_format << 5) |
		(info_avi.db1_active_info << 4) |
		(info_avi.db1_bar_info_dv << 2) |
		(info_avi.db1_scan_info);
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_DBYTE(0), val);
	sum += val;

	val = (info_avi.db2_colorimetry << 6) |
		(info_avi.db2_aspect_ratio << 4) |
		(info_avi.db2_active_fmt_ar);
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_DBYTE(1), val);
	sum += val;

	val = (info_avi.db3_itc << 7) |
		(info_avi.db3_ec << 4) |
		(info_avi.db3_q_range << 2) |
		(info_avi.db3_nup_scaling);
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_DBYTE(2), val);
	sum += val;

	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_DBYTE(3),
					info_avi.db4_videocode);
	sum += info_avi.db4_videocode;

	val = info_avi.db5_pixel_repeat;
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_DBYTE(4), val);
	sum += val;

	val = info_avi.db6_7_line_eoftop & 0x00FF;
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_DBYTE(5), val);
	sum += val;

	val = ((info_avi.db6_7_line_eoftop >> 8) & 0x00FF);
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_DBYTE(6), val);
	sum += val;

	val = info_avi.db8_9_line_sofbottom & 0x00FF;
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_DBYTE(7), val);
	sum += val;

	val = ((info_avi.db8_9_line_sofbottom >> 8) & 0x00FF);
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_DBYTE(8), val);
	sum += val;

	val = info_avi.db10_11_pixel_eofleft & 0x00FF;
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_DBYTE(9), val);
	sum += val;

	val = ((info_avi.db10_11_pixel_eofleft >> 8) & 0x00FF);
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_DBYTE(10), val);
	sum += val;

	val = info_avi.db12_13_pixel_sofright & 0x00FF;
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_DBYTE(11), val);
	sum += val;

	val = ((info_avi.db12_13_pixel_sofright >> 8) & 0x00FF);
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_DBYTE(12), val);
	sum += val;

	checksum = 0x100 - sum;
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_AVI_CHSUM, checksum);
}

static void hdmi_core_av_packet_config(struct hdmi_ip_data *ip_data,
		struct hdmi_core_packet_enable_repeat repeat_cfg)
{
	/* enable/repeat the infoframe */
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_PB_CTRL1,
		(repeat_cfg.audio_pkt << 5) |
		(repeat_cfg.audio_pkt_repeat << 4) |
		(repeat_cfg.avi_infoframe << 1) |
		(repeat_cfg.avi_infoframe_repeat));

	/* enable/repeat the packet */
	hdmi_write_reg(hdmi_av_base(ip_data), HDMI_CORE_AV_PB_CTRL2,
		(repeat_cfg.gen_cntrl_pkt << 3) |
		(repeat_cfg.gen_cntrl_pkt_repeat << 2) |
		(repeat_cfg.generic_pkt << 1) |
		(repeat_cfg.generic_pkt_repeat));
}

static void hdmi_wp_init(struct omap_video_timings *timings,
			struct hdmi_video_format *video_fmt,
			struct hdmi_video_interface *video_int)
{
	pr_debug("Enter hdmi_wp_init\n");

	timings->hbp = 0;
	timings->hfp = 0;
	timings->hsw = 0;
	timings->vbp = 0;
	timings->vfp = 0;
	timings->vsw = 0;

	video_fmt->packing_mode = HDMI_PACK_10b_RGB_YUV444;
	video_fmt->y_res = 0;
	video_fmt->x_res = 0;

	video_int->vsp = 0;
	video_int->hsp = 0;

	video_int->interlacing = 0;
	video_int->tm = 0; /* HDMI_TIMING_SLAVE */

}

void hdmi_ti_4xxx_wp_video_start(struct hdmi_ip_data *ip_data, bool start)
{
	REG_FLD_MOD(hdmi_wp_base(ip_data), HDMI_WP_VIDEO_CFG, start, 31, 31);
}
EXPORT_SYMBOL(hdmi_ti_4xxx_wp_video_start);

static void hdmi_wp_video_init_format(struct hdmi_video_format *video_fmt,
	struct omap_video_timings *timings, struct hdmi_config *param)
{
	pr_debug("Enter hdmi_wp_video_init_format\n");

	video_fmt->y_res = param->timings.yres;
	video_fmt->x_res = param->timings.xres;

	omapfb_fb2dss_timings(&param->timings, timings);
}

static void hdmi_wp_video_config_format(struct hdmi_ip_data *ip_data,
		struct hdmi_video_format *video_fmt)
{
	u32 l = 0;

	REG_FLD_MOD(hdmi_wp_base(ip_data), HDMI_WP_VIDEO_CFG,
			video_fmt->packing_mode, 10, 8);

	l |= FLD_VAL(video_fmt->y_res, 31, 16);
	l |= FLD_VAL(video_fmt->x_res, 15, 0);
	hdmi_write_reg(hdmi_wp_base(ip_data), HDMI_WP_VIDEO_SIZE, l);
}

static void hdmi_wp_video_config_interface(struct hdmi_ip_data *ip_data,
		struct hdmi_video_interface *video_int)
{
	u32 r;
	pr_debug("Enter hdmi_wp_video_config_interface\n");

	r = hdmi_read_reg(hdmi_wp_base(ip_data), HDMI_WP_VIDEO_CFG);
	r = FLD_MOD(r, video_int->vsp, 7, 7);
	r = FLD_MOD(r, video_int->hsp, 6, 6);
	r = FLD_MOD(r, video_int->interlacing, 3, 3);
	r = FLD_MOD(r, video_int->tm, 1, 0);
	hdmi_write_reg(hdmi_wp_base(ip_data), HDMI_WP_VIDEO_CFG, r);
}

static void hdmi_wp_video_config_timing(struct hdmi_ip_data *ip_data,
		struct omap_video_timings *timings)
{
	u32 timing_h = 0;
	u32 timing_v = 0;

	pr_debug("Enter hdmi_wp_video_config_timing\n");

	timing_h |= FLD_VAL(timings->hbp, 31, 20);
	timing_h |= FLD_VAL(timings->hfp, 19, 8);
	timing_h |= FLD_VAL(timings->hsw, 7, 0);
	hdmi_write_reg(hdmi_wp_base(ip_data), HDMI_WP_VIDEO_TIMING_H, timing_h);

	timing_v |= FLD_VAL(timings->vbp, 31, 20);
	timing_v |= FLD_VAL(timings->vfp, 19, 8);
	timing_v |= FLD_VAL(timings->vsw, 7, 0);
	hdmi_write_reg(hdmi_wp_base(ip_data), HDMI_WP_VIDEO_TIMING_V, timing_v);
}

void hdmi_ti_4xxx_basic_configure(struct hdmi_ip_data *ip_data,
			struct hdmi_config *cfg)
{
	/* HDMI */
	struct omap_video_timings video_timing;
	struct hdmi_video_format video_format;
	struct hdmi_video_interface video_interface;
	/* HDMI core */
	struct hdmi_core_infoframe_avi avi_cfg;
	struct hdmi_core_video_config v_core_cfg;
	struct hdmi_core_packet_enable_repeat repeat_cfg;

	hdmi_wp_init(&video_timing, &video_format,
		&video_interface);

	hdmi_core_init(cfg->deep_color, &v_core_cfg,
		&avi_cfg,
		&repeat_cfg);

	hdmi_wp_video_init_format(&video_format, &video_timing, cfg);

	hdmi_wp_video_config_timing(ip_data, &video_timing);

	/* video config */
	video_format.packing_mode = HDMI_PACK_24b_RGB_YUV444_YUV422;

	hdmi_wp_video_config_format(ip_data, &video_format);

	video_interface.vsp = !!(cfg->timings.sync & FB_SYNC_VERT_HIGH_ACT);
	video_interface.hsp = !!(cfg->timings.sync & FB_SYNC_HOR_HIGH_ACT);
	video_interface.interlacing = cfg->timings.vmode & FB_VMODE_INTERLACED;
	video_interface.tm = 1 ; /* HDMI_TIMING_MASTER_24BIT */

	hdmi_wp_video_config_interface(ip_data, &video_interface);

	/*
	 * configure core video part
	 * set software reset in the core
	 */
	hdmi_core_swreset_assert(ip_data);

	/* power down off */
	hdmi_core_powerdown_disable(ip_data);

	v_core_cfg.pkt_mode = HDMI_PACKETMODE24BITPERPIXEL;
	v_core_cfg.hdmi_dvi = cfg->cm.mode;

	hdmi_core_video_config(ip_data, &v_core_cfg);

	/* release software reset in the core */
	hdmi_core_swreset_release(ip_data);

	/*
	 * configure packet
	 * info frame video see doc CEA861-D page 65
	 */
	avi_cfg.db1_format = HDMI_INFOFRAME_AVI_DB1Y_RGB;
	avi_cfg.db1_active_info =
		HDMI_INFOFRAME_AVI_DB1A_ACTIVE_FORMAT_OFF;
	avi_cfg.db1_bar_info_dv = HDMI_INFOFRAME_AVI_DB1B_NO;
	avi_cfg.db1_scan_info = HDMI_INFOFRAME_AVI_DB1S_0;
	avi_cfg.db2_colorimetry = HDMI_INFOFRAME_AVI_DB2C_NO;
	avi_cfg.db2_aspect_ratio = HDMI_INFOFRAME_AVI_DB2M_NO;
	avi_cfg.db2_active_fmt_ar = HDMI_INFOFRAME_AVI_DB2R_SAME;
	avi_cfg.db3_itc = HDMI_INFOFRAME_AVI_DB3ITC_NO;
	avi_cfg.db3_ec = HDMI_INFOFRAME_AVI_DB3EC_XVYUV601;
	avi_cfg.db3_q_range = HDMI_INFOFRAME_AVI_DB3Q_DEFAULT;
	avi_cfg.db3_nup_scaling = HDMI_INFOFRAME_AVI_DB3SC_NO;
	avi_cfg.db4_videocode = cfg->cm.code;
	avi_cfg.db5_pixel_repeat = HDMI_INFOFRAME_AVI_DB5PR_NO;
	avi_cfg.db6_7_line_eoftop = 0;
	avi_cfg.db8_9_line_sofbottom = 0;
	avi_cfg.db10_11_pixel_eofleft = 0;
	avi_cfg.db12_13_pixel_sofright = 0;

	hdmi_core_aux_infoframe_avi_config(ip_data, avi_cfg);

	/* enable/repeat the infoframe */
	repeat_cfg.avi_infoframe = HDMI_PACKETENABLE;
	repeat_cfg.avi_infoframe_repeat = HDMI_PACKETREPEATON;
	/* wakeup */
	repeat_cfg.audio_pkt = HDMI_PACKETENABLE;
	repeat_cfg.audio_pkt_repeat = HDMI_PACKETREPEATON;
	hdmi_core_av_packet_config(ip_data, repeat_cfg);
}
EXPORT_SYMBOL(hdmi_ti_4xxx_basic_configure);

void hdmi_ti_4xxx_dump_regs(struct hdmi_ip_data *ip_data, struct seq_file *s)
{
#define DUMPREG(g, r) seq_printf(s, "%-35s %08x\n", #r, hdmi_read_reg(g, r))

	void __iomem *wp_base = hdmi_wp_base(ip_data);
	void __iomem *core_sys_base = hdmi_core_sys_base(ip_data);
	void __iomem *phy_base = hdmi_phy_base(ip_data);
	void __iomem *pll_base = hdmi_pll_base(ip_data);
	void __iomem *av_base = hdmi_av_base(ip_data);

	/* wrapper registers */
	DUMPREG(wp_base, HDMI_WP_REVISION);
	DUMPREG(wp_base, HDMI_WP_SYSCONFIG);
	DUMPREG(wp_base, HDMI_WP_IRQSTATUS_RAW);
	DUMPREG(wp_base, HDMI_WP_IRQSTATUS);
	DUMPREG(wp_base, HDMI_WP_PWR_CTRL);
	DUMPREG(wp_base, HDMI_WP_IRQENABLE_SET);
	DUMPREG(wp_base, HDMI_WP_VIDEO_SIZE);
	DUMPREG(wp_base, HDMI_WP_VIDEO_TIMING_H);
	DUMPREG(wp_base, HDMI_WP_VIDEO_TIMING_V);
	DUMPREG(wp_base, HDMI_WP_WP_CLK);

	DUMPREG(core_sys_base, HDMI_CORE_SYS_VND_IDL);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_DEV_IDL);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_DEV_IDH);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_DEV_REV);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_SRST);
	DUMPREG(core_sys_base, HDMI_CORE_CTRL1);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_SYS_STAT);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_VID_ACEN);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_VID_MODE);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_INTR_STATE);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_INTR1);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_INTR2);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_INTR3);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_INTR4);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_UMASK1);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_TMDS_CTRL);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_DE_DLY);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_DE_CTRL);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_DE_TOP);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_DE_CNTL);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_DE_CNTH);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_DE_LINL);
	DUMPREG(core_sys_base, HDMI_CORE_SYS_DE_LINH_1);
	DUMPREG(core_sys_base, HDMI_CORE_DDC_CMD);
	DUMPREG(core_sys_base, HDMI_CORE_DDC_STATUS);
	DUMPREG(core_sys_base, HDMI_CORE_DDC_ADDR);
	DUMPREG(core_sys_base, HDMI_CORE_DDC_OFFSET);
	DUMPREG(core_sys_base, HDMI_CORE_DDC_COUNT1);
	DUMPREG(core_sys_base, HDMI_CORE_DDC_COUNT2);
	DUMPREG(core_sys_base, HDMI_CORE_DDC_DATA);
	DUMPREG(core_sys_base, HDMI_CORE_DDC_SEGM);

	DUMPREG(av_base, HDMI_CORE_AV_HDMI_CTRL);
	DUMPREG(av_base, HDMI_CORE_AV_AVI_DBYTE_NELEMS);
	DUMPREG(av_base, HDMI_CORE_AV_SPD_DBYTE);
	DUMPREG(av_base, HDMI_CORE_AV_SPD_DBYTE_NELEMS);
	DUMPREG(av_base, HDMI_CORE_AV_AUD_DBYTE_NELEMS);
	DUMPREG(av_base, HDMI_CORE_AV_MPEG_DBYTE);
	DUMPREG(av_base, HDMI_CORE_AV_MPEG_DBYTE_NELEMS);
	DUMPREG(av_base, HDMI_CORE_AV_GEN_DBYTE);
	DUMPREG(av_base, HDMI_CORE_AV_GEN_DBYTE_NELEMS);
	DUMPREG(av_base, HDMI_CORE_AV_GEN2_DBYTE);
	DUMPREG(av_base, HDMI_CORE_AV_GEN2_DBYTE_NELEMS);
	DUMPREG(av_base, HDMI_CORE_AV_ACR_CTRL);
	DUMPREG(av_base, HDMI_CORE_AV_FREQ_SVAL);
	DUMPREG(av_base, HDMI_CORE_AV_N_SVAL1);
	DUMPREG(av_base, HDMI_CORE_AV_N_SVAL2);
	DUMPREG(av_base, HDMI_CORE_AV_N_SVAL3);
	DUMPREG(av_base, HDMI_CORE_AV_CTS_SVAL1);
	DUMPREG(av_base, HDMI_CORE_AV_CTS_SVAL2);
	DUMPREG(av_base, HDMI_CORE_AV_CTS_SVAL3);
	DUMPREG(av_base, HDMI_CORE_AV_CTS_HVAL1);
	DUMPREG(av_base, HDMI_CORE_AV_CTS_HVAL2);
	DUMPREG(av_base, HDMI_CORE_AV_CTS_HVAL3);
	DUMPREG(av_base, HDMI_CORE_AV_AUD_MODE);
	DUMPREG(av_base, HDMI_CORE_AV_SPDIF_CTRL);
	DUMPREG(av_base, HDMI_CORE_AV_HW_SPDIF_FS);
	DUMPREG(av_base, HDMI_CORE_AV_SWAP_I2S);
	DUMPREG(av_base, HDMI_CORE_AV_SPDIF_ERTH);
	DUMPREG(av_base, HDMI_CORE_AV_I2S_IN_MAP);
	DUMPREG(av_base, HDMI_CORE_AV_I2S_IN_CTRL);
	DUMPREG(av_base, HDMI_CORE_AV_I2S_CHST0);
	DUMPREG(av_base, HDMI_CORE_AV_I2S_CHST1);
	DUMPREG(av_base, HDMI_CORE_AV_I2S_CHST2);
	DUMPREG(av_base, HDMI_CORE_AV_I2S_CHST4);
	DUMPREG(av_base, HDMI_CORE_AV_I2S_CHST5);
	DUMPREG(av_base, HDMI_CORE_AV_ASRC);
	DUMPREG(av_base, HDMI_CORE_AV_I2S_IN_LEN);
	DUMPREG(av_base, HDMI_CORE_AV_AUDO_TXSTAT);
	DUMPREG(av_base, HDMI_CORE_AV_AUD_PAR_BUSCLK_1);
	DUMPREG(av_base, HDMI_CORE_AV_AUD_PAR_BUSCLK_2);
	DUMPREG(av_base, HDMI_CORE_AV_AUD_PAR_BUSCLK_3);
	DUMPREG(av_base, HDMI_CORE_AV_TEST_TXCTRL);

	DUMPREG(av_base, HDMI_CORE_AV_DPD);
	DUMPREG(av_base, HDMI_CORE_AV_PB_CTRL1);
	DUMPREG(av_base, HDMI_CORE_AV_PB_CTRL2);
	DUMPREG(av_base, HDMI_CORE_AV_AVI_TYPE);
	DUMPREG(av_base, HDMI_CORE_AV_AVI_VERS);
	DUMPREG(av_base, HDMI_CORE_AV_AVI_LEN);
	DUMPREG(av_base, HDMI_CORE_AV_AVI_CHSUM);
	DUMPREG(av_base, HDMI_CORE_AV_SPD_TYPE);
	DUMPREG(av_base, HDMI_CORE_AV_SPD_VERS);
	DUMPREG(av_base, HDMI_CORE_AV_SPD_LEN);
	DUMPREG(av_base, HDMI_CORE_AV_SPD_CHSUM);
	DUMPREG(av_base, HDMI_CORE_AV_AUDIO_TYPE);
	DUMPREG(av_base, HDMI_CORE_AV_AUDIO_VERS);
	DUMPREG(av_base, HDMI_CORE_AV_AUDIO_LEN);
	DUMPREG(av_base, HDMI_CORE_AV_AUDIO_CHSUM);
	DUMPREG(av_base, HDMI_CORE_AV_MPEG_TYPE);
	DUMPREG(av_base, HDMI_CORE_AV_MPEG_VERS);
	DUMPREG(av_base, HDMI_CORE_AV_MPEG_LEN);
	DUMPREG(av_base, HDMI_CORE_AV_MPEG_CHSUM);
	DUMPREG(av_base, HDMI_CORE_AV_CP_BYTE1);
	DUMPREG(av_base, HDMI_CORE_AV_CEC_ADDR_ID);

	DUMPREG(pll_base, PLLCTRL_PLL_CONTROL);
	DUMPREG(pll_base, PLLCTRL_PLL_STATUS);
	DUMPREG(pll_base, PLLCTRL_PLL_GO);
	DUMPREG(pll_base, PLLCTRL_CFG1);
	DUMPREG(pll_base, PLLCTRL_CFG2);
	DUMPREG(pll_base, PLLCTRL_CFG3);
	DUMPREG(pll_base, PLLCTRL_CFG4);

	DUMPREG(phy_base, HDMI_TXPHY_TX_CTRL);
	DUMPREG(phy_base, HDMI_TXPHY_DIGITAL_CTRL);
	DUMPREG(phy_base, HDMI_TXPHY_POWER_CTRL);
	DUMPREG(phy_base, HDMI_TXPHY_PAD_CFG_CTRL);

#undef DUMPREG
}
EXPORT_SYMBOL(hdmi_ti_4xxx_dump_regs);

static int __init hdmi_ti_4xxx_init(void)
{
	return 0;
}

static void __exit hdmi_ti_4xxx_exit(void)
{

}

module_init(hdmi_ti_4xxx_init);
module_exit(hdmi_ti_4xxx_exit);

MODULE_AUTHOR("Texas Instruments");
MODULE_DESCRIPTION("hdmi_ti_4xxx_ip module");
MODULE_LICENSE("GPL");