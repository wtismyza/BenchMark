/*****************************************************************************
 *
 *     Author: Xilinx, Inc.
 *
 *     This program is free software; you can redistribute it and/or modify it
 *     under the terms of the GNU General Public License as published by the
 *     Free Software Foundation; either version 2 of the License, or (at your
 *     option) any later version.
 *
 *     XILINX IS PROVIDING THIS DESIGN, CODE, OR INFORMATION "AS IS"
 *     AS A COURTESY TO YOU, SOLELY FOR USE IN DEVELOPING PROGRAMS AND
 *     SOLUTIONS FOR XILINX DEVICES.  BY PROVIDING THIS DESIGN, CODE,
 *     OR INFORMATION AS ONE POSSIBLE IMPLEMENTATION OF THIS FEATURE,
 *     APPLICATION OR STANDARD, XILINX IS MAKING NO REPRESENTATION
 *     THAT THIS IMPLEMENTATION IS FREE FROM ANY CLAIMS OF INFRINGEMENT,
 *     AND YOU ARE RESPONSIBLE FOR OBTAINING ANY RIGHTS YOU MAY REQUIRE
 *     FOR YOUR IMPLEMENTATION.  XILINX EXPRESSLY DISCLAIMS ANY
 *     WARRANTY WHATSOEVER WITH RESPECT TO THE ADEQUACY OF THE
 *     IMPLEMENTATION, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OR
 *     REPRESENTATIONS THAT THIS IMPLEMENTATION IS FREE FROM CLAIMS OF
 *     INFRINGEMENT, IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *     FOR A PARTICULAR PURPOSE.
 *
 *     Xilinx products are not intended for use in life support appliances,
 *     devices, or systems. Use in such applications is expressly prohibited.
 *
 *     (c) Copyright 2003-2007 Xilinx Inc.
 *     All rights reserved.
 *
 *     You should have received a copy of the GNU General Public License along
 *     with this program; if not, write to the Free Software Foundation, Inc.,
 *     675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *****************************************************************************/

#ifndef XILINX_HWICAP_H_	/* prevent circular inclusions */
#define XILINX_HWICAP_H_	/* by using protection macros */

#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/version.h>
#include <linux/platform_device.h>

#include <asm/io.h>

struct hwicap_drvdata {
	u32 write_buffer_in_use;  /* Always in [0,3] */
	u8 write_buffer[4];
	u32 read_buffer_in_use;	  /* Always in [0,3] */
	u8 read_buffer[4];
	u32 mem_start;		  /* phys. address of the control registers */
	u32 mem_end;		  /* phys. address of the control registers */
	u32 mem_size;
	void __iomem *base_address;/* virt. address of the control registers */

	struct device *dev;
	struct cdev cdev;	/* Char device structure */
	dev_t devt;

	const struct hwicap_driver_config *config;
	const struct config_registers *config_regs;
	void *private_data;
	bool is_open;
	struct semaphore sem;
};

struct hwicap_driver_config {
	int (*get_configuration)(struct hwicap_drvdata *drvdata, u32 *data,
			u32 size);
	int (*set_configuration)(struct hwicap_drvdata *drvdata, u32 *data,
			u32 size);
	void (*reset)(struct hwicap_drvdata *drvdata);
};

/* Number of times to poll the done regsiter */
#define XHI_MAX_RETRIES     10

/************ Constant Definitions *************/

#define XHI_PAD_FRAMES              0x1

/* Mask for calculating configuration packet headers */
#define XHI_WORD_COUNT_MASK_TYPE_1  0x7FFUL
#define XHI_WORD_COUNT_MASK_TYPE_2  0x1FFFFFUL
#define XHI_TYPE_MASK               0x7
#define XHI_REGISTER_MASK           0xF
#define XHI_OP_MASK                 0x3

#define XHI_TYPE_SHIFT              29
#define XHI_REGISTER_SHIFT          13
#define XHI_OP_SHIFT                27

#define XHI_TYPE_1                  1
#define XHI_TYPE_2                  2
#define XHI_OP_WRITE                2
#define XHI_OP_READ                 1

/* Address Block Types */
#define XHI_FAR_CLB_BLOCK           0
#define XHI_FAR_BRAM_BLOCK          1
#define XHI_FAR_BRAM_INT_BLOCK      2

struct config_registers {
	u32 CRC;
	u32 FAR;
	u32 FDRI;
	u32 FDRO;
	u32 CMD;
	u32 CTL;
	u32 MASK;
	u32 STAT;
	u32 LOUT;
	u32 COR;
	u32 MFWR;
	u32 FLR;
	u32 KEY;
	u32 CBC;
	u32 IDCODE;
	u32 AXSS;
	u32 C0R_1;
	u32 CSOB;
	u32 WBSTAR;
	u32 TIMER;
	u32 BOOTSTS;
	u32 CTL_1;
};

/* Configuration Commands */
#define XHI_CMD_NULL                0
#define XHI_CMD_WCFG                1
#define XHI_CMD_MFW                 2
#define XHI_CMD_DGHIGH              3
#define XHI_CMD_RCFG                4
#define XHI_CMD_START               5
#define XHI_CMD_RCAP                6
#define XHI_CMD_RCRC                7
#define XHI_CMD_AGHIGH              8
#define XHI_CMD_SWITCH              9
#define XHI_CMD_GRESTORE            10
#define XHI_CMD_SHUTDOWN            11
#define XHI_CMD_GCAPTURE            12
#define XHI_CMD_DESYNCH             13
#define XHI_CMD_IPROG               15 /* Only in Virtex5 */
#define XHI_CMD_CRCC                16 /* Only in Virtex5 */
#define XHI_CMD_LTIMER              17 /* Only in Virtex5 */

/* Packet constants */
#define XHI_SYNC_PACKET             0xAA995566UL
#define XHI_DUMMY_PACKET            0xFFFFFFFFUL
#define XHI_NOOP_PACKET             (XHI_TYPE_1 << XHI_TYPE_SHIFT)
#define XHI_TYPE_2_READ ((XHI_TYPE_2 << XHI_TYPE_SHIFT) | \
			(XHI_OP_READ << XHI_OP_SHIFT))

#define XHI_TYPE_2_WRITE ((XHI_TYPE_2 << XHI_TYPE_SHIFT) | \
			(XHI_OP_WRITE << XHI_OP_SHIFT))

#define XHI_TYPE2_CNT_MASK          0x07FFFFFF

#define XHI_TYPE_1_PACKET_MAX_WORDS 2047UL
#define XHI_TYPE_1_HEADER_BYTES     4
#define XHI_TYPE_2_HEADER_BYTES     8

/* Constant to use for CRC check when CRC has been disabled */
#define XHI_DISABLED_AUTO_CRC       0x0000DEFCUL

/**
 * hwicap_type_1_read: Generates a Type 1 read packet header.
 * @parameter: Register is the address of the register to be read back.
 *
 * Generates a Type 1 read packet header, which is used to indirectly
 * read registers in the configuration logic.  This packet must then
 * be sent through the icap device, and a return packet received with
 * the information.
 **/
static inline u32 hwicap_type_1_read(u32 Register)
{
	return (XHI_TYPE_1 << XHI_TYPE_SHIFT) |
		(Register << XHI_REGISTER_SHIFT) |
		(XHI_OP_READ << XHI_OP_SHIFT);
}

/**
 * hwicap_type_1_write: Generates a Type 1 write packet header
 * @parameter: Register is the address of the register to be read back.
 **/
static inline u32 hwicap_type_1_write(u32 Register)
{
	return (XHI_TYPE_1 << XHI_TYPE_SHIFT) |
		(Register << XHI_REGISTER_SHIFT) |
		(XHI_OP_WRITE << XHI_OP_SHIFT);
}

#endif
