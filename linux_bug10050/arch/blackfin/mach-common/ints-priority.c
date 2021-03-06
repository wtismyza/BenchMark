/*
 * File:         arch/blackfin/mach-common/ints-priority.c
 * Based on:
 * Author:
 *
 * Created:      ?
 * Description:  Set up the interrupt priorities
 *
 * Modified:
 *               1996 Roman Zippel
 *               1999 D. Jeff Dionne <jeff@uclinux.org>
 *               2000-2001 Lineo, Inc. D. Jefff Dionne <jeff@lineo.ca>
 *               2002 Arcturus Networks Inc. MaTed <mated@sympatico.ca>
 *               2003 Metrowerks/Motorola
 *               2003 Bas Vermeulen <bas@buyways.nl>
 *               Copyright 2004-2008 Analog Devices Inc.
 *
 * Bugs:         Enter bugs at http://blackfin.uclinux.org/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see the file COPYING, or write
 * to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <linux/module.h>
#include <linux/kernel_stat.h>
#include <linux/seq_file.h>
#include <linux/irq.h>
#ifdef CONFIG_KGDB
#include <linux/kgdb.h>
#endif
#include <asm/traps.h>
#include <asm/blackfin.h>
#include <asm/gpio.h>
#include <asm/irq_handler.h>

#ifdef BF537_FAMILY
# define BF537_GENERIC_ERROR_INT_DEMUX
#else
# undef BF537_GENERIC_ERROR_INT_DEMUX
#endif

/*
 * NOTES:
 * - we have separated the physical Hardware interrupt from the
 * levels that the LINUX kernel sees (see the description in irq.h)
 * -
 */

/* Initialize this to an actual value to force it into the .data
 * section so that we know it is properly initialized at entry into
 * the kernel but before bss is initialized to zero (which is where
 * it would live otherwise).  The 0x1f magic represents the IRQs we
 * cannot actually mask out in hardware.
 */
unsigned long irq_flags = 0x1f;

/* The number of spurious interrupts */
atomic_t num_spurious;

#ifdef CONFIG_PM
unsigned long bfin_sic_iwr[3];	/* Up to 3 SIC_IWRx registers */
#endif

struct ivgx {
	/* irq number for request_irq, available in mach-bf533/irq.h */
	unsigned int irqno;
	/* corresponding bit in the SIC_ISR register */
	unsigned int isrflag;
} ivg_table[NR_PERI_INTS];

struct ivg_slice {
	/* position of first irq in ivg_table for given ivg */
	struct ivgx *ifirst;
	struct ivgx *istop;
} ivg7_13[IVG13 - IVG7 + 1];

static void search_IAR(void);

/*
 * Search SIC_IAR and fill tables with the irqvalues
 * and their positions in the SIC_ISR register.
 */
static void __init search_IAR(void)
{
	unsigned ivg, irq_pos = 0;
	for (ivg = 0; ivg <= IVG13 - IVG7; ivg++) {
		int irqn;

		ivg7_13[ivg].istop = ivg7_13[ivg].ifirst = &ivg_table[irq_pos];

		for (irqn = 0; irqn < NR_PERI_INTS; irqn++) {
			int iar_shift = (irqn & 7) * 4;
				if (ivg == (0xf &
#ifndef CONFIG_BF52x
			     bfin_read32((unsigned long *)SIC_IAR0 +
					 (irqn >> 3)) >> iar_shift)) {
#else
			     bfin_read32((unsigned long *)SIC_IAR0 +
					 ((irqn%32) >> 3) + ((irqn / 32) * 16)) >> iar_shift)) {
#endif
				ivg_table[irq_pos].irqno = IVG7 + irqn;
				ivg_table[irq_pos].isrflag = 1 << (irqn % 32);
				ivg7_13[ivg].istop++;
				irq_pos++;
			}
		}
	}
}

/*
 * This is for BF533 internal IRQs
 */

static void ack_noop(unsigned int irq)
{
	/* Dummy function.  */
}

static void bfin_core_mask_irq(unsigned int irq)
{
	irq_flags &= ~(1 << irq);
	if (!irqs_disabled())
		local_irq_enable();
}

static void bfin_core_unmask_irq(unsigned int irq)
{
	irq_flags |= 1 << irq;
	/*
	 * If interrupts are enabled, IMASK must contain the same value
	 * as irq_flags.  Make sure that invariant holds.  If interrupts
	 * are currently disabled we need not do anything; one of the
	 * callers will take care of setting IMASK to the proper value
	 * when reenabling interrupts.
	 * local_irq_enable just does "STI irq_flags", so it's exactly
	 * what we need.
	 */
	if (!irqs_disabled())
		local_irq_enable();
	return;
}

static void bfin_internal_mask_irq(unsigned int irq)
{
#ifdef CONFIG_BF53x
	bfin_write_SIC_IMASK(bfin_read_SIC_IMASK() &
			     ~(1 << (irq - (IRQ_CORETMR + 1))));
#else
	unsigned mask_bank, mask_bit;
	mask_bank = (irq - (IRQ_CORETMR + 1)) / 32;
	mask_bit = (irq - (IRQ_CORETMR + 1)) % 32;
	bfin_write_SIC_IMASK(mask_bank, bfin_read_SIC_IMASK(mask_bank) &
			     ~(1 << mask_bit));
#endif
	SSYNC();
}

static void bfin_internal_unmask_irq(unsigned int irq)
{
#ifdef CONFIG_BF53x
	bfin_write_SIC_IMASK(bfin_read_SIC_IMASK() |
			     (1 << (irq - (IRQ_CORETMR + 1))));
#else
	unsigned mask_bank, mask_bit;
	mask_bank = (irq - (IRQ_CORETMR + 1)) / 32;
	mask_bit = (irq - (IRQ_CORETMR + 1)) % 32;
	bfin_write_SIC_IMASK(mask_bank, bfin_read_SIC_IMASK(mask_bank) |
			     (1 << mask_bit));
#endif
	SSYNC();
}

#ifdef CONFIG_PM
int bfin_internal_set_wake(unsigned int irq, unsigned int state)
{
	unsigned bank, bit;
	unsigned long flags;
	bank = (irq - (IRQ_CORETMR + 1)) / 32;
	bit = (irq - (IRQ_CORETMR + 1)) % 32;

	local_irq_save(flags);

	if (state)
		bfin_sic_iwr[bank] |= (1 << bit);
	else
		bfin_sic_iwr[bank] &= ~(1 << bit);

	local_irq_restore(flags);

	return 0;
}
#endif

static struct irq_chip bfin_core_irqchip = {
	.ack = ack_noop,
	.mask = bfin_core_mask_irq,
	.unmask = bfin_core_unmask_irq,
};

static struct irq_chip bfin_internal_irqchip = {
	.ack = ack_noop,
	.mask = bfin_internal_mask_irq,
	.unmask = bfin_internal_unmask_irq,
#ifdef CONFIG_PM
	.set_wake = bfin_internal_set_wake,
#endif
};

#ifdef BF537_GENERIC_ERROR_INT_DEMUX
static int error_int_mask;

static void bfin_generic_error_ack_irq(unsigned int irq)
{

}

static void bfin_generic_error_mask_irq(unsigned int irq)
{
	error_int_mask &= ~(1L << (irq - IRQ_PPI_ERROR));

	if (!error_int_mask) {
		local_irq_disable();
		bfin_write_SIC_IMASK(bfin_read_SIC_IMASK() &
				     ~(1 << (IRQ_GENERIC_ERROR -
					(IRQ_CORETMR + 1))));
		SSYNC();
		local_irq_enable();
	}
}

static void bfin_generic_error_unmask_irq(unsigned int irq)
{
	local_irq_disable();
	bfin_write_SIC_IMASK(bfin_read_SIC_IMASK() | 1 <<
			     (IRQ_GENERIC_ERROR - (IRQ_CORETMR + 1)));
	SSYNC();
	local_irq_enable();

	error_int_mask |= 1L << (irq - IRQ_PPI_ERROR);
}

static struct irq_chip bfin_generic_error_irqchip = {
	.ack = bfin_generic_error_ack_irq,
	.mask = bfin_generic_error_mask_irq,
	.unmask = bfin_generic_error_unmask_irq,
};

static void bfin_demux_error_irq(unsigned int int_err_irq,
				 struct irq_desc *inta_desc)
{
	int irq = 0;

	SSYNC();

#if (defined(CONFIG_BF537) || defined(CONFIG_BF536))
	if (bfin_read_EMAC_SYSTAT() & EMAC_ERR_MASK)
		irq = IRQ_MAC_ERROR;
	else
#endif
	if (bfin_read_SPORT0_STAT() & SPORT_ERR_MASK)
		irq = IRQ_SPORT0_ERROR;
	else if (bfin_read_SPORT1_STAT() & SPORT_ERR_MASK)
		irq = IRQ_SPORT1_ERROR;
	else if (bfin_read_PPI_STATUS() & PPI_ERR_MASK)
		irq = IRQ_PPI_ERROR;
	else if (bfin_read_CAN_GIF() & CAN_ERR_MASK)
		irq = IRQ_CAN_ERROR;
	else if (bfin_read_SPI_STAT() & SPI_ERR_MASK)
		irq = IRQ_SPI_ERROR;
	else if ((bfin_read_UART0_IIR() & UART_ERR_MASK_STAT1) &&
		 (bfin_read_UART0_IIR() & UART_ERR_MASK_STAT0))
		irq = IRQ_UART0_ERROR;
	else if ((bfin_read_UART1_IIR() & UART_ERR_MASK_STAT1) &&
		 (bfin_read_UART1_IIR() & UART_ERR_MASK_STAT0))
		irq = IRQ_UART1_ERROR;

	if (irq) {
		if (error_int_mask & (1L << (irq - IRQ_PPI_ERROR))) {
			struct irq_desc *desc = irq_desc + irq;
			desc->handle_irq(irq, desc);
		} else {

			switch (irq) {
			case IRQ_PPI_ERROR:
				bfin_write_PPI_STATUS(PPI_ERR_MASK);
				break;
#if (defined(CONFIG_BF537) || defined(CONFIG_BF536))
			case IRQ_MAC_ERROR:
				bfin_write_EMAC_SYSTAT(EMAC_ERR_MASK);
				break;
#endif
			case IRQ_SPORT0_ERROR:
				bfin_write_SPORT0_STAT(SPORT_ERR_MASK);
				break;

			case IRQ_SPORT1_ERROR:
				bfin_write_SPORT1_STAT(SPORT_ERR_MASK);
				break;

			case IRQ_CAN_ERROR:
				bfin_write_CAN_GIS(CAN_ERR_MASK);
				break;

			case IRQ_SPI_ERROR:
				bfin_write_SPI_STAT(SPI_ERR_MASK);
				break;

			default:
				break;
			}

			pr_debug("IRQ %d:"
				 " MASKED PERIPHERAL ERROR INTERRUPT ASSERTED\n",
				 irq);
		}
	} else
		printk(KERN_ERR
		       "%s : %s : LINE %d :\nIRQ ?: PERIPHERAL ERROR"
		       " INTERRUPT ASSERTED BUT NO SOURCE FOUND\n",
		       __FUNCTION__, __FILE__, __LINE__);

}
#endif				/* BF537_GENERIC_ERROR_INT_DEMUX */

#if !defined(CONFIG_BF54x)

static unsigned short gpio_enabled[gpio_bank(MAX_BLACKFIN_GPIOS)];
static unsigned short gpio_edge_triggered[gpio_bank(MAX_BLACKFIN_GPIOS)];


static void bfin_gpio_ack_irq(unsigned int irq)
{
	u16 gpionr = irq - IRQ_PF0;

	if (gpio_edge_triggered[gpio_bank(gpionr)] & gpio_bit(gpionr)) {
		set_gpio_data(gpionr, 0);
		SSYNC();
	}
}

static void bfin_gpio_mask_ack_irq(unsigned int irq)
{
	u16 gpionr = irq - IRQ_PF0;

	if (gpio_edge_triggered[gpio_bank(gpionr)] & gpio_bit(gpionr)) {
		set_gpio_data(gpionr, 0);
		SSYNC();
	}

	set_gpio_maska(gpionr, 0);
	SSYNC();
}

static void bfin_gpio_mask_irq(unsigned int irq)
{
	set_gpio_maska(irq - IRQ_PF0, 0);
	SSYNC();
}

static void bfin_gpio_unmask_irq(unsigned int irq)
{
	set_gpio_maska(irq - IRQ_PF0, 1);
	SSYNC();
}

static unsigned int bfin_gpio_irq_startup(unsigned int irq)
{
	unsigned int ret;
	u16 gpionr = irq - IRQ_PF0;
	char buf[8];

	if (!(gpio_enabled[gpio_bank(gpionr)] & gpio_bit(gpionr))) {
		snprintf(buf, sizeof buf, "IRQ %d", irq);
		ret = gpio_request(gpionr, buf);
		if (ret)
			return ret;
	}

	gpio_enabled[gpio_bank(gpionr)] |= gpio_bit(gpionr);
	bfin_gpio_unmask_irq(irq);

	return ret;
}

static void bfin_gpio_irq_shutdown(unsigned int irq)
{
	bfin_gpio_mask_irq(irq);
	gpio_free(irq - IRQ_PF0);
	gpio_enabled[gpio_bank(irq - IRQ_PF0)] &= ~gpio_bit(irq - IRQ_PF0);
}

static int bfin_gpio_irq_type(unsigned int irq, unsigned int type)
{

	unsigned int ret;
	char buf[8];
	u16 gpionr = irq - IRQ_PF0;

	if (type == IRQ_TYPE_PROBE) {
		/* only probe unenabled GPIO interrupt lines */
		if (gpio_enabled[gpio_bank(gpionr)] & gpio_bit(gpionr))
			return 0;
		type = IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING;
	}

	if (type & (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING |
		    IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW)) {
		if (!(gpio_enabled[gpio_bank(gpionr)] & gpio_bit(gpionr))) {
			snprintf(buf, sizeof buf, "IRQ %d", irq);
			ret = gpio_request(gpionr, buf);
			if (ret)
				return ret;
		}

		gpio_enabled[gpio_bank(gpionr)] |= gpio_bit(gpionr);
	} else {
		gpio_enabled[gpio_bank(gpionr)] &= ~gpio_bit(gpionr);
		return 0;
	}

	set_gpio_inen(gpionr, 0);
	set_gpio_dir(gpionr, 0);

	if ((type & (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING))
	    == (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING))
		set_gpio_both(gpionr, 1);
	else
		set_gpio_both(gpionr, 0);

	if ((type & (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_LEVEL_LOW)))
		set_gpio_polar(gpionr, 1);	/* low or falling edge denoted by one */
	else
		set_gpio_polar(gpionr, 0);	/* high or rising edge denoted by zero */

	if (type & (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING)) {
		set_gpio_edge(gpionr, 1);
		set_gpio_inen(gpionr, 1);
		gpio_edge_triggered[gpio_bank(gpionr)] |= gpio_bit(gpionr);
		set_gpio_data(gpionr, 0);

	} else {
		set_gpio_edge(gpionr, 0);
		gpio_edge_triggered[gpio_bank(gpionr)] &= ~gpio_bit(gpionr);
		set_gpio_inen(gpionr, 1);
	}

	SSYNC();

	if (type & (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING))
		set_irq_handler(irq, handle_edge_irq);
	else
		set_irq_handler(irq, handle_level_irq);

	return 0;
}

#ifdef CONFIG_PM
int bfin_gpio_set_wake(unsigned int irq, unsigned int state)
{
	unsigned gpio = irq_to_gpio(irq);

	if (state)
		gpio_pm_wakeup_request(gpio, PM_WAKE_IGNORE);
	else
		gpio_pm_wakeup_free(gpio);

	return 0;
}
#endif

static struct irq_chip bfin_gpio_irqchip = {
	.ack = bfin_gpio_ack_irq,
	.mask = bfin_gpio_mask_irq,
	.mask_ack = bfin_gpio_mask_ack_irq,
	.unmask = bfin_gpio_unmask_irq,
	.set_type = bfin_gpio_irq_type,
	.startup = bfin_gpio_irq_startup,
	.shutdown = bfin_gpio_irq_shutdown,
#ifdef CONFIG_PM
	.set_wake = bfin_gpio_set_wake,
#endif
};

static void bfin_demux_gpio_irq(unsigned int inta_irq,
				struct irq_desc *desc)
{
	unsigned int i, gpio, mask, irq, search = 0;

	switch (inta_irq) {
#if defined(CONFIG_BF53x)
	case IRQ_PROG_INTA:
		irq = IRQ_PF0;
		search = 1;
		break;
# if defined(BF537_FAMILY) && !(defined(CONFIG_BFIN_MAC) || defined(CONFIG_BFIN_MAC_MODULE))
	case IRQ_MAC_RX:
		irq = IRQ_PH0;
		break;
# endif
#elif defined(CONFIG_BF52x)
	case IRQ_PORTF_INTA:
		irq = IRQ_PF0;
		break;
	case IRQ_PORTG_INTA:
		irq = IRQ_PG0;
		break;
	case IRQ_PORTH_INTA:
		irq = IRQ_PH0;
		break;
#elif defined(CONFIG_BF561)
	case IRQ_PROG0_INTA:
		irq = IRQ_PF0;
		break;
	case IRQ_PROG1_INTA:
		irq = IRQ_PF16;
		break;
	case IRQ_PROG2_INTA:
		irq = IRQ_PF32;
		break;
#endif
	default:
		BUG();
		return;
	}

	if (search) {
		for (i = 0; i < MAX_BLACKFIN_GPIOS; i += GPIO_BANKSIZE) {
			irq += i;

			mask = get_gpiop_data(i) &
				(gpio_enabled[gpio_bank(i)] &
				get_gpiop_maska(i));

			while (mask) {
				if (mask & 1) {
					desc = irq_desc + irq;
					desc->handle_irq(irq, desc);
				}
				irq++;
				mask >>= 1;
			}
		}
	} else {
			gpio = irq_to_gpio(irq);
			mask = get_gpiop_data(gpio) &
				(gpio_enabled[gpio_bank(gpio)] &
				get_gpiop_maska(gpio));

			do {
				if (mask & 1) {
					desc = irq_desc + irq;
					desc->handle_irq(irq, desc);
				}
				irq++;
				mask >>= 1;
			} while (mask);
	}

}

#else				/* CONFIG_BF54x */

#define NR_PINT_SYS_IRQS	4
#define NR_PINT_BITS		32
#define NR_PINTS		160
#define IRQ_NOT_AVAIL		0xFF

#define PINT_2_BANK(x)		((x) >> 5)
#define PINT_2_BIT(x)		((x) & 0x1F)
#define PINT_BIT(x)		(1 << (PINT_2_BIT(x)))

static unsigned char irq2pint_lut[NR_PINTS];
static unsigned char pint2irq_lut[NR_PINT_SYS_IRQS * NR_PINT_BITS];

static unsigned int gpio_both_edge_triggered[NR_PINT_SYS_IRQS];
static unsigned short gpio_enabled[gpio_bank(MAX_BLACKFIN_GPIOS)];


struct pin_int_t {
	unsigned int mask_set;
	unsigned int mask_clear;
	unsigned int request;
	unsigned int assign;
	unsigned int edge_set;
	unsigned int edge_clear;
	unsigned int invert_set;
	unsigned int invert_clear;
	unsigned int pinstate;
	unsigned int latch;
};

static struct pin_int_t *pint[NR_PINT_SYS_IRQS] = {
	(struct pin_int_t *)PINT0_MASK_SET,
	(struct pin_int_t *)PINT1_MASK_SET,
	(struct pin_int_t *)PINT2_MASK_SET,
	(struct pin_int_t *)PINT3_MASK_SET,
};

unsigned short get_irq_base(u8 bank, u8 bmap)
{

	u16 irq_base;

	if (bank < 2) {		/*PA-PB */
		irq_base = IRQ_PA0 + bmap * 16;
	} else {		/*PC-PJ */
		irq_base = IRQ_PC0 + bmap * 16;
	}

	return irq_base;

}

	/* Whenever PINTx_ASSIGN is altered init_pint_lut() must be executed! */
void init_pint_lut(void)
{
	u16 bank, bit, irq_base, bit_pos;
	u32 pint_assign;
	u8 bmap;

	memset(irq2pint_lut, IRQ_NOT_AVAIL, sizeof(irq2pint_lut));

	for (bank = 0; bank < NR_PINT_SYS_IRQS; bank++) {

		pint_assign = pint[bank]->assign;

		for (bit = 0; bit < NR_PINT_BITS; bit++) {

			bmap = (pint_assign >> ((bit / 8) * 8)) & 0xFF;

			irq_base = get_irq_base(bank, bmap);

			irq_base += (bit % 8) + ((bit / 8) & 1 ? 8 : 0);
			bit_pos = bit + bank * NR_PINT_BITS;

			pint2irq_lut[bit_pos] = irq_base - SYS_IRQS;
			irq2pint_lut[irq_base - SYS_IRQS] = bit_pos;

		}

	}

}

static void bfin_gpio_ack_irq(unsigned int irq)
{
	u8 pint_val = irq2pint_lut[irq - SYS_IRQS];
	u32 pintbit = PINT_BIT(pint_val);
	u8 bank = PINT_2_BANK(pint_val);

	if (unlikely(gpio_both_edge_triggered[bank] & pintbit)) {
		if (pint[bank]->invert_set & pintbit)
			pint[bank]->invert_clear = pintbit;
		else
			pint[bank]->invert_set = pintbit;
	}
	pint[bank]->request = pintbit;

	SSYNC();
}

static void bfin_gpio_mask_ack_irq(unsigned int irq)
{
	u8 pint_val = irq2pint_lut[irq - SYS_IRQS];
	u32 pintbit = PINT_BIT(pint_val);
	u8 bank = PINT_2_BANK(pint_val);

	if (unlikely(gpio_both_edge_triggered[bank] & pintbit)) {
		if (pint[bank]->invert_set & pintbit)
			pint[bank]->invert_clear = pintbit;
		else
			pint[bank]->invert_set = pintbit;
	}

	pint[bank]->request = pintbit;
	pint[bank]->mask_clear = pintbit;
	SSYNC();
}

static void bfin_gpio_mask_irq(unsigned int irq)
{
	u8 pint_val = irq2pint_lut[irq - SYS_IRQS];

	pint[PINT_2_BANK(pint_val)]->mask_clear = PINT_BIT(pint_val);
	SSYNC();
}

static void bfin_gpio_unmask_irq(unsigned int irq)
{
	u8 pint_val = irq2pint_lut[irq - SYS_IRQS];
	u32 pintbit = PINT_BIT(pint_val);
	u8 bank = PINT_2_BANK(pint_val);

	pint[bank]->request = pintbit;
	pint[bank]->mask_set = pintbit;
	SSYNC();
}

static unsigned int bfin_gpio_irq_startup(unsigned int irq)
{
	unsigned int ret;
	char buf[8];
	u16 gpionr = irq_to_gpio(irq);
	u8 pint_val = irq2pint_lut[irq - SYS_IRQS];

	if (pint_val == IRQ_NOT_AVAIL) {
		printk(KERN_ERR
		"GPIO IRQ %d :Not in PINT Assign table "
		"Reconfigure Interrupt to Port Assignemt\n", irq);
		return -ENODEV;
	}

	if (!(gpio_enabled[gpio_bank(gpionr)] & gpio_bit(gpionr))) {
		snprintf(buf, sizeof buf, "IRQ %d", irq);
		ret = gpio_request(gpionr, buf);
		if (ret)
			return ret;
	}

	gpio_enabled[gpio_bank(gpionr)] |= gpio_bit(gpionr);
	bfin_gpio_unmask_irq(irq);

	return ret;
}

static void bfin_gpio_irq_shutdown(unsigned int irq)
{
	u16 gpionr = irq_to_gpio(irq);

	bfin_gpio_mask_irq(irq);
	gpio_free(gpionr);
	gpio_enabled[gpio_bank(gpionr)] &= ~gpio_bit(gpionr);
}

static int bfin_gpio_irq_type(unsigned int irq, unsigned int type)
{

	unsigned int ret;
	char buf[8];
	u16 gpionr = irq_to_gpio(irq);
	u8 pint_val = irq2pint_lut[irq - SYS_IRQS];
	u32 pintbit = PINT_BIT(pint_val);
	u8 bank = PINT_2_BANK(pint_val);

	if (pint_val == IRQ_NOT_AVAIL)
		return -ENODEV;

	if (type == IRQ_TYPE_PROBE) {
		/* only probe unenabled GPIO interrupt lines */
		if (gpio_enabled[gpio_bank(gpionr)] & gpio_bit(gpionr))
			return 0;
		type = IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING;
	}

	if (type & (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING |
		    IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW)) {
		if (!(gpio_enabled[gpio_bank(gpionr)] & gpio_bit(gpionr))) {
			snprintf(buf, sizeof buf, "IRQ %d", irq);
			ret = gpio_request(gpionr, buf);
			if (ret)
				return ret;
		}

		gpio_enabled[gpio_bank(gpionr)] |= gpio_bit(gpionr);
	} else {
		gpio_enabled[gpio_bank(gpionr)] &= ~gpio_bit(gpionr);
		return 0;
	}

	gpio_direction_input(gpionr);

	if ((type & (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_LEVEL_LOW)))
		pint[bank]->invert_set = pintbit;	/* low or falling edge denoted by one */
	else
		pint[bank]->invert_clear = pintbit;	/* high or rising edge denoted by zero */

	if ((type & (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING))
	    == (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING)) {

		gpio_both_edge_triggered[bank] |= pintbit;

		if (gpio_get_value(gpionr))
			pint[bank]->invert_set = pintbit;
		else
			pint[bank]->invert_clear = pintbit;
	} else {
		gpio_both_edge_triggered[bank] &= ~pintbit;
	}

	if (type & (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING)) {
		pint[bank]->edge_set = pintbit;
		set_irq_handler(irq, handle_edge_irq);
	} else {
		pint[bank]->edge_clear = pintbit;
		set_irq_handler(irq, handle_level_irq);
	}

	SSYNC();

	return 0;
}

#ifdef CONFIG_PM
u32 pint_saved_masks[NR_PINT_SYS_IRQS];
u32 pint_wakeup_masks[NR_PINT_SYS_IRQS];

int bfin_gpio_set_wake(unsigned int irq, unsigned int state)
{
	u32 pint_irq;
	u8 pint_val = irq2pint_lut[irq - SYS_IRQS];
	u32 bank = PINT_2_BANK(pint_val);
	u32 pintbit = PINT_BIT(pint_val);

	switch (bank) {
	case 0:
		pint_irq = IRQ_PINT0;
		break;
	case 2:
		pint_irq = IRQ_PINT2;
		break;
	case 3:
		pint_irq = IRQ_PINT3;
		break;
	case 1:
		pint_irq = IRQ_PINT1;
		break;
	default:
		return -EINVAL;
	}

	bfin_internal_set_wake(pint_irq, state);

	if (state)
		pint_wakeup_masks[bank] |= pintbit;
	else
		pint_wakeup_masks[bank] &= ~pintbit;

	return 0;
}

u32 bfin_pm_setup(void)
{
	u32 val, i;

	for (i = 0; i < NR_PINT_SYS_IRQS; i++) {
		val = pint[i]->mask_clear;
		pint_saved_masks[i] = val;
		if (val ^ pint_wakeup_masks[i]) {
			pint[i]->mask_clear = val;
			pint[i]->mask_set = pint_wakeup_masks[i];
		}
	}

	return 0;
}

void bfin_pm_restore(void)
{
	u32 i, val;

	for (i = 0; i < NR_PINT_SYS_IRQS; i++) {
		val = pint_saved_masks[i];
		if (val ^ pint_wakeup_masks[i]) {
			pint[i]->mask_clear = pint[i]->mask_clear;
			pint[i]->mask_set = val;
		}
	}
}
#endif

static struct irq_chip bfin_gpio_irqchip = {
	.ack = bfin_gpio_ack_irq,
	.mask = bfin_gpio_mask_irq,
	.mask_ack = bfin_gpio_mask_ack_irq,
	.unmask = bfin_gpio_unmask_irq,
	.set_type = bfin_gpio_irq_type,
	.startup = bfin_gpio_irq_startup,
	.shutdown = bfin_gpio_irq_shutdown,
#ifdef CONFIG_PM
	.set_wake = bfin_gpio_set_wake,
#endif
};

static void bfin_demux_gpio_irq(unsigned int inta_irq,
				struct irq_desc *desc)
{
	u8 bank, pint_val;
	u32 request, irq;

	switch (inta_irq) {
	case IRQ_PINT0:
		bank = 0;
		break;
	case IRQ_PINT2:
		bank = 2;
		break;
	case IRQ_PINT3:
		bank = 3;
		break;
	case IRQ_PINT1:
		bank = 1;
		break;
	default:
		return;
	}

	pint_val = bank * NR_PINT_BITS;

	request = pint[bank]->request;

	while (request) {
		if (request & 1) {
			irq = pint2irq_lut[pint_val] + SYS_IRQS;
			desc = irq_desc + irq;
			desc->handle_irq(irq, desc);
		}
		pint_val++;
		request >>= 1;
	}

}
#endif

void __init init_exception_vectors(void)
{
	SSYNC();

	/* cannot program in software:
	 * evt0 - emulation (jtag)
	 * evt1 - reset
	 */
	bfin_write_EVT2(evt_nmi);
	bfin_write_EVT3(trap);
	bfin_write_EVT5(evt_ivhw);
	bfin_write_EVT6(evt_timer);
	bfin_write_EVT7(evt_evt7);
	bfin_write_EVT8(evt_evt8);
	bfin_write_EVT9(evt_evt9);
	bfin_write_EVT10(evt_evt10);
	bfin_write_EVT11(evt_evt11);
	bfin_write_EVT12(evt_evt12);
	bfin_write_EVT13(evt_evt13);
	bfin_write_EVT14(evt14_softirq);
	bfin_write_EVT15(evt_system_call);
	CSYNC();
}

/*
 * This function should be called during kernel startup to initialize
 * the BFin IRQ handling routines.
 */
int __init init_arch_irq(void)
{
	int irq;
	unsigned long ilat = 0;
	/*  Disable all the peripheral intrs  - page 4-29 HW Ref manual */
#if defined(CONFIG_BF54x) || defined(CONFIG_BF52x) || defined(CONFIG_BF561)
	bfin_write_SIC_IMASK0(SIC_UNMASK_ALL);
	bfin_write_SIC_IMASK1(SIC_UNMASK_ALL);
	bfin_write_SIC_IWR0(IWR_ENABLE_ALL);
	bfin_write_SIC_IWR1(IWR_ENABLE_ALL);
# ifdef CONFIG_BF54x
	bfin_write_SIC_IMASK2(SIC_UNMASK_ALL);
	bfin_write_SIC_IWR2(IWR_ENABLE_ALL);
# endif
#else
	bfin_write_SIC_IMASK(SIC_UNMASK_ALL);
	bfin_write_SIC_IWR(IWR_ENABLE_ALL);
#endif
	SSYNC();

	local_irq_disable();

	init_exception_buff();

#ifdef CONFIG_BF54x
# ifdef CONFIG_PINTx_REASSIGN
	pint[0]->assign = CONFIG_PINT0_ASSIGN;
	pint[1]->assign = CONFIG_PINT1_ASSIGN;
	pint[2]->assign = CONFIG_PINT2_ASSIGN;
	pint[3]->assign = CONFIG_PINT3_ASSIGN;
# endif
	/* Whenever PINTx_ASSIGN is altered init_pint_lut() must be executed! */
	init_pint_lut();
#endif

	for (irq = 0; irq <= SYS_IRQS; irq++) {
		if (irq <= IRQ_CORETMR)
			set_irq_chip(irq, &bfin_core_irqchip);
		else
			set_irq_chip(irq, &bfin_internal_irqchip);
#ifdef BF537_GENERIC_ERROR_INT_DEMUX
		if (irq != IRQ_GENERIC_ERROR) {
#endif

			switch (irq) {
#if defined(CONFIG_BF53x)
			case IRQ_PROG_INTA:
				set_irq_chained_handler(irq,
							bfin_demux_gpio_irq);
				break;
# if defined(BF537_FAMILY) && !(defined(CONFIG_BFIN_MAC) || defined(CONFIG_BFIN_MAC_MODULE))
			case IRQ_MAC_RX:
				set_irq_chained_handler(irq,
							bfin_demux_gpio_irq);
				break;
# endif
#elif defined(CONFIG_BF54x)
			case IRQ_PINT0:
				set_irq_chained_handler(irq,
							bfin_demux_gpio_irq);
				break;
			case IRQ_PINT1:
				set_irq_chained_handler(irq,
							bfin_demux_gpio_irq);
				break;
			case IRQ_PINT2:
				set_irq_chained_handler(irq,
							bfin_demux_gpio_irq);
				break;
			case IRQ_PINT3:
				set_irq_chained_handler(irq,
							bfin_demux_gpio_irq);
				break;
#elif defined(CONFIG_BF52x)
			case IRQ_PORTF_INTA:
				set_irq_chained_handler(irq,
							bfin_demux_gpio_irq);
				break;
			case IRQ_PORTG_INTA:
				set_irq_chained_handler(irq,
							bfin_demux_gpio_irq);
				break;
			case IRQ_PORTH_INTA:
				set_irq_chained_handler(irq,
							bfin_demux_gpio_irq);
				break;
#elif defined(CONFIG_BF561)
			case IRQ_PROG0_INTA:
				set_irq_chained_handler(irq,
							bfin_demux_gpio_irq);
				break;
			case IRQ_PROG1_INTA:
				set_irq_chained_handler(irq,
							bfin_demux_gpio_irq);
				break;
			case IRQ_PROG2_INTA:
				set_irq_chained_handler(irq,
							bfin_demux_gpio_irq);
				break;
#endif
			default:
				set_irq_handler(irq, handle_simple_irq);
				break;
			}

#ifdef BF537_GENERIC_ERROR_INT_DEMUX
		} else {
			set_irq_handler(irq, bfin_demux_error_irq);
		}
#endif
	}
#ifdef BF537_GENERIC_ERROR_INT_DEMUX
	for (irq = IRQ_PPI_ERROR; irq <= IRQ_UART1_ERROR; irq++) {
		set_irq_chip(irq, &bfin_generic_error_irqchip);
		set_irq_handler(irq, handle_level_irq);
	}
#endif

	for (irq = GPIO_IRQ_BASE; irq < NR_IRQS; irq++) {

		set_irq_chip(irq, &bfin_gpio_irqchip);
		/* if configured as edge, then will be changed to do_edge_IRQ */
		set_irq_handler(irq, handle_level_irq);
	}

	bfin_write_IMASK(0);
	CSYNC();
	ilat = bfin_read_ILAT();
	CSYNC();
	bfin_write_ILAT(ilat);
	CSYNC();

	printk(KERN_INFO "Configuring Blackfin Priority Driven Interrupts\n");
	/* IMASK=xxx is equivalent to STI xx or irq_flags=xx,
	 * local_irq_enable()
	 */
	program_IAR();
	/* Therefore it's better to setup IARs before interrupts enabled */
	search_IAR();

	/* Enable interrupts IVG7-15 */
	irq_flags = irq_flags | IMASK_IVG15 |
	    IMASK_IVG14 | IMASK_IVG13 | IMASK_IVG12 | IMASK_IVG11 |
	    IMASK_IVG10 | IMASK_IVG9 | IMASK_IVG8 | IMASK_IVG7 | IMASK_IVGHW;

	return 0;
}

#ifdef CONFIG_DO_IRQ_L1
__attribute__((l1_text))
#endif
void do_irq(int vec, struct pt_regs *fp)
{
	if (vec == EVT_IVTMR_P) {
		vec = IRQ_CORETMR;
	} else {
		struct ivgx *ivg = ivg7_13[vec - IVG7].ifirst;
		struct ivgx *ivg_stop = ivg7_13[vec - IVG7].istop;
#if defined(CONFIG_BF54x) || defined(CONFIG_BF52x) || defined(CONFIG_BF561)
		unsigned long sic_status[3];

		SSYNC();
		sic_status[0] = bfin_read_SIC_ISR0() & bfin_read_SIC_IMASK0();
		sic_status[1] = bfin_read_SIC_ISR1() & bfin_read_SIC_IMASK1();
#ifdef CONFIG_BF54x
		sic_status[2] = bfin_read_SIC_ISR2() & bfin_read_SIC_IMASK2();
#endif
		for (;; ivg++) {
			if (ivg >= ivg_stop) {
				atomic_inc(&num_spurious);
				return;
			}
			if (sic_status[(ivg->irqno - IVG7) / 32] & ivg->isrflag)
				break;
		}
#else
		unsigned long sic_status;
		SSYNC();
		sic_status = bfin_read_SIC_IMASK() & bfin_read_SIC_ISR();

		for (;; ivg++) {
			if (ivg >= ivg_stop) {
				atomic_inc(&num_spurious);
				return;
			} else if (sic_status & ivg->isrflag)
				break;
		}
#endif
		vec = ivg->irqno;
	}
	asm_do_IRQ(vec, fp);

#ifdef CONFIG_KGDB
	kgdb_process_breakpoint();
#endif
}
