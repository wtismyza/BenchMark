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
 *     (c) Copyright 2002 Xilinx Inc., Systems Engineering Group
 *     (c) Copyright 2004 Xilinx Inc., Systems Engineering Group
 *     (c) Copyright 2007-2008 Xilinx Inc.
 *     All rights reserved.
 *
 *     You should have received a copy of the GNU General Public License along
 *     with this program; if not, write to the Free Software Foundation, Inc.,
 *     675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *****************************************************************************/

/*
 * This is the code behind /dev/xilinx_icap -- it allows a user-space
 * application to use the Xilinx ICAP subsystem.
 *
 * The following operations are possible:
 *
 * open         open the port and initialize for access.
 * release      release port
 * write        Write a bitstream to the configuration processor.
 * read         Read a data stream from the configuration processor.
 *
 * After being opened, the port is initialized and accessed to avoid a
 * corrupted first read which may occur with some hardware.  The port
 * is left in a desynched state, requiring that a synch sequence be
 * transmitted before any valid configuration data.  A user will have
 * exclusive access to the device while it remains open, and the state
 * of the ICAP cannot be guaranteed after the device is closed.  Note
 * that a complete reset of the core and the state of the ICAP cannot
 * be performed on many versions of the cores, hence users of this
 * device should avoid making inconsistent accesses to the device.  In
 * particular, accessing the read interface, without first generating
 * a write containing a readback packet can leave the ICAP in an
 * inaccessible state.
 *
 * Note that in order to use the read interface, it is first necessary
 * to write a request packet to the write interface.  i.e., it is not
 * possible to simply readback the bitstream (or any configuration
 * bits) from a device without specifically requesting them first.
 * The code to craft such packets is intended to be part of the
 * user-space application code that uses this device.  The simplest
 * way to use this interface is simply:
 *
 * cp foo.bit /dev/xilinx_icap
 *
 * Note that unless foo.bit is an appropriately constructed partial
 * bitstream, this has a high likelyhood of overwriting the design
 * currently programmed in the FPGA.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/fcntl.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <asm/semaphore.h>
#include <linux/sysctl.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>

#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/system.h>

#ifdef CONFIG_OF
/* For open firmware. */
#include <linux/of_device.h>
#include <linux/of_platform.h>
#endif

#include "xilinx_hwicap.h"
#include "buffer_icap.h"
#include "fifo_icap.h"

#define DRIVER_NAME "xilinx_icap"

#define HWICAP_REGS   (0x10000)

/* dynamically allocate device number */
static int xhwicap_major;
static int xhwicap_minor;
#define HWICAP_DEVICES 1

module_param(xhwicap_major, int, S_IRUGO);
module_param(xhwicap_minor, int, S_IRUGO);

/* An array, which is set to true when the device is registered. */
static bool probed_devices[HWICAP_DEVICES];

static struct class *icap_class;

#define UNIMPLEMENTED 0xFFFF

static const struct config_registers v2_config_registers = {
	.CRC = 0,
	.FAR = 1,
	.FDRI = 2,
	.FDRO = 3,
	.CMD = 4,
	.CTL = 5,
	.MASK = 6,
	.STAT = 7,
	.LOUT = 8,
	.COR = 9,
	.MFWR = 10,
	.FLR = 11,
	.KEY = 12,
	.CBC = 13,
	.IDCODE = 14,
	.AXSS = UNIMPLEMENTED,
	.C0R_1 = UNIMPLEMENTED,
	.CSOB = UNIMPLEMENTED,
	.WBSTAR = UNIMPLEMENTED,
	.TIMER = UNIMPLEMENTED,
	.BOOTSTS = UNIMPLEMENTED,
	.CTL_1 = UNIMPLEMENTED,
};

static const struct config_registers v4_config_registers = {
	.CRC = 0,
	.FAR = 1,
	.FDRI = 2,
	.FDRO = 3,
	.CMD = 4,
	.CTL = 5,
	.MASK = 6,
	.STAT = 7,
	.LOUT = 8,
	.COR = 9,
	.MFWR = 10,
	.FLR = UNIMPLEMENTED,
	.KEY = UNIMPLEMENTED,
	.CBC = 11,
	.IDCODE = 12,
	.AXSS = 13,
	.C0R_1 = UNIMPLEMENTED,
	.CSOB = UNIMPLEMENTED,
	.WBSTAR = UNIMPLEMENTED,
	.TIMER = UNIMPLEMENTED,
	.BOOTSTS = UNIMPLEMENTED,
	.CTL_1 = UNIMPLEMENTED,
};
static const struct config_registers v5_config_registers = {
	.CRC = 0,
	.FAR = 1,
	.FDRI = 2,
	.FDRO = 3,
	.CMD = 4,
	.CTL = 5,
	.MASK = 6,
	.STAT = 7,
	.LOUT = 8,
	.COR = 9,
	.MFWR = 10,
	.FLR = UNIMPLEMENTED,
	.KEY = UNIMPLEMENTED,
	.CBC = 11,
	.IDCODE = 12,
	.AXSS = 13,
	.C0R_1 = 14,
	.CSOB = 15,
	.WBSTAR = 16,
	.TIMER = 17,
	.BOOTSTS = 18,
	.CTL_1 = 19,
};

/**
 * hwicap_command_desync: Send a DESYNC command to the ICAP port.
 * @parameter drvdata: a pointer to the drvdata.
 *
 * This command desynchronizes the ICAP After this command, a
 * bitstream containing a NULL packet, followed by a SYNCH packet is
 * required before the ICAP will recognize commands.
 */
int hwicap_command_desync(struct hwicap_drvdata *drvdata)
{
	u32 buffer[4];
	u32 index = 0;

	/*
	 * Create the data to be written to the ICAP.
	 */
	buffer[index++] = hwicap_type_1_write(drvdata->config_regs->CMD) | 1;
	buffer[index++] = XHI_CMD_DESYNCH;
	buffer[index++] = XHI_NOOP_PACKET;
	buffer[index++] = XHI_NOOP_PACKET;

	/*
	 * Write the data to the FIFO and intiate the transfer of data present
	 * in the FIFO to the ICAP device.
	 */
	return drvdata->config->set_configuration(drvdata,
			&buffer[0], index);
}

/**
 * hwicap_command_capture: Send a CAPTURE command to the ICAP port.
 * @parameter drvdata: a pointer to the drvdata.
 *
 * This command captures all of the flip flop states so they will be
 * available during readback.  One can use this command instead of
 * enabling the CAPTURE block in the design.
 */
int hwicap_command_capture(struct hwicap_drvdata *drvdata)
{
	u32 buffer[7];
	u32 index = 0;

	/*
	 * Create the data to be written to the ICAP.
	 */
	buffer[index++] = XHI_DUMMY_PACKET;
	buffer[index++] = XHI_SYNC_PACKET;
	buffer[index++] = XHI_NOOP_PACKET;
	buffer[index++] = hwicap_type_1_write(drvdata->config_regs->CMD) | 1;
	buffer[index++] = XHI_CMD_GCAPTURE;
	buffer[index++] = XHI_DUMMY_PACKET;
	buffer[index++] = XHI_DUMMY_PACKET;

	/*
	 * Write the data to the FIFO and intiate the transfer of data
	 * present in the FIFO to the ICAP device.
	 */
	return drvdata->config->set_configuration(drvdata,
			&buffer[0], index);

}

/**
 * hwicap_get_configuration_register: Query a configuration register.
 * @parameter drvdata: a pointer to the drvdata.
 * @parameter reg: a constant which represents the configuration
 *		register value to be returned.
 * 		Examples:  XHI_IDCODE, XHI_FLR.
 * @parameter RegData: returns the value of the register.
 *
 * Sends a query packet to the ICAP and then receives the response.
 * The icap is left in Synched state.
 */
int hwicap_get_configuration_register(struct hwicap_drvdata *drvdata,
		u32 reg, u32 *RegData)
{
	int status;
	u32 buffer[6];
	u32 index = 0;

	/*
	 * Create the data to be written to the ICAP.
	 */
	buffer[index++] = XHI_DUMMY_PACKET;
	buffer[index++] = XHI_SYNC_PACKET;
	buffer[index++] = XHI_NOOP_PACKET;
	buffer[index++] = hwicap_type_1_read(reg) | 1;
	buffer[index++] = XHI_NOOP_PACKET;
	buffer[index++] = XHI_NOOP_PACKET;

	/*
	 * Write the data to the FIFO and intiate the transfer of data present
	 * in the FIFO to the ICAP device.
	 */
	status = drvdata->config->set_configuration(drvdata,
			&buffer[0], index);
	if (status)
		return status;

	/*
	 * Read the configuration register
	 */
	status = drvdata->config->get_configuration(drvdata, RegData, 1);
	if (status)
		return status;

	return 0;
}

int hwicap_initialize_hwicap(struct hwicap_drvdata *drvdata)
{
	int status;
	u32 idcode;

	dev_dbg(drvdata->dev, "initializing\n");

	/* Abort any current transaction, to make sure we have the
	 * ICAP in a good state. */
	dev_dbg(drvdata->dev, "Reset...\n");
	drvdata->config->reset(drvdata);

	dev_dbg(drvdata->dev, "Desync...\n");
	status = hwicap_command_desync(drvdata);
	if (status)
		return status;

	/* Attempt to read the IDCODE from ICAP.  This
	 * may not be returned correctly, due to the design of the
	 * hardware.
	 */
	dev_dbg(drvdata->dev, "Reading IDCODE...\n");
	status = hwicap_get_configuration_register(
			drvdata, drvdata->config_regs->IDCODE, &idcode);
	dev_dbg(drvdata->dev, "IDCODE = %x\n", idcode);
	if (status)
		return status;

	dev_dbg(drvdata->dev, "Desync...\n");
	status = hwicap_command_desync(drvdata);
	if (status)
		return status;

	return 0;
}

static ssize_t
hwicap_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
	struct hwicap_drvdata *drvdata = file->private_data;
	ssize_t bytes_to_read = 0;
	u32 *kbuf;
	u32 words;
	u32 bytes_remaining;
	int status;

	if (down_interruptible(&drvdata->sem))
		return -ERESTARTSYS;

	if (drvdata->read_buffer_in_use) {
		/* If there are leftover bytes in the buffer, just */
		/* return them and don't try to read more from the */
		/* ICAP device. */
		bytes_to_read =
			(count < drvdata->read_buffer_in_use) ? count :
			drvdata->read_buffer_in_use;

		/* Return the data currently in the read buffer. */
		if (copy_to_user(buf, drvdata->read_buffer, bytes_to_read)) {
			status = -EFAULT;
			goto error;
		}
		drvdata->read_buffer_in_use -= bytes_to_read;
		memcpy(drvdata->read_buffer + bytes_to_read,
				drvdata->read_buffer, 4 - bytes_to_read);
	} else {
		/* Get new data from the ICAP, and return was was requested. */
		kbuf = (u32 *) get_zeroed_page(GFP_KERNEL);
		if (!kbuf) {
			status = -ENOMEM;
			goto error;
		}

		/* The ICAP device is only able to read complete */
		/* words.  If a number of bytes that do not correspond */
		/* to complete words is requested, then we read enough */
		/* words to get the required number of bytes, and then */
		/* save the remaining bytes for the next read. */

		/* Determine the number of words to read, rounding up */
		/* if necessary. */
		words = ((count + 3) >> 2);
		bytes_to_read = words << 2;

		if (bytes_to_read > PAGE_SIZE)
			bytes_to_read = PAGE_SIZE;

		/* Ensure we only read a complete number of words. */
		bytes_remaining = bytes_to_read & 3;
		bytes_to_read &= ~3;
		words = bytes_to_read >> 2;

		status = drvdata->config->get_configuration(drvdata,
				kbuf, words);

		/* If we didn't read correctly, then bail out. */
		if (status) {
			free_page((unsigned long)kbuf);
			goto error;
		}

		/* If we fail to return the data to the user, then bail out. */
		if (copy_to_user(buf, kbuf, bytes_to_read)) {
			free_page((unsigned long)kbuf);
			status = -EFAULT;
			goto error;
		}
		memcpy(kbuf, drvdata->read_buffer, bytes_remaining);
		drvdata->read_buffer_in_use = bytes_remaining;
		free_page((unsigned long)kbuf);
	}
	status = bytes_to_read;
 error:
	up(&drvdata->sem);
	return status;
}

static ssize_t
hwicap_write(struct file *file, const char *buf,
		size_t count, loff_t *ppos)
{
	struct hwicap_drvdata *drvdata = file->private_data;
	ssize_t written = 0;
	ssize_t left = count;
	u32 *kbuf;
	ssize_t len;
	ssize_t status;

	if (down_interruptible(&drvdata->sem))
		return -ERESTARTSYS;

	left += drvdata->write_buffer_in_use;

	/* Only write multiples of 4 bytes. */
	if (left < 4) {
		status = 0;
		goto error;
	}

	kbuf = (u32 *) __get_free_page(GFP_KERNEL);
	if (!kbuf) {
		status = -ENOMEM;
		goto error;
	}

	while (left > 3) {
		/* only write multiples of 4 bytes, so there might */
		/* be as many as 3 bytes left (at the end). */
		len = left;

		if (len > PAGE_SIZE)
			len = PAGE_SIZE;
		len &= ~3;

		if (drvdata->write_buffer_in_use) {
			memcpy(kbuf, drvdata->write_buffer,
					drvdata->write_buffer_in_use);
			if (copy_from_user(
			    (((char *)kbuf) + (drvdata->write_buffer_in_use)),
			    buf + written,
			    len - (drvdata->write_buffer_in_use))) {
				free_page((unsigned long)kbuf);
				status = -EFAULT;
				goto error;
			}
		} else {
			if (copy_from_user(kbuf, buf + written, len)) {
				free_page((unsigned long)kbuf);
				status = -EFAULT;
				goto error;
			}
		}

		status = drvdata->config->set_configuration(drvdata,
				kbuf, len >> 2);

		if (status) {
			free_page((unsigned long)kbuf);
			status = -EFAULT;
			goto error;
		}
		if (drvdata->write_buffer_in_use) {
			len -= drvdata->write_buffer_in_use;
			left -= drvdata->write_buffer_in_use;
			drvdata->write_buffer_in_use = 0;
		}
		written += len;
		left -= len;
	}
	if ((left > 0) && (left < 4)) {
		if (!copy_from_user(drvdata->write_buffer,
						buf + written, left)) {
			drvdata->write_buffer_in_use = left;
			written += left;
			left = 0;
		}
	}

	free_page((unsigned long)kbuf);
	status = written;
 error:
	up(&drvdata->sem);
	return status;
}

static int hwicap_open(struct inode *inode, struct file *file)
{
	struct hwicap_drvdata *drvdata;
	int status;

	drvdata = container_of(inode->i_cdev, struct hwicap_drvdata, cdev);

	if (down_interruptible(&drvdata->sem))
		return -ERESTARTSYS;

	if (drvdata->is_open) {
		status = -EBUSY;
		goto error;
	}

	status = hwicap_initialize_hwicap(drvdata);
	if (status) {
		dev_err(drvdata->dev, "Failed to open file");
		goto error;
	}

	file->private_data = drvdata;
	drvdata->write_buffer_in_use = 0;
	drvdata->read_buffer_in_use = 0;
	drvdata->is_open = 1;

 error:
	up(&drvdata->sem);
	return status;
}

static int hwicap_release(struct inode *inode, struct file *file)
{
	struct hwicap_drvdata *drvdata = file->private_data;
	int i;
	int status = 0;

	if (down_interruptible(&drvdata->sem))
		return -ERESTARTSYS;

	if (drvdata->write_buffer_in_use) {
		/* Flush write buffer. */
		for (i = drvdata->write_buffer_in_use; i < 4; i++)
			drvdata->write_buffer[i] = 0;

		status = drvdata->config->set_configuration(drvdata,
				(u32 *) drvdata->write_buffer, 1);
		if (status)
			goto error;
	}

	status = hwicap_command_desync(drvdata);
	if (status)
		goto error;

 error:
	drvdata->is_open = 0;
	up(&drvdata->sem);
	return status;
}

static struct file_operations hwicap_fops = {
	.owner = THIS_MODULE,
	.write = hwicap_write,
	.read = hwicap_read,
	.open = hwicap_open,
	.release = hwicap_release,
};

static int __devinit hwicap_setup(struct device *dev, int id,
		const struct resource *regs_res,
		const struct hwicap_driver_config *config,
		const struct config_registers *config_regs)
{
	dev_t devt;
	struct hwicap_drvdata *drvdata = NULL;
	int retval = 0;

	dev_info(dev, "Xilinx icap port driver\n");

	if (id < 0) {
		for (id = 0; id < HWICAP_DEVICES; id++)
			if (!probed_devices[id])
				break;
	}
	if (id < 0 || id >= HWICAP_DEVICES) {
		dev_err(dev, "%s%i too large\n", DRIVER_NAME, id);
		return -EINVAL;
	}
	if (probed_devices[id]) {
		dev_err(dev, "cannot assign to %s%i; it is already in use\n",
			DRIVER_NAME, id);
		return -EBUSY;
	}

	probed_devices[id] = 1;

	devt = MKDEV(xhwicap_major, xhwicap_minor + id);

	drvdata = kmalloc(sizeof(struct hwicap_drvdata), GFP_KERNEL);
	if (!drvdata) {
		dev_err(dev, "Couldn't allocate device private record\n");
		return -ENOMEM;
	}
	memset((void *)drvdata, 0, sizeof(struct hwicap_drvdata));
	dev_set_drvdata(dev, (void *)drvdata);

	if (!regs_res) {
		dev_err(dev, "Couldn't get registers resource\n");
		retval = -EFAULT;
		goto failed1;
	}

	drvdata->mem_start = regs_res->start;
	drvdata->mem_end = regs_res->end;
	drvdata->mem_size = regs_res->end - regs_res->start + 1;

	if (!request_mem_region(drvdata->mem_start,
					drvdata->mem_size, DRIVER_NAME)) {
		dev_err(dev, "Couldn't lock memory region at %p\n",
			(void *)regs_res->start);
		retval = -EBUSY;
		goto failed1;
	}

	drvdata->devt = devt;
	drvdata->dev = dev;
	drvdata->base_address = ioremap(drvdata->mem_start, drvdata->mem_size);
	if (!drvdata->base_address) {
		dev_err(dev, "ioremap() failed\n");
		goto failed2;
	}

	drvdata->config = config;
	drvdata->config_regs = config_regs;

	init_MUTEX(&drvdata->sem);
	drvdata->is_open = 0;

	dev_info(dev, "ioremap %lx to %p with size %x\n",
		 (unsigned long int)drvdata->mem_start,
			drvdata->base_address, drvdata->mem_size);

	cdev_init(&drvdata->cdev, &hwicap_fops);
	drvdata->cdev.owner = THIS_MODULE;
	retval = cdev_add(&drvdata->cdev, devt, 1);
	if (retval) {
		dev_err(dev, "cdev_add() failed\n");
		goto failed3;
	}
	/*  devfs_mk_cdev(devt, S_IFCHR|S_IRUGO|S_IWUGO, DRIVER_NAME); */
	class_device_create(icap_class, NULL, devt, NULL, DRIVER_NAME);
	return 0;		/* success */

 failed3:
	iounmap(drvdata->base_address);

 failed2:
	release_mem_region(regs_res->start, drvdata->mem_size);

 failed1:
	kfree(drvdata);

	return retval;
}

static struct hwicap_driver_config buffer_icap_config = {
	.get_configuration = buffer_icap_get_configuration,
	.set_configuration = buffer_icap_set_configuration,
	.reset = buffer_icap_reset,
};

static struct hwicap_driver_config fifo_icap_config = {
	.get_configuration = fifo_icap_get_configuration,
	.set_configuration = fifo_icap_set_configuration,
	.reset = fifo_icap_reset,
};

static int __devexit hwicap_remove(struct device *dev)
{
	struct hwicap_drvdata *drvdata;

	drvdata = (struct hwicap_drvdata *)dev_get_drvdata(dev);

	if (!drvdata)
		return 0;

	class_device_destroy(icap_class, drvdata->devt);
	cdev_del(&drvdata->cdev);
	iounmap(drvdata->base_address);
	release_mem_region(drvdata->mem_start, drvdata->mem_size);
	kfree(drvdata);
	dev_set_drvdata(dev, NULL);
	probed_devices[MINOR(dev->devt)-xhwicap_minor] = 0;

	return 0;		/* success */
}

static int __devinit hwicap_drv_probe(struct platform_device *pdev)
{
	struct resource *res;
	const struct config_registers *regs;
	const char *family;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	/* It's most likely that we're using V4, if the family is not
	   specified */
	regs = &v4_config_registers;
	family = pdev->dev.platform_data;

	if (family) {
		if (!strcmp(family, "virtex2p")) {
			regs = &v2_config_registers;
		} else if (!strcmp(family, "virtex4")) {
			regs = &v4_config_registers;
		} else if (!strcmp(family, "virtex5")) {
			regs = &v5_config_registers;
		}
	}

	return hwicap_setup(&pdev->dev, pdev->id, res,
			&buffer_icap_config, regs);
}

static int __devexit hwicap_drv_remove(struct platform_device *pdev)
{
	return hwicap_remove(&pdev->dev);
}

static struct platform_driver hwicap_platform_driver = {
	.probe = hwicap_drv_probe,
	.remove = hwicap_drv_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = DRIVER_NAME,
	},
};

/* ---------------------------------------------------------------------
 * OF bus binding
 */

#if defined(CONFIG_OF)
static int __devinit
hwicap_of_probe(struct of_device *op, const struct of_device_id *match)
{
	struct resource res;
	const unsigned int *id;
	const char *family;
	int rc;
	const struct hwicap_driver_config *config = match->data;
	const struct config_registers *regs;

	dev_dbg(&op->dev, "hwicap_of_probe(%p, %p)\n", op, match);

	rc = of_address_to_resource(op->node, 0, &res);
	if (rc) {
		dev_err(&op->dev, "invalid address\n");
		return rc;
	}

	id = of_get_property(op->node, "port-number", NULL);

	/* It's most likely that we're using V4, if the family is not
	   specified */
	regs = &v4_config_registers;
	family = of_get_property(op->node, "xlnx,family", NULL);

	if (family) {
		if (!strcmp(family, "virtex2p")) {
			regs = &v2_config_registers;
		} else if (!strcmp(family, "virtex4")) {
			regs = &v4_config_registers;
		} else if (!strcmp(family, "virtex5")) {
			regs = &v5_config_registers;
		}
	}
	return hwicap_setup(&op->dev, id ? *id : -1, &res, config,
			regs);
}

static int __devexit hwicap_of_remove(struct of_device *op)
{
	return hwicap_remove(&op->dev);
}

/* Match table for of_platform binding */
static const struct of_device_id __devinit hwicap_of_match[] = {
	{ .compatible = "xlnx,opb-hwicap-1.00.b", .data = &buffer_icap_config},
	{ .compatible = "xlnx,xps-hwicap-1.00.a", .data = &fifo_icap_config},
	{},
};
MODULE_DEVICE_TABLE(of, hwicap_of_match);

static struct of_platform_driver hwicap_of_driver = {
	.owner = THIS_MODULE,
	.name = DRIVER_NAME,
	.match_table = hwicap_of_match,
	.probe = hwicap_of_probe,
	.remove = __devexit_p(hwicap_of_remove),
	.driver = {
		.name = DRIVER_NAME,
	},
};

/* Registration helpers to keep the number of #ifdefs to a minimum */
static inline int __devinit hwicap_of_register(void)
{
	pr_debug("hwicap: calling of_register_platform_driver()\n");
	return of_register_platform_driver(&hwicap_of_driver);
}

static inline void __devexit hwicap_of_unregister(void)
{
	of_unregister_platform_driver(&hwicap_of_driver);
}
#else /* CONFIG_OF */
/* CONFIG_OF not enabled; do nothing helpers */
static inline int __devinit hwicap_of_register(void) { return 0; }
static inline void __devexit hwicap_of_unregister(void) { }
#endif /* CONFIG_OF */

static int __devinit hwicap_module_init(void)
{
	dev_t devt;
	int retval;

	icap_class = class_create(THIS_MODULE, "xilinx_config");

	if (xhwicap_major) {
		devt = MKDEV(xhwicap_major, xhwicap_minor);
		retval = register_chrdev_region(
				devt,
				HWICAP_DEVICES,
				DRIVER_NAME);
		if (retval < 0)
			return retval;
	} else {
		retval = alloc_chrdev_region(&devt,
				xhwicap_minor,
				HWICAP_DEVICES,
				DRIVER_NAME);
		if (retval < 0)
			return retval;
		xhwicap_major = MAJOR(devt);
	}

	retval = platform_driver_register(&hwicap_platform_driver);

	if (retval)
		goto failed1;

	retval = hwicap_of_register();

	if (retval)
		goto failed2;

	return retval;

 failed2:
	platform_driver_unregister(&hwicap_platform_driver);

 failed1:
	unregister_chrdev_region(devt, HWICAP_DEVICES);

	return retval;
}

static void __devexit hwicap_module_cleanup(void)
{
	dev_t devt = MKDEV(xhwicap_major, xhwicap_minor);

	class_destroy(icap_class);

	platform_driver_unregister(&hwicap_platform_driver);

	hwicap_of_unregister();

	unregister_chrdev_region(devt, HWICAP_DEVICES);
}

module_init(hwicap_module_init);
module_exit(hwicap_module_cleanup);

MODULE_AUTHOR("Xilinx, Inc; Xilinx Research Labs Group");
MODULE_DESCRIPTION("Xilinx ICAP Port Driver");
MODULE_LICENSE("GPL");
