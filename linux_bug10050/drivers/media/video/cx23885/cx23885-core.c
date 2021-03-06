/*
 *  Driver for the Conexant CX23885 PCIe bridge
 *
 *  Copyright (c) 2006 Steven Toth <stoth@hauppauge.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kmod.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <asm/div64.h>

#include "cx23885.h"

MODULE_DESCRIPTION("Driver for cx23885 based TV cards");
MODULE_AUTHOR("Steven Toth <stoth@hauppauge.com>");
MODULE_LICENSE("GPL");

static unsigned int debug;
module_param(debug,int,0644);
MODULE_PARM_DESC(debug,"enable debug messages");

static unsigned int card[]  = {[0 ... (CX23885_MAXBOARDS - 1)] = UNSET };
module_param_array(card,  int, NULL, 0444);
MODULE_PARM_DESC(card,"card type");

#define dprintk(level, fmt, arg...)\
	do { if (debug >= level)\
		printk(KERN_DEBUG "%s/0: " fmt, dev->name, ## arg);\
	} while (0)

static unsigned int cx23885_devcount;

static DEFINE_MUTEX(devlist);
LIST_HEAD(cx23885_devlist);

#define NO_SYNC_LINE (-1U)

/*
 * CX23885 Assumptions
 * 1 line = 16 bytes of CDT
 * cmds size = 80
 * cdt size = 16 * linesize
 * iqsize = 64
 * maxlines = 6
 *
 * Address Space:
 * 0x00000000 0x00008fff FIFO clusters
 * 0x00010000 0x000104af Channel Management Data Structures
 * 0x000104b0 0x000104ff Free
 * 0x00010500 0x000108bf 15 channels * iqsize
 * 0x000108c0 0x000108ff Free
 * 0x00010900 0x00010e9f IQ's + Cluster Descriptor Tables
 *                       15 channels * (iqsize + (maxlines * linesize))
 * 0x00010ea0 0x00010xxx Free
 */

static struct sram_channel cx23885_sram_channels[] = {
	[SRAM_CH01] = {
		.name		= "VID A",
		.cmds_start	= 0x10000,
		.ctrl_start	= 0x105b0,
		.cdt		= 0x107b0,
		.fifo_start	= 0x40,
		.fifo_size	= 0x2800,
		.ptr1_reg	= DMA1_PTR1,
		.ptr2_reg	= DMA1_PTR2,
		.cnt1_reg	= DMA1_CNT1,
		.cnt2_reg	= DMA1_CNT2,
		.jumponly	= 1,
	},
	[SRAM_CH02] = {
		.name		= "ch2",
		.cmds_start	= 0x0,
		.ctrl_start	= 0x0,
		.cdt		= 0x0,
		.fifo_start	= 0x0,
		.fifo_size	= 0x0,
		.ptr1_reg	= DMA2_PTR1,
		.ptr2_reg	= DMA2_PTR2,
		.cnt1_reg	= DMA2_CNT1,
		.cnt2_reg	= DMA2_CNT2,
	},
	[SRAM_CH03] = {
		.name		= "TS1 B",
		.cmds_start	= 0x100A0,
		.ctrl_start	= 0x10630,
		.cdt		= 0x10870,
		.fifo_start	= 0x5000,
		.fifo_size	= 0x1000,
		.ptr1_reg	= DMA3_PTR1,
		.ptr2_reg	= DMA3_PTR2,
		.cnt1_reg	= DMA3_CNT1,
		.cnt2_reg	= DMA3_CNT2,
	},
	[SRAM_CH04] = {
		.name		= "ch4",
		.cmds_start	= 0x0,
		.ctrl_start	= 0x0,
		.cdt		= 0x0,
		.fifo_start	= 0x0,
		.fifo_size	= 0x0,
		.ptr1_reg	= DMA4_PTR1,
		.ptr2_reg	= DMA4_PTR2,
		.cnt1_reg	= DMA4_CNT1,
		.cnt2_reg	= DMA4_CNT2,
	},
	[SRAM_CH05] = {
		.name		= "ch5",
		.cmds_start	= 0x0,
		.ctrl_start	= 0x0,
		.cdt		= 0x0,
		.fifo_start	= 0x0,
		.fifo_size	= 0x0,
		.ptr1_reg	= DMA5_PTR1,
		.ptr2_reg	= DMA5_PTR2,
		.cnt1_reg	= DMA5_CNT1,
		.cnt2_reg	= DMA5_CNT2,
	},
	[SRAM_CH06] = {
		.name		= "TS2 C",
		.cmds_start	= 0x10140,
		.ctrl_start	= 0x10680,
		.cdt		= 0x108d0,
		.fifo_start	= 0x6000,
		.fifo_size	= 0x1000,
		.ptr1_reg	= DMA5_PTR1,
		.ptr2_reg	= DMA5_PTR2,
		.cnt1_reg	= DMA5_CNT1,
		.cnt2_reg	= DMA5_CNT2,
	},
	[SRAM_CH07] = {
		.name		= "ch7",
		.cmds_start	= 0x0,
		.ctrl_start	= 0x0,
		.cdt		= 0x0,
		.fifo_start	= 0x0,
		.fifo_size	= 0x0,
		.ptr1_reg	= DMA6_PTR1,
		.ptr2_reg	= DMA6_PTR2,
		.cnt1_reg	= DMA6_CNT1,
		.cnt2_reg	= DMA6_CNT2,
	},
	[SRAM_CH08] = {
		.name		= "ch8",
		.cmds_start	= 0x0,
		.ctrl_start	= 0x0,
		.cdt		= 0x0,
		.fifo_start	= 0x0,
		.fifo_size	= 0x0,
		.ptr1_reg	= DMA7_PTR1,
		.ptr2_reg	= DMA7_PTR2,
		.cnt1_reg	= DMA7_CNT1,
		.cnt2_reg	= DMA7_CNT2,
	},
	[SRAM_CH09] = {
		.name		= "ch9",
		.cmds_start	= 0x0,
		.ctrl_start	= 0x0,
		.cdt		= 0x0,
		.fifo_start	= 0x0,
		.fifo_size	= 0x0,
		.ptr1_reg	= DMA8_PTR1,
		.ptr2_reg	= DMA8_PTR2,
		.cnt1_reg	= DMA8_CNT1,
		.cnt2_reg	= DMA8_CNT2,
	},
};

/* FIXME, these allocations will change when
 * analog arrives. The be reviewed.
 * CX23887 Assumptions
 * 1 line = 16 bytes of CDT
 * cmds size = 80
 * cdt size = 16 * linesize
 * iqsize = 64
 * maxlines = 6
 *
 * Address Space:
 * 0x00000000 0x00008fff FIFO clusters
 * 0x00010000 0x000104af Channel Management Data Structures
 * 0x000104b0 0x000104ff Free
 * 0x00010500 0x000108bf 15 channels * iqsize
 * 0x000108c0 0x000108ff Free
 * 0x00010900 0x00010e9f IQ's + Cluster Descriptor Tables
 *                       15 channels * (iqsize + (maxlines * linesize))
 * 0x00010ea0 0x00010xxx Free
 */

static struct sram_channel cx23887_sram_channels[] = {
	[SRAM_CH01] = {
		.name		= "VID A",
		.cmds_start	= 0x10000,
		.ctrl_start	= 0x105b0,
		.cdt		= 0x107b0,
		.fifo_start	= 0x40,
		.fifo_size	= 0x2800,
		.ptr1_reg	= DMA1_PTR1,
		.ptr2_reg	= DMA1_PTR2,
		.cnt1_reg	= DMA1_CNT1,
		.cnt2_reg	= DMA1_CNT2,
	},
	[SRAM_CH02] = {
		.name		= "ch2",
		.cmds_start	= 0x0,
		.ctrl_start	= 0x0,
		.cdt		= 0x0,
		.fifo_start	= 0x0,
		.fifo_size	= 0x0,
		.ptr1_reg	= DMA2_PTR1,
		.ptr2_reg	= DMA2_PTR2,
		.cnt1_reg	= DMA2_CNT1,
		.cnt2_reg	= DMA2_CNT2,
	},
	[SRAM_CH03] = {
		.name		= "TS1 B",
		.cmds_start	= 0x100A0,
		.ctrl_start	= 0x10780,
		.cdt		= 0x10400,
		.fifo_start	= 0x5000,
		.fifo_size	= 0x1000,
		.ptr1_reg	= DMA3_PTR1,
		.ptr2_reg	= DMA3_PTR2,
		.cnt1_reg	= DMA3_CNT1,
		.cnt2_reg	= DMA3_CNT2,
	},
	[SRAM_CH04] = {
		.name		= "ch4",
		.cmds_start	= 0x0,
		.ctrl_start	= 0x0,
		.cdt		= 0x0,
		.fifo_start	= 0x0,
		.fifo_size	= 0x0,
		.ptr1_reg	= DMA4_PTR1,
		.ptr2_reg	= DMA4_PTR2,
		.cnt1_reg	= DMA4_CNT1,
		.cnt2_reg	= DMA4_CNT2,
	},
	[SRAM_CH05] = {
		.name		= "ch5",
		.cmds_start	= 0x0,
		.ctrl_start	= 0x0,
		.cdt		= 0x0,
		.fifo_start	= 0x0,
		.fifo_size	= 0x0,
		.ptr1_reg	= DMA5_PTR1,
		.ptr2_reg	= DMA5_PTR2,
		.cnt1_reg	= DMA5_CNT1,
		.cnt2_reg	= DMA5_CNT2,
	},
	[SRAM_CH06] = {
		.name		= "TS2 C",
		.cmds_start	= 0x10140,
		.ctrl_start	= 0x10680,
		.cdt		= 0x108d0,
		.fifo_start	= 0x6000,
		.fifo_size	= 0x1000,
		.ptr1_reg	= DMA5_PTR1,
		.ptr2_reg	= DMA5_PTR2,
		.cnt1_reg	= DMA5_CNT1,
		.cnt2_reg	= DMA5_CNT2,
	},
	[SRAM_CH07] = {
		.name		= "ch7",
		.cmds_start	= 0x0,
		.ctrl_start	= 0x0,
		.cdt		= 0x0,
		.fifo_start	= 0x0,
		.fifo_size	= 0x0,
		.ptr1_reg	= DMA6_PTR1,
		.ptr2_reg	= DMA6_PTR2,
		.cnt1_reg	= DMA6_CNT1,
		.cnt2_reg	= DMA6_CNT2,
	},
	[SRAM_CH08] = {
		.name		= "ch8",
		.cmds_start	= 0x0,
		.ctrl_start	= 0x0,
		.cdt		= 0x0,
		.fifo_start	= 0x0,
		.fifo_size	= 0x0,
		.ptr1_reg	= DMA7_PTR1,
		.ptr2_reg	= DMA7_PTR2,
		.cnt1_reg	= DMA7_CNT1,
		.cnt2_reg	= DMA7_CNT2,
	},
	[SRAM_CH09] = {
		.name		= "ch9",
		.cmds_start	= 0x0,
		.ctrl_start	= 0x0,
		.cdt		= 0x0,
		.fifo_start	= 0x0,
		.fifo_size	= 0x0,
		.ptr1_reg	= DMA8_PTR1,
		.ptr2_reg	= DMA8_PTR2,
		.cnt1_reg	= DMA8_CNT1,
		.cnt2_reg	= DMA8_CNT2,
	},
};

static int cx23885_risc_decode(u32 risc)
{
	static char *instr[16] = {
		[ RISC_SYNC    >> 28 ] = "sync",
		[ RISC_WRITE   >> 28 ] = "write",
		[ RISC_WRITEC  >> 28 ] = "writec",
		[ RISC_READ    >> 28 ] = "read",
		[ RISC_READC   >> 28 ] = "readc",
		[ RISC_JUMP    >> 28 ] = "jump",
		[ RISC_SKIP    >> 28 ] = "skip",
		[ RISC_WRITERM >> 28 ] = "writerm",
		[ RISC_WRITECM >> 28 ] = "writecm",
		[ RISC_WRITECR >> 28 ] = "writecr",
	};
	static int incr[16] = {
		[ RISC_WRITE   >> 28 ] = 3,
		[ RISC_JUMP    >> 28 ] = 3,
		[ RISC_SKIP    >> 28 ] = 1,
		[ RISC_SYNC    >> 28 ] = 1,
		[ RISC_WRITERM >> 28 ] = 3,
		[ RISC_WRITECM >> 28 ] = 3,
		[ RISC_WRITECR >> 28 ] = 4,
	};
	static char *bits[] = {
		"12",   "13",   "14",   "resync",
		"cnt0", "cnt1", "18",   "19",
		"20",   "21",   "22",   "23",
		"irq1", "irq2", "eol",  "sol",
	};
	int i;

	printk("0x%08x [ %s", risc,
	       instr[risc >> 28] ? instr[risc >> 28] : "INVALID");
	for (i = ARRAY_SIZE(bits) - 1; i >= 0; i--)
		if (risc & (1 << (i + 12)))
			printk(" %s", bits[i]);
	printk(" count=%d ]\n", risc & 0xfff);
	return incr[risc >> 28] ? incr[risc >> 28] : 1;
}

void cx23885_wakeup(struct cx23885_tsport *port,
			   struct cx23885_dmaqueue *q, u32 count)
{
	struct cx23885_dev *dev = port->dev;
	struct cx23885_buffer *buf;
	int bc;

	for (bc = 0;; bc++) {
		if (list_empty(&q->active))
			break;
		buf = list_entry(q->active.next,
				 struct cx23885_buffer, vb.queue);

		/* count comes from the hw and is is 16bit wide --
		 * this trick handles wrap-arounds correctly for
		 * up to 32767 buffers in flight... */
		if ((s16) (count - buf->count) < 0)
			break;

		do_gettimeofday(&buf->vb.ts);
		dprintk(2, "[%p/%d] wakeup reg=%d buf=%d\n", buf, buf->vb.i,
			count, buf->count);
		buf->vb.state = VIDEOBUF_DONE;
		list_del(&buf->vb.queue);
		wake_up(&buf->vb.done);
	}
	if (list_empty(&q->active)) {
		del_timer(&q->timeout);
	} else {
		mod_timer(&q->timeout, jiffies + BUFFER_TIMEOUT);
	}
	if (bc != 1)
		printk("%s: %d buffers handled (should be 1)\n",
		       __FUNCTION__, bc);
}

int cx23885_sram_channel_setup(struct cx23885_dev *dev,
				      struct sram_channel *ch,
				      unsigned int bpl, u32 risc)
{
	unsigned int i, lines;
	u32 cdt;

	if (ch->cmds_start == 0)
	{
		dprintk(1, "%s() Erasing channel [%s]\n", __FUNCTION__,
			ch->name);
		cx_write(ch->ptr1_reg, 0);
		cx_write(ch->ptr2_reg, 0);
		cx_write(ch->cnt2_reg, 0);
		cx_write(ch->cnt1_reg, 0);
		return 0;
	} else {
		dprintk(1, "%s() Configuring channel [%s]\n", __FUNCTION__,
			ch->name);
	}

	bpl   = (bpl + 7) & ~7; /* alignment */
	cdt   = ch->cdt;
	lines = ch->fifo_size / bpl;
	if (lines > 6)
		lines = 6;
	BUG_ON(lines < 2);

	cx_write(8 + 0, cpu_to_le32(RISC_JUMP | RISC_IRQ1 | RISC_CNT_INC) );
	cx_write(8 + 4, cpu_to_le32(8) );
	cx_write(8 + 8, cpu_to_le32(0) );

	/* write CDT */
	for (i = 0; i < lines; i++) {
		dprintk(2, "%s() 0x%08x <- 0x%08x\n", __FUNCTION__, cdt + 16*i,
			ch->fifo_start + bpl*i);
		cx_write(cdt + 16*i, ch->fifo_start + bpl*i);
		cx_write(cdt + 16*i +  4, 0);
		cx_write(cdt + 16*i +  8, 0);
		cx_write(cdt + 16*i + 12, 0);
	}

	/* write CMDS */
	if (ch->jumponly)
		cx_write(ch->cmds_start +  0, 8);
	else
		cx_write(ch->cmds_start +  0, risc);
	cx_write(ch->cmds_start +  4, 0); /* 64 bits 63-32 */
	cx_write(ch->cmds_start +  8, cdt);
	cx_write(ch->cmds_start + 12, (lines*16) >> 3);
	cx_write(ch->cmds_start + 16, ch->ctrl_start);
	if (ch->jumponly)
		cx_write(ch->cmds_start + 20, 0x80000000 | (64 >> 2) );
	else
		cx_write(ch->cmds_start + 20, 64 >> 2);
	for (i = 24; i < 80; i += 4)
		cx_write(ch->cmds_start + i, 0);

	/* fill registers */
	cx_write(ch->ptr1_reg, ch->fifo_start);
	cx_write(ch->ptr2_reg, cdt);
	cx_write(ch->cnt2_reg, (lines*16) >> 3);
	cx_write(ch->cnt1_reg, (bpl >> 3) -1);

	dprintk(2,"[bridge %d] sram setup %s: bpl=%d lines=%d\n",
		dev->bridge,
		ch->name,
		bpl,
		lines);

	return 0;
}

void cx23885_sram_channel_dump(struct cx23885_dev *dev,
				      struct sram_channel *ch)
{
	static char *name[] = {
		"init risc lo",
		"init risc hi",
		"cdt base",
		"cdt size",
		"iq base",
		"iq size",
		"risc pc lo",
		"risc pc hi",
		"iq wr ptr",
		"iq rd ptr",
		"cdt current",
		"pci target lo",
		"pci target hi",
		"line / byte",
	};
	u32 risc;
	unsigned int i, j, n;

	printk("%s: %s - dma channel status dump\n",
	       dev->name, ch->name);
	for (i = 0; i < ARRAY_SIZE(name); i++)
		printk("%s:   cmds: %-15s: 0x%08x\n",
		       dev->name, name[i],
		       cx_read(ch->cmds_start + 4*i));

	for (i = 0; i < 4; i++) {
		risc = cx_read(ch->cmds_start + 4 * (i + 14));
		printk("%s:   risc%d: ", dev->name, i);
		cx23885_risc_decode(risc);
	}
	for (i = 0; i < (64 >> 2); i += n) {
		risc = cx_read(ch->ctrl_start + 4 * i);
		/* No consideration for bits 63-32 */

		printk("%s:   (0x%08x) iq %x: ", dev->name,
		       ch->ctrl_start + 4 * i, i);
		n = cx23885_risc_decode(risc);
		for (j = 1; j < n; j++) {
			risc = cx_read(ch->ctrl_start + 4 * (i + j));
			printk("%s:   iq %x: 0x%08x [ arg #%d ]\n",
			       dev->name, i+j, risc, j);
		}
	}

	printk("%s: fifo: 0x%08x -> 0x%x\n",
	       dev->name, ch->fifo_start, ch->fifo_start+ch->fifo_size);
	printk("%s: ctrl: 0x%08x -> 0x%x\n",
	       dev->name, ch->ctrl_start, ch->ctrl_start + 6*16);
	printk("%s:   ptr1_reg: 0x%08x\n",
	       dev->name, cx_read(ch->ptr1_reg));
	printk("%s:   ptr2_reg: 0x%08x\n",
	       dev->name, cx_read(ch->ptr2_reg));
	printk("%s:   cnt1_reg: 0x%08x\n",
	       dev->name, cx_read(ch->cnt1_reg));
	printk("%s:   cnt2_reg: 0x%08x\n",
	       dev->name, cx_read(ch->cnt2_reg));
}

static void cx23885_risc_disasm(struct cx23885_tsport *port,
				struct btcx_riscmem *risc)
{
	struct cx23885_dev *dev = port->dev;
	unsigned int i, j, n;

	printk("%s: risc disasm: %p [dma=0x%08lx]\n",
	       dev->name, risc->cpu, (unsigned long)risc->dma);
	for (i = 0; i < (risc->size >> 2); i += n) {
		printk("%s:   %04d: ", dev->name, i);
		n = cx23885_risc_decode(risc->cpu[i]);
		for (j = 1; j < n; j++)
			printk("%s:   %04d: 0x%08x [ arg #%d ]\n",
			       dev->name, i + j, risc->cpu[i + j], j);
		if (risc->cpu[i] == RISC_JUMP)
			break;
	}
}

static void cx23885_shutdown(struct cx23885_dev *dev)
{
	/* disable RISC controller */
	cx_write(DEV_CNTRL2, 0);

	/* Disable all IR activity */
	cx_write(IR_CNTRL_REG, 0);

	/* Disable Video A/B activity */
	cx_write(VID_A_DMA_CTL, 0);
	cx_write(VID_B_DMA_CTL, 0);
	cx_write(VID_C_DMA_CTL, 0);

	/* Disable Audio activity */
	cx_write(AUD_INT_DMA_CTL, 0);
	cx_write(AUD_EXT_DMA_CTL, 0);

	/* Disable Serial port */
	cx_write(UART_CTL, 0);

	/* Disable Interrupts */
	cx_write(PCI_INT_MSK, 0);
	cx_write(VID_A_INT_MSK, 0);
	cx_write(VID_B_INT_MSK, 0);
	cx_write(VID_C_INT_MSK, 0);
	cx_write(AUDIO_INT_INT_MSK, 0);
	cx_write(AUDIO_EXT_INT_MSK, 0);

}

static void cx23885_reset(struct cx23885_dev *dev)
{
	dprintk(1, "%s()\n", __FUNCTION__);

	cx23885_shutdown(dev);

	cx_write(PCI_INT_STAT, 0xffffffff);
	cx_write(VID_A_INT_STAT, 0xffffffff);
	cx_write(VID_B_INT_STAT, 0xffffffff);
	cx_write(VID_C_INT_STAT, 0xffffffff);
	cx_write(AUDIO_INT_INT_STAT, 0xffffffff);
	cx_write(AUDIO_EXT_INT_STAT, 0xffffffff);
	cx_write(CLK_DELAY, cx_read(CLK_DELAY) & 0x80000000);

	mdelay(100);

	cx23885_sram_channel_setup(dev, &dev->sram_channels[SRAM_CH01],
		720*4, 0);
	cx23885_sram_channel_setup(dev, &dev->sram_channels[SRAM_CH02], 128, 0);
	cx23885_sram_channel_setup(dev, &dev->sram_channels[SRAM_CH03],
		188*4, 0);
	cx23885_sram_channel_setup(dev, &dev->sram_channels[SRAM_CH04], 128, 0);
	cx23885_sram_channel_setup(dev, &dev->sram_channels[SRAM_CH05], 128, 0);
	cx23885_sram_channel_setup(dev, &dev->sram_channels[SRAM_CH06],
		188*4, 0);
	cx23885_sram_channel_setup(dev, &dev->sram_channels[SRAM_CH07], 128, 0);
	cx23885_sram_channel_setup(dev, &dev->sram_channels[SRAM_CH08], 128, 0);
	cx23885_sram_channel_setup(dev, &dev->sram_channels[SRAM_CH09], 128, 0);

	cx23885_gpio_setup(dev);
}


static int cx23885_pci_quirks(struct cx23885_dev *dev)
{
	dprintk(1, "%s()\n", __FUNCTION__);

	/* The cx23885 bridge has a weird bug which causes NMI to be asserted
	 * when DMA begins if RDR_TLCTL0 bit4 is not cleared. It does not
	 * occur on the cx23887 bridge.
	 */
	if(dev->bridge == CX23885_BRIDGE_885)
		cx_clear(RDR_TLCTL0, 1 << 4);

	return 0;
}

static int get_resources(struct cx23885_dev *dev)
{
	if (request_mem_region(pci_resource_start(dev->pci,0),
			       pci_resource_len(dev->pci,0),
			       dev->name))
		return 0;

	printk(KERN_ERR "%s: can't get MMIO memory @ 0x%llx\n",
		dev->name, (unsigned long long)pci_resource_start(dev->pci,0));

	return -EBUSY;
}

static void cx23885_timeout(unsigned long data);
int cx23885_risc_stopper(struct pci_dev *pci, struct btcx_riscmem *risc,
				u32 reg, u32 mask, u32 value);

static int cx23885_init_tsport(struct cx23885_dev *dev, struct cx23885_tsport *port, int portno)
{
	dprintk(1, "%s(portno=%d)\n", __FUNCTION__, portno);

	/* Transport bus init dma queue  - Common settings */
	port->dma_ctl_val        = 0x11; /* Enable RISC controller and Fifo */
	port->ts_int_msk_val     = 0x1111; /* TS port bits for RISC */

	spin_lock_init(&port->slock);
	port->dev = dev;
	port->nr = portno;

	INIT_LIST_HEAD(&port->mpegq.active);
	INIT_LIST_HEAD(&port->mpegq.queued);
	port->mpegq.timeout.function = cx23885_timeout;
	port->mpegq.timeout.data = (unsigned long)port;
	init_timer(&port->mpegq.timeout);

	switch(portno) {
	case 1:
		port->reg_gpcnt          = VID_B_GPCNT;
		port->reg_gpcnt_ctl      = VID_B_GPCNT_CTL;
		port->reg_dma_ctl        = VID_B_DMA_CTL;
		port->reg_lngth          = VID_B_LNGTH;
		port->reg_hw_sop_ctrl    = VID_B_HW_SOP_CTL;
		port->reg_gen_ctrl       = VID_B_GEN_CTL;
		port->reg_bd_pkt_status  = VID_B_BD_PKT_STATUS;
		port->reg_sop_status     = VID_B_SOP_STATUS;
		port->reg_fifo_ovfl_stat = VID_B_FIFO_OVFL_STAT;
		port->reg_vld_misc       = VID_B_VLD_MISC;
		port->reg_ts_clk_en      = VID_B_TS_CLK_EN;
		port->reg_src_sel        = VID_B_SRC_SEL;
		port->reg_ts_int_msk     = VID_B_INT_MSK;
		port->reg_ts_int_stat   = VID_B_INT_STAT;
		port->sram_chno          = SRAM_CH03; /* VID_B */
		port->pci_irqmask        = 0x02; /* VID_B bit1 */
		break;
	case 2:
		port->reg_gpcnt          = VID_C_GPCNT;
		port->reg_gpcnt_ctl      = VID_C_GPCNT_CTL;
		port->reg_dma_ctl        = VID_C_DMA_CTL;
		port->reg_lngth          = VID_C_LNGTH;
		port->reg_hw_sop_ctrl    = VID_C_HW_SOP_CTL;
		port->reg_gen_ctrl       = VID_C_GEN_CTL;
		port->reg_bd_pkt_status  = VID_C_BD_PKT_STATUS;
		port->reg_sop_status     = VID_C_SOP_STATUS;
		port->reg_fifo_ovfl_stat = VID_C_FIFO_OVFL_STAT;
		port->reg_vld_misc       = VID_C_VLD_MISC;
		port->reg_ts_clk_en      = VID_C_TS_CLK_EN;
		port->reg_src_sel        = 0;
		port->reg_ts_int_msk     = VID_C_INT_MSK;
		port->reg_ts_int_stat    = VID_C_INT_STAT;
		port->sram_chno          = SRAM_CH06; /* VID_C */
		port->pci_irqmask        = 0x04; /* VID_C bit2 */
		break;
	default:
		BUG();
	}

	cx23885_risc_stopper(dev->pci, &port->mpegq.stopper,
		     port->reg_dma_ctl, port->dma_ctl_val, 0x00);

	return 0;
}

static void cx23885_dev_checkrevision(struct cx23885_dev *dev)
{
	switch (cx_read(RDR_CFG2) & 0xff) {
	case 0x00:
		/* cx23885 */
		dev->hwrevision = 0xa0;
		break;
	case 0x01:
		/* CX23885-12Z */
		dev->hwrevision = 0xa1;
		break;
	case 0x02:
		/* CX23885-13Z */
		dev->hwrevision = 0xb0;
		break;
	case 0x03:
		/* CX23888-22Z */
		dev->hwrevision = 0xc0;
		break;
	case 0x0e:
		/* CX23887-15Z */
		dev->hwrevision = 0xc0;
	case 0x0f:
		/* CX23887-14Z */
		dev->hwrevision = 0xb1;
		break;
	default:
		printk(KERN_ERR "%s() New hardware revision found 0x%x\n",
			__FUNCTION__, dev->hwrevision);
	}
	if (dev->hwrevision)
		printk(KERN_INFO "%s() Hardware revision = 0x%02x\n",
			__FUNCTION__, dev->hwrevision);
	else
		printk(KERN_ERR "%s() Hardware revision unknown 0x%x\n",
			__FUNCTION__, dev->hwrevision);
}

static int cx23885_dev_setup(struct cx23885_dev *dev)
{
	int i;

	mutex_init(&dev->lock);

	atomic_inc(&dev->refcount);

	dev->nr = cx23885_devcount++;
	sprintf(dev->name, "cx23885[%d]", dev->nr);

	mutex_lock(&devlist);
	list_add_tail(&dev->devlist, &cx23885_devlist);
	mutex_unlock(&devlist);

	/* Configure the internal memory */
	if(dev->pci->device == 0x8880) {
		dev->bridge = CX23885_BRIDGE_887;
		dev->sram_channels = cx23887_sram_channels;
		/* Apply a sensible clock frequency for the PCIe bridge */
		dev->clk_freq = 25000000;
	} else
	if(dev->pci->device == 0x8852) {
		dev->bridge = CX23885_BRIDGE_885;
		dev->sram_channels = cx23885_sram_channels;
		/* Apply a sensible clock frequency for the PCIe bridge */
		dev->clk_freq = 28000000;
	} else
		BUG();

	dprintk(1, "%s() Memory configured for PCIe bridge type %d\n",
		__FUNCTION__, dev->bridge);

	/* board config */
	dev->board = UNSET;
	if (card[dev->nr] < cx23885_bcount)
		dev->board = card[dev->nr];
	for (i = 0; UNSET == dev->board  &&  i < cx23885_idcount; i++)
		if (dev->pci->subsystem_vendor == cx23885_subids[i].subvendor &&
		    dev->pci->subsystem_device == cx23885_subids[i].subdevice)
			dev->board = cx23885_subids[i].card;
	if (UNSET == dev->board) {
		dev->board = CX23885_BOARD_UNKNOWN;
		cx23885_card_list(dev);
	}

	/* If the user specific a clk freq override, apply it */
	if (cx23885_boards[dev->board].clk_freq > 0)
		dev->clk_freq = cx23885_boards[dev->board].clk_freq;

	dev->pci_bus  = dev->pci->bus->number;
	dev->pci_slot = PCI_SLOT(dev->pci->devfn);
	dev->pci_irqmask = 0x001f00;

	/* External Master 1 Bus */
	dev->i2c_bus[0].nr = 0;
	dev->i2c_bus[0].dev = dev;
	dev->i2c_bus[0].reg_stat  = I2C1_STAT;
	dev->i2c_bus[0].reg_ctrl  = I2C1_CTRL;
	dev->i2c_bus[0].reg_addr  = I2C1_ADDR;
	dev->i2c_bus[0].reg_rdata = I2C1_RDATA;
	dev->i2c_bus[0].reg_wdata = I2C1_WDATA;
	dev->i2c_bus[0].i2c_period = (0x9d << 24); /* 100kHz */

	/* External Master 2 Bus */
	dev->i2c_bus[1].nr = 1;
	dev->i2c_bus[1].dev = dev;
	dev->i2c_bus[1].reg_stat  = I2C2_STAT;
	dev->i2c_bus[1].reg_ctrl  = I2C2_CTRL;
	dev->i2c_bus[1].reg_addr  = I2C2_ADDR;
	dev->i2c_bus[1].reg_rdata = I2C2_RDATA;
	dev->i2c_bus[1].reg_wdata = I2C2_WDATA;
	dev->i2c_bus[1].i2c_period = (0x9d << 24); /* 100kHz */

	/* Internal Master 3 Bus */
	dev->i2c_bus[2].nr = 2;
	dev->i2c_bus[2].dev = dev;
	dev->i2c_bus[2].reg_stat  = I2C3_STAT;
	dev->i2c_bus[2].reg_ctrl  = I2C3_CTRL;
	dev->i2c_bus[2].reg_addr  = I2C3_ADDR;
	dev->i2c_bus[2].reg_rdata = I2C3_RDATA;
	dev->i2c_bus[2].reg_wdata = I2C3_WDATA;
	dev->i2c_bus[2].i2c_period = (0x07 << 24); /* 1.95MHz */

	if(cx23885_boards[dev->board].portb == CX23885_MPEG_DVB)
		cx23885_init_tsport(dev, &dev->ts1, 1);

	if(cx23885_boards[dev->board].portc == CX23885_MPEG_DVB)
		cx23885_init_tsport(dev, &dev->ts2, 2);

	if (get_resources(dev) < 0) {
		printk(KERN_ERR "CORE %s No more PCIe resources for "
		       "subsystem: %04x:%04x\n",
		       dev->name, dev->pci->subsystem_vendor,
		       dev->pci->subsystem_device);

		cx23885_devcount--;
		return -ENODEV;
	}

	/* PCIe stuff */
	dev->lmmio = ioremap(pci_resource_start(dev->pci,0),
			     pci_resource_len(dev->pci,0));

	dev->bmmio = (u8 __iomem *)dev->lmmio;

	printk(KERN_INFO "CORE %s: subsystem: %04x:%04x, board: %s [card=%d,%s]\n",
	       dev->name, dev->pci->subsystem_vendor,
	       dev->pci->subsystem_device, cx23885_boards[dev->board].name,
	       dev->board, card[dev->nr] == dev->board ?
	       "insmod option" : "autodetected");

	cx23885_pci_quirks(dev);

	/* Assume some sensible defaults */
	dev->tuner_type = cx23885_boards[dev->board].tuner_type;
	dev->tuner_addr = cx23885_boards[dev->board].tuner_addr;
	dev->radio_type = cx23885_boards[dev->board].radio_type;
	dev->radio_addr = cx23885_boards[dev->board].radio_addr;

	dprintk(1, "%s() tuner_type = 0x%x tuner_addr = 0x%x\n",
		__FUNCTION__, dev->tuner_type, dev->tuner_addr);
	dprintk(1, "%s() radio_type = 0x%x radio_addr = 0x%x\n",
		__FUNCTION__, dev->radio_type, dev->radio_addr);

	/* init hardware */
	cx23885_reset(dev);

	cx23885_i2c_register(&dev->i2c_bus[0]);
	cx23885_i2c_register(&dev->i2c_bus[1]);
	cx23885_i2c_register(&dev->i2c_bus[2]);
	cx23885_call_i2c_clients (&dev->i2c_bus[0], TUNER_SET_STANDBY, NULL);
	cx23885_card_setup(dev);
	cx23885_ir_init(dev);

	if (cx23885_boards[dev->board].porta == CX23885_ANALOG_VIDEO) {
		if (cx23885_video_register(dev) < 0) {
			printk(KERN_ERR "%s() Failed to register analog "
				"video adapters on VID_A\n", __FUNCTION__);
		}
	}

	if (cx23885_boards[dev->board].portb == CX23885_MPEG_DVB) {
		if (cx23885_dvb_register(&dev->ts1) < 0) {
			printk(KERN_ERR "%s() Failed to register dvb adapters on VID_B\n",
			       __FUNCTION__);
		}
	}

	if (cx23885_boards[dev->board].portc == CX23885_MPEG_DVB) {
		if (cx23885_dvb_register(&dev->ts2) < 0) {
			printk(KERN_ERR "%s() Failed to register dvb adapters on VID_C\n",
			       __FUNCTION__);
		}
	}

	cx23885_dev_checkrevision(dev);

	return 0;
}

static void cx23885_dev_unregister(struct cx23885_dev *dev)
{
	release_mem_region(pci_resource_start(dev->pci,0),
			   pci_resource_len(dev->pci,0));

	if (!atomic_dec_and_test(&dev->refcount))
		return;

	if (cx23885_boards[dev->board].porta == CX23885_ANALOG_VIDEO)
		cx23885_video_unregister(dev);

	if(cx23885_boards[dev->board].portb == CX23885_MPEG_DVB)
		cx23885_dvb_unregister(&dev->ts1);

	if(cx23885_boards[dev->board].portc == CX23885_MPEG_DVB)
		cx23885_dvb_unregister(&dev->ts2);

	cx23885_i2c_unregister(&dev->i2c_bus[2]);
	cx23885_i2c_unregister(&dev->i2c_bus[1]);
	cx23885_i2c_unregister(&dev->i2c_bus[0]);

	iounmap(dev->lmmio);
}

static u32* cx23885_risc_field(u32 *rp, struct scatterlist *sglist,
			       unsigned int offset, u32 sync_line,
			       unsigned int bpl, unsigned int padding,
			       unsigned int lines)
{
	struct scatterlist *sg;
	unsigned int line, todo;

	/* sync instruction */
	if (sync_line != NO_SYNC_LINE)
		*(rp++) = cpu_to_le32(RISC_RESYNC | sync_line);

	/* scan lines */
	sg = sglist;
	for (line = 0; line < lines; line++) {
		while (offset && offset >= sg_dma_len(sg)) {
			offset -= sg_dma_len(sg);
			sg++;
		}
		if (bpl <= sg_dma_len(sg)-offset) {
			/* fits into current chunk */
			*(rp++)=cpu_to_le32(RISC_WRITE|RISC_SOL|RISC_EOL|bpl);
			*(rp++)=cpu_to_le32(sg_dma_address(sg)+offset);
			*(rp++)=cpu_to_le32(0); /* bits 63-32 */
			offset+=bpl;
		} else {
			/* scanline needs to be split */
			todo = bpl;
			*(rp++)=cpu_to_le32(RISC_WRITE|RISC_SOL|
					    (sg_dma_len(sg)-offset));
			*(rp++)=cpu_to_le32(sg_dma_address(sg)+offset);
			*(rp++)=cpu_to_le32(0); /* bits 63-32 */
			todo -= (sg_dma_len(sg)-offset);
			offset = 0;
			sg++;
			while (todo > sg_dma_len(sg)) {
				*(rp++)=cpu_to_le32(RISC_WRITE|
						    sg_dma_len(sg));
				*(rp++)=cpu_to_le32(sg_dma_address(sg));
				*(rp++)=cpu_to_le32(0); /* bits 63-32 */
				todo -= sg_dma_len(sg);
				sg++;
			}
			*(rp++)=cpu_to_le32(RISC_WRITE|RISC_EOL|todo);
			*(rp++)=cpu_to_le32(sg_dma_address(sg));
			*(rp++)=cpu_to_le32(0); /* bits 63-32 */
			offset += todo;
		}
		offset += padding;
	}

	return rp;
}

int cx23885_risc_buffer(struct pci_dev *pci, struct btcx_riscmem *risc,
			struct scatterlist *sglist, unsigned int top_offset,
			unsigned int bottom_offset, unsigned int bpl,
			unsigned int padding, unsigned int lines)
{
	u32 instructions, fields;
	u32 *rp;
	int rc;

	fields = 0;
	if (UNSET != top_offset)
		fields++;
	if (UNSET != bottom_offset)
		fields++;

	/* estimate risc mem: worst case is one write per page border +
	   one write per scan line + syncs + jump (all 2 dwords).  Padding
	   can cause next bpl to start close to a page border.  First DMA
	   region may be smaller than PAGE_SIZE */
	/* write and jump need and extra dword */
	instructions  = fields * (1 + ((bpl + padding) * lines) / PAGE_SIZE + lines);
	instructions += 2;
	if ((rc = btcx_riscmem_alloc(pci,risc,instructions*12)) < 0)
		return rc;

	/* write risc instructions */
	rp = risc->cpu;
	if (UNSET != top_offset)
		rp = cx23885_risc_field(rp, sglist, top_offset, 0,
					bpl, padding, lines);
	if (UNSET != bottom_offset)
		rp = cx23885_risc_field(rp, sglist, bottom_offset, 0x200,
					bpl, padding, lines);

	/* save pointer to jmp instruction address */
	risc->jmp = rp;
	BUG_ON((risc->jmp - risc->cpu + 2) * sizeof (*risc->cpu) > risc->size);
	return 0;
}

static int cx23885_risc_databuffer(struct pci_dev *pci,
				   struct btcx_riscmem *risc,
				   struct scatterlist *sglist,
				   unsigned int bpl,
				   unsigned int lines)
{
	u32 instructions;
	u32 *rp;
	int rc;

	/* estimate risc mem: worst case is one write per page border +
	   one write per scan line + syncs + jump (all 2 dwords).  Here
	   there is no padding and no sync.  First DMA region may be smaller
	   than PAGE_SIZE */
	/* Jump and write need an extra dword */
	instructions  = 1 + (bpl * lines) / PAGE_SIZE + lines;
	instructions += 1;

	if ((rc = btcx_riscmem_alloc(pci,risc,instructions*12)) < 0)
		return rc;

	/* write risc instructions */
	rp = risc->cpu;
	rp = cx23885_risc_field(rp, sglist, 0, NO_SYNC_LINE, bpl, 0, lines);

	/* save pointer to jmp instruction address */
	risc->jmp = rp;
	BUG_ON((risc->jmp - risc->cpu + 2) * sizeof (*risc->cpu) > risc->size);
	return 0;
}

int cx23885_risc_stopper(struct pci_dev *pci, struct btcx_riscmem *risc,
				u32 reg, u32 mask, u32 value)
{
	u32 *rp;
	int rc;

	if ((rc = btcx_riscmem_alloc(pci, risc, 4*16)) < 0)
		return rc;

	/* write risc instructions */
	rp = risc->cpu;
	*(rp++) = cpu_to_le32(RISC_WRITECR  | RISC_IRQ2);
	*(rp++) = cpu_to_le32(reg);
	*(rp++) = cpu_to_le32(value);
	*(rp++) = cpu_to_le32(mask);
	*(rp++) = cpu_to_le32(RISC_JUMP);
	*(rp++) = cpu_to_le32(risc->dma);
	*(rp++) = cpu_to_le32(0); /* bits 63-32 */
	return 0;
}

void cx23885_free_buffer(struct videobuf_queue *q, struct cx23885_buffer *buf)
{
	struct videobuf_dmabuf *dma = videobuf_to_dma(&buf->vb);

	BUG_ON(in_interrupt());
	videobuf_waiton(&buf->vb, 0, 0);
	videobuf_dma_unmap(q, dma);
	videobuf_dma_free(dma);
	btcx_riscmem_free((struct pci_dev *)q->dev, &buf->risc);
	buf->vb.state = VIDEOBUF_NEEDS_INIT;
}

static void cx23885_tsport_reg_dump(struct cx23885_tsport *port)
{
	struct cx23885_dev *dev = port->dev;

	dprintk(1, "%s() Register Dump\n", __FUNCTION__);
	dprintk(1, "%s() DEV_CNTRL2               0x%08X\n", __FUNCTION__,
		cx_read(DEV_CNTRL2));
	dprintk(1, "%s() PCI_INT_MSK              0x%08X\n", __FUNCTION__,
		cx_read(PCI_INT_MSK));
	dprintk(1, "%s() AUD_INT_INT_MSK          0x%08X\n", __FUNCTION__,
		cx_read(AUDIO_INT_INT_MSK));
	dprintk(1, "%s() AUD_INT_DMA_CTL          0x%08X\n", __FUNCTION__,
		cx_read(AUD_INT_DMA_CTL));
	dprintk(1, "%s() AUD_EXT_INT_MSK          0x%08X\n", __FUNCTION__,
		cx_read(AUDIO_EXT_INT_MSK));
	dprintk(1, "%s() AUD_EXT_DMA_CTL          0x%08X\n", __FUNCTION__,
		cx_read(AUD_EXT_DMA_CTL));
	dprintk(1, "%s() PAD_CTRL                 0x%08X\n", __FUNCTION__,
		cx_read(PAD_CTRL));
	dprintk(1, "%s() ALT_PIN_OUT_SEL          0x%08X\n", __FUNCTION__,
		cx_read(ALT_PIN_OUT_SEL));
	dprintk(1, "%s() GPIO2                    0x%08X\n", __FUNCTION__,
		cx_read(GPIO2));
	dprintk(1, "%s() gpcnt(0x%08X)          0x%08X\n", __FUNCTION__,
		port->reg_gpcnt, cx_read(port->reg_gpcnt));
	dprintk(1, "%s() gpcnt_ctl(0x%08X)      0x%08x\n", __FUNCTION__,
		port->reg_gpcnt_ctl, cx_read(port->reg_gpcnt_ctl));
	dprintk(1, "%s() dma_ctl(0x%08X)        0x%08x\n", __FUNCTION__,
		port->reg_dma_ctl, cx_read(port->reg_dma_ctl));
	dprintk(1, "%s() src_sel(0x%08X)        0x%08x\n", __FUNCTION__,
		port->reg_src_sel, cx_read(port->reg_src_sel));
	dprintk(1, "%s() lngth(0x%08X)          0x%08x\n", __FUNCTION__,
		port->reg_lngth, cx_read(port->reg_lngth));
	dprintk(1, "%s() hw_sop_ctrl(0x%08X)    0x%08x\n", __FUNCTION__,
		port->reg_hw_sop_ctrl, cx_read(port->reg_hw_sop_ctrl));
	dprintk(1, "%s() gen_ctrl(0x%08X)       0x%08x\n", __FUNCTION__,
		port->reg_gen_ctrl, cx_read(port->reg_gen_ctrl));
	dprintk(1, "%s() bd_pkt_status(0x%08X)  0x%08x\n", __FUNCTION__,
		port->reg_bd_pkt_status, cx_read(port->reg_bd_pkt_status));
	dprintk(1, "%s() sop_status(0x%08X)     0x%08x\n", __FUNCTION__,
		port->reg_sop_status, cx_read(port->reg_sop_status));
	dprintk(1, "%s() fifo_ovfl_stat(0x%08X) 0x%08x\n", __FUNCTION__,
		port->reg_fifo_ovfl_stat, cx_read(port->reg_fifo_ovfl_stat));
	dprintk(1, "%s() vld_misc(0x%08X)       0x%08x\n", __FUNCTION__,
		port->reg_vld_misc, cx_read(port->reg_vld_misc));
	dprintk(1, "%s() ts_clk_en(0x%08X)      0x%08x\n", __FUNCTION__,
		port->reg_ts_clk_en, cx_read(port->reg_ts_clk_en));
	dprintk(1, "%s() ts_int_msk(0x%08X)     0x%08x\n", __FUNCTION__,
		port->reg_ts_int_msk, cx_read(port->reg_ts_int_msk));
}

static int cx23885_start_dma(struct cx23885_tsport *port,
			     struct cx23885_dmaqueue *q,
			     struct cx23885_buffer   *buf)
{
	struct cx23885_dev *dev = port->dev;

	dprintk(1, "%s() w: %d, h: %d, f: %d\n", __FUNCTION__,
		buf->vb.width, buf->vb.height, buf->vb.field);

	/* setup fifo + format */
	cx23885_sram_channel_setup(dev,
				   &dev->sram_channels[ port->sram_chno ],
				   port->ts_packet_size, buf->risc.dma);
	if(debug > 5) {
		cx23885_sram_channel_dump(dev, &dev->sram_channels[ port->sram_chno ] );
		cx23885_risc_disasm(port, &buf->risc);
	}

	/* write TS length to chip */
	cx_write(port->reg_lngth, buf->vb.width);

	if ( (!(cx23885_boards[dev->board].portb & CX23885_MPEG_DVB)) &&
		(!(cx23885_boards[dev->board].portc & CX23885_MPEG_DVB)) ) {
		printk( "%s() Failed. Unsupported value in .portb/c (0x%08x)/(0x%08x)\n",
			__FUNCTION__,
			cx23885_boards[dev->board].portb,
			cx23885_boards[dev->board].portc );
		return -EINVAL;
	}

	udelay(100);

	/* If the port supports SRC SELECT, configure it */
	if(port->reg_src_sel)
		cx_write(port->reg_src_sel, port->src_sel_val);

	cx_write(port->reg_hw_sop_ctrl, 0x47 << 16 | 188 << 4);
	cx_write(port->reg_ts_clk_en, port->ts_clk_en_val);
	cx_write(port->reg_vld_misc, 0x00);
	cx_write(port->reg_gen_ctrl, port->gen_ctrl_val);
	udelay(100);

	// NOTE: this is 2 (reserved) for portb, does it matter?
	/* reset counter to zero */
	cx_write(port->reg_gpcnt_ctl, 3);
	q->count = 1;

	switch(dev->bridge) {
	case CX23885_BRIDGE_885:
	case CX23885_BRIDGE_887:
		/* enable irqs */
		dprintk(1, "%s() enabling TS int's and DMA\n", __FUNCTION__ );
		cx_set(port->reg_ts_int_msk,  port->ts_int_msk_val);
		cx_set(port->reg_dma_ctl, port->dma_ctl_val);
		cx_set(PCI_INT_MSK, dev->pci_irqmask | port->pci_irqmask);
		break;
	default:
		BUG();
	}

	cx_set(DEV_CNTRL2, (1<<5)); /* Enable RISC controller */

	if (debug > 4)
		cx23885_tsport_reg_dump(port);

	return 0;
}

static int cx23885_stop_dma(struct cx23885_tsport *port)
{
	struct cx23885_dev *dev = port->dev;
	dprintk(1, "%s()\n", __FUNCTION__);

	/* Stop interrupts and DMA */
	cx_clear(port->reg_ts_int_msk, port->ts_int_msk_val);
	cx_clear(port->reg_dma_ctl, port->dma_ctl_val);

	return 0;
}

int cx23885_restart_queue(struct cx23885_tsport *port,
				struct cx23885_dmaqueue *q)
{
	struct cx23885_dev *dev = port->dev;
	struct cx23885_buffer *buf;

	dprintk(5, "%s()\n", __FUNCTION__);
	if (list_empty(&q->active))
	{
		struct cx23885_buffer *prev;
		prev = NULL;

		dprintk(5, "%s() queue is empty\n", __FUNCTION__);

		for (;;) {
			if (list_empty(&q->queued))
				return 0;
			buf = list_entry(q->queued.next, struct cx23885_buffer,
					 vb.queue);
			if (NULL == prev) {
				list_del(&buf->vb.queue);
				list_add_tail(&buf->vb.queue, &q->active);
				cx23885_start_dma(port, q, buf);
				buf->vb.state = VIDEOBUF_ACTIVE;
				buf->count    = q->count++;
				mod_timer(&q->timeout, jiffies+BUFFER_TIMEOUT);
				dprintk(5, "[%p/%d] restart_queue - first active\n",
					buf, buf->vb.i);

			} else if (prev->vb.width  == buf->vb.width  &&
				   prev->vb.height == buf->vb.height &&
				   prev->fmt       == buf->fmt) {
				list_del(&buf->vb.queue);
				list_add_tail(&buf->vb.queue, &q->active);
				buf->vb.state = VIDEOBUF_ACTIVE;
				buf->count    = q->count++;
				prev->risc.jmp[1] = cpu_to_le32(buf->risc.dma);
				prev->risc.jmp[2] = cpu_to_le32(0); /* 64 bit bits 63-32 */
				dprintk(5,"[%p/%d] restart_queue - move to active\n",
					buf, buf->vb.i);
			} else {
				return 0;
			}
			prev = buf;
		}
		return 0;
	}

	buf = list_entry(q->active.next, struct cx23885_buffer, vb.queue);
	dprintk(2, "restart_queue [%p/%d]: restart dma\n",
		buf, buf->vb.i);
	cx23885_start_dma(port, q, buf);
	list_for_each_entry(buf, &q->active, vb.queue)
		buf->count = q->count++;
	mod_timer(&q->timeout, jiffies + BUFFER_TIMEOUT);
	return 0;
}

/* ------------------------------------------------------------------ */

int cx23885_buf_prepare(struct videobuf_queue *q, struct cx23885_tsport *port,
			struct cx23885_buffer *buf, enum v4l2_field field)
{
	struct cx23885_dev *dev = port->dev;
	int size = port->ts_packet_size * port->ts_packet_count;
	int rc;

	dprintk(1, "%s: %p\n", __FUNCTION__, buf);
	if (0 != buf->vb.baddr  &&  buf->vb.bsize < size)
		return -EINVAL;

	if (VIDEOBUF_NEEDS_INIT == buf->vb.state) {
		buf->vb.width  = port->ts_packet_size;
		buf->vb.height = port->ts_packet_count;
		buf->vb.size   = size;
		buf->vb.field  = field /*V4L2_FIELD_TOP*/;

		if (0 != (rc = videobuf_iolock(q, &buf->vb, NULL)))
			goto fail;
		cx23885_risc_databuffer(dev->pci, &buf->risc,
					videobuf_to_dma(&buf->vb)->sglist,
					buf->vb.width, buf->vb.height);
	}
	buf->vb.state = VIDEOBUF_PREPARED;
	return 0;

 fail:
	cx23885_free_buffer(q, buf);
	return rc;
}

void cx23885_buf_queue(struct cx23885_tsport *port, struct cx23885_buffer *buf)
{
	struct cx23885_buffer    *prev;
	struct cx23885_dev *dev = port->dev;
	struct cx23885_dmaqueue  *cx88q = &port->mpegq;

	/* add jump to stopper */
	buf->risc.jmp[0] = cpu_to_le32(RISC_JUMP | RISC_IRQ1 | RISC_CNT_INC);
	buf->risc.jmp[1] = cpu_to_le32(cx88q->stopper.dma);
	buf->risc.jmp[2] = cpu_to_le32(0); /* bits 63-32 */

	if (list_empty(&cx88q->active)) {
		dprintk( 1, "queue is empty - first active\n" );
		list_add_tail(&buf->vb.queue, &cx88q->active);
		cx23885_start_dma(port, cx88q, buf);
		buf->vb.state = VIDEOBUF_ACTIVE;
		buf->count    = cx88q->count++;
		mod_timer(&cx88q->timeout, jiffies + BUFFER_TIMEOUT);
		dprintk(1, "[%p/%d] %s - first active\n",
			buf, buf->vb.i, __FUNCTION__);
	} else {
		dprintk( 1, "queue is not empty - append to active\n" );
		prev = list_entry(cx88q->active.prev, struct cx23885_buffer,
				  vb.queue);
		list_add_tail(&buf->vb.queue, &cx88q->active);
		buf->vb.state = VIDEOBUF_ACTIVE;
		buf->count    = cx88q->count++;
		prev->risc.jmp[1] = cpu_to_le32(buf->risc.dma);
		prev->risc.jmp[2] = cpu_to_le32(0); /* 64 bit bits 63-32 */
		dprintk( 1, "[%p/%d] %s - append to active\n",
			 buf, buf->vb.i, __FUNCTION__);
	}
}

/* ----------------------------------------------------------- */

static void do_cancel_buffers(struct cx23885_tsport *port, char *reason,
			      int restart)
{
	struct cx23885_dev *dev = port->dev;
	struct cx23885_dmaqueue *q = &port->mpegq;
	struct cx23885_buffer *buf;
	unsigned long flags;

	spin_lock_irqsave(&port->slock, flags);
	while (!list_empty(&q->active)) {
		buf = list_entry(q->active.next, struct cx23885_buffer,
				 vb.queue);
		list_del(&buf->vb.queue);
		buf->vb.state = VIDEOBUF_ERROR;
		wake_up(&buf->vb.done);
		dprintk(1, "[%p/%d] %s - dma=0x%08lx\n",
			buf, buf->vb.i, reason, (unsigned long)buf->risc.dma);
	}
	if (restart) {
		dprintk(1, "restarting queue\n" );
		cx23885_restart_queue(port, q);
	}
	spin_unlock_irqrestore(&port->slock, flags);
}


static void cx23885_timeout(unsigned long data)
{
	struct cx23885_tsport *port = (struct cx23885_tsport *)data;
	struct cx23885_dev *dev = port->dev;

	dprintk(1, "%s()\n",__FUNCTION__);

	if (debug > 5)
		cx23885_sram_channel_dump(dev, &dev->sram_channels[ port->sram_chno ]);

	cx23885_stop_dma(port);
	do_cancel_buffers(port, "timeout", 1);
}

static int cx23885_irq_ts(struct cx23885_tsport *port, u32 status)
{
	struct cx23885_dev *dev = port->dev;
	int handled = 0;
	u32 count;

	if ( (status & VID_BC_MSK_OPC_ERR) ||
	     (status & VID_BC_MSK_BAD_PKT) ||
	     (status & VID_BC_MSK_SYNC) ||
	     (status & VID_BC_MSK_OF))
	{
		if (status & VID_BC_MSK_OPC_ERR)
			dprintk(7, " (VID_BC_MSK_OPC_ERR 0x%08x)\n", VID_BC_MSK_OPC_ERR);
		if (status & VID_BC_MSK_BAD_PKT)
			dprintk(7, " (VID_BC_MSK_BAD_PKT 0x%08x)\n", VID_BC_MSK_BAD_PKT);
		if (status & VID_BC_MSK_SYNC)
			dprintk(7, " (VID_BC_MSK_SYNC    0x%08x)\n", VID_BC_MSK_SYNC);
		if (status & VID_BC_MSK_OF)
			dprintk(7, " (VID_BC_MSK_OF      0x%08x)\n", VID_BC_MSK_OF);

		printk(KERN_ERR "%s: mpeg risc op code error\n", dev->name);

		cx_clear(port->reg_dma_ctl, port->dma_ctl_val);
		cx23885_sram_channel_dump(dev, &dev->sram_channels[ port->sram_chno ]);

	} else if (status & VID_BC_MSK_RISCI1) {

		dprintk(7, " (RISCI1            0x%08x)\n", VID_BC_MSK_RISCI1);

		spin_lock(&port->slock);
		count = cx_read(port->reg_gpcnt);
		cx23885_wakeup(port, &port->mpegq, count);
		spin_unlock(&port->slock);

	} else if (status & VID_BC_MSK_RISCI2) {

		dprintk(7, " (RISCI2            0x%08x)\n", VID_BC_MSK_RISCI2);

		spin_lock(&port->slock);
		cx23885_restart_queue(port, &port->mpegq);
		spin_unlock(&port->slock);

	}
	if (status) {
		cx_write(port->reg_ts_int_stat, status);
		handled = 1;
	}

	return handled;
}

static irqreturn_t cx23885_irq(int irq, void *dev_id)
{
	struct cx23885_dev *dev = dev_id;
	struct cx23885_tsport *ts1 = &dev->ts1;
	struct cx23885_tsport *ts2 = &dev->ts2;
	u32 pci_status, pci_mask;
	u32 vida_status, vida_mask;
	u32 ts1_status, ts1_mask;
	u32 ts2_status, ts2_mask;
	int vida_count = 0, ts1_count = 0, ts2_count = 0, handled = 0;

	pci_status = cx_read(PCI_INT_STAT);
	pci_mask = cx_read(PCI_INT_MSK);
	vida_status = cx_read(VID_A_INT_STAT);
	vida_mask = cx_read(VID_A_INT_MSK);
	ts1_status = cx_read(VID_B_INT_STAT);
	ts1_mask = cx_read(VID_B_INT_MSK);
	ts2_status = cx_read(VID_C_INT_STAT);
	ts2_mask = cx_read(VID_C_INT_MSK);

	if ( (pci_status == 0) && (ts2_status == 0) && (ts1_status == 0) )
		goto out;

	vida_count = cx_read(VID_A_GPCNT);
	ts1_count = cx_read(ts1->reg_gpcnt);
	ts2_count = cx_read(ts2->reg_gpcnt);
	dprintk(7, "pci_status: 0x%08x  pci_mask: 0x%08x\n",
		pci_status, pci_mask);
	dprintk(7, "vida_status: 0x%08x vida_mask: 0x%08x count: 0x%x\n",
		vida_status, vida_mask, vida_count);
	dprintk(7, "ts1_status: 0x%08x  ts1_mask: 0x%08x count: 0x%x\n",
		ts1_status, ts1_mask, ts1_count);
	dprintk(7, "ts2_status: 0x%08x  ts2_mask: 0x%08x count: 0x%x\n",
		ts2_status, ts2_mask, ts2_count);

	if ( (pci_status & PCI_MSK_RISC_RD) ||
	     (pci_status & PCI_MSK_RISC_WR) ||
	     (pci_status & PCI_MSK_AL_RD) ||
	     (pci_status & PCI_MSK_AL_WR) ||
	     (pci_status & PCI_MSK_APB_DMA) ||
	     (pci_status & PCI_MSK_VID_C) ||
	     (pci_status & PCI_MSK_VID_B) ||
	     (pci_status & PCI_MSK_VID_A) ||
	     (pci_status & PCI_MSK_AUD_INT) ||
	     (pci_status & PCI_MSK_AUD_EXT) )
	{

		if (pci_status & PCI_MSK_RISC_RD)
			dprintk(7, " (PCI_MSK_RISC_RD   0x%08x)\n", PCI_MSK_RISC_RD);
		if (pci_status & PCI_MSK_RISC_WR)
			dprintk(7, " (PCI_MSK_RISC_WR   0x%08x)\n", PCI_MSK_RISC_WR);
		if (pci_status & PCI_MSK_AL_RD)
			dprintk(7, " (PCI_MSK_AL_RD     0x%08x)\n", PCI_MSK_AL_RD);
		if (pci_status & PCI_MSK_AL_WR)
			dprintk(7, " (PCI_MSK_AL_WR     0x%08x)\n", PCI_MSK_AL_WR);
		if (pci_status & PCI_MSK_APB_DMA)
			dprintk(7, " (PCI_MSK_APB_DMA   0x%08x)\n", PCI_MSK_APB_DMA);
		if (pci_status & PCI_MSK_VID_C)
			dprintk(7, " (PCI_MSK_VID_C     0x%08x)\n", PCI_MSK_VID_C);
		if (pci_status & PCI_MSK_VID_B)
			dprintk(7, " (PCI_MSK_VID_B     0x%08x)\n", PCI_MSK_VID_B);
		if (pci_status & PCI_MSK_VID_A)
			dprintk(7, " (PCI_MSK_VID_A     0x%08x)\n", PCI_MSK_VID_A);
		if (pci_status & PCI_MSK_AUD_INT)
			dprintk(7, " (PCI_MSK_AUD_INT   0x%08x)\n", PCI_MSK_AUD_INT);
		if (pci_status & PCI_MSK_AUD_EXT)
			dprintk(7, " (PCI_MSK_AUD_EXT   0x%08x)\n", PCI_MSK_AUD_EXT);

	}

	if (ts1_status) {
		if (cx23885_boards[dev->board].portb == CX23885_MPEG_DVB)
			handled += cx23885_irq_ts(ts1, ts1_status);
	}

	if (ts2_status) {
		if (cx23885_boards[dev->board].portc == CX23885_MPEG_DVB)
			handled += cx23885_irq_ts(ts2, ts2_status);
	}

	if (vida_status)
		handled += cx23885_video_irq(dev, vida_status);

	if (handled)
		cx_write(PCI_INT_STAT, pci_status);
out:
	return IRQ_RETVAL(handled);
}

static int __devinit cx23885_initdev(struct pci_dev *pci_dev,
				     const struct pci_device_id *pci_id)
{
	struct cx23885_dev *dev;
	int err;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (NULL == dev)
		return -ENOMEM;

	/* pci init */
	dev->pci = pci_dev;
	if (pci_enable_device(pci_dev)) {
		err = -EIO;
		goto fail_free;
	}

	if (cx23885_dev_setup(dev) < 0) {
		err = -EINVAL;
		goto fail_free;
	}

	/* print pci info */
	pci_read_config_byte(pci_dev, PCI_CLASS_REVISION, &dev->pci_rev);
	pci_read_config_byte(pci_dev, PCI_LATENCY_TIMER,  &dev->pci_lat);
	printk(KERN_INFO "%s/0: found at %s, rev: %d, irq: %d, "
	       "latency: %d, mmio: 0x%llx\n", dev->name,
	       pci_name(pci_dev), dev->pci_rev, pci_dev->irq,
	       dev->pci_lat, (unsigned long long)pci_resource_start(pci_dev,0));

	pci_set_master(pci_dev);
	if (!pci_dma_supported(pci_dev, 0xffffffff)) {
		printk("%s/0: Oops: no 32bit PCI DMA ???\n", dev->name);
		err = -EIO;
		goto fail_irq;
	}

	err = request_irq(pci_dev->irq, cx23885_irq,
			  IRQF_SHARED | IRQF_DISABLED, dev->name, dev);
	if (err < 0) {
		printk(KERN_ERR "%s: can't get IRQ %d\n",
		       dev->name, pci_dev->irq);
		goto fail_irq;
	}

	pci_set_drvdata(pci_dev, dev);
	return 0;

fail_irq:
	cx23885_dev_unregister(dev);
fail_free:
	kfree(dev);
	return err;
}

static void __devexit cx23885_finidev(struct pci_dev *pci_dev)
{
	struct cx23885_dev *dev = pci_get_drvdata(pci_dev);

	cx23885_shutdown(dev);

	pci_disable_device(pci_dev);

	/* unregister stuff */
	free_irq(pci_dev->irq, dev);
	pci_set_drvdata(pci_dev, NULL);

	mutex_lock(&devlist);
	list_del(&dev->devlist);
	mutex_unlock(&devlist);

	cx23885_dev_unregister(dev);
	kfree(dev);
}

static struct pci_device_id cx23885_pci_tbl[] = {
	{
		/* CX23885 */
		.vendor       = 0x14f1,
		.device       = 0x8852,
		.subvendor    = PCI_ANY_ID,
		.subdevice    = PCI_ANY_ID,
	},{
		/* CX23887 Rev 2 */
		.vendor       = 0x14f1,
		.device       = 0x8880,
		.subvendor    = PCI_ANY_ID,
		.subdevice    = PCI_ANY_ID,
	},{
		/* --- end of list --- */
	}
};
MODULE_DEVICE_TABLE(pci, cx23885_pci_tbl);

static struct pci_driver cx23885_pci_driver = {
	.name     = "cx23885",
	.id_table = cx23885_pci_tbl,
	.probe    = cx23885_initdev,
	.remove   = __devexit_p(cx23885_finidev),
	/* TODO */
	.suspend  = NULL,
	.resume   = NULL,
};

static int cx23885_init(void)
{
	printk(KERN_INFO "cx23885 driver version %d.%d.%d loaded\n",
	       (CX23885_VERSION_CODE >> 16) & 0xff,
	       (CX23885_VERSION_CODE >>  8) & 0xff,
	       CX23885_VERSION_CODE & 0xff);
#ifdef SNAPSHOT
	printk(KERN_INFO "cx23885: snapshot date %04d-%02d-%02d\n",
	       SNAPSHOT/10000, (SNAPSHOT/100)%100, SNAPSHOT%100);
#endif
	return pci_register_driver(&cx23885_pci_driver);
}

static void cx23885_fini(void)
{
	pci_unregister_driver(&cx23885_pci_driver);
}

module_init(cx23885_init);
module_exit(cx23885_fini);

/* ----------------------------------------------------------- */
/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 * kate: eol "unix"; indent-width 3; remove-trailing-space on; replace-trailing-space-save on; tab-width 8; replace-tabs off; space-indent off; mixed-indent off
 */
