/*
 *  This file contains quirk handling code for PnP devices
 *  Some devices do not report all their resources, and need to have extra
 *  resources added. This is most easily accomplished at initialisation time
 *  when building up the resource structure for the first time.
 *
 *  Copyright (c) 2000 Peter Denison <peterd@pnd-pc.demon.co.uk>
 *
 *  Heavily based on PCI quirks handling which is
 *
 *  Copyright (c) 1999 Martin Mares <mj@ucw.cz>
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/pnp.h>
#include <linux/io.h>
#include <linux/dmi.h>
#include <linux/kallsyms.h>
#include "base.h"

static void quirk_awe32_resources(struct pnp_dev *dev)
{
	struct pnp_port *port, *port2, *port3;
	struct pnp_option *res = dev->dependent;

	/*
	 * Unfortunately the isapnp_add_port_resource is too tightly bound
	 * into the PnP discovery sequence, and cannot be used. Link in the
	 * two extra ports (at offset 0x400 and 0x800 from the one given) by
	 * hand.
	 */
	for (; res; res = res->next) {
		port2 = pnp_alloc(sizeof(struct pnp_port));
		if (!port2)
			return;
		port3 = pnp_alloc(sizeof(struct pnp_port));
		if (!port3) {
			kfree(port2);
			return;
		}
		port = res->port;
		memcpy(port2, port, sizeof(struct pnp_port));
		memcpy(port3, port, sizeof(struct pnp_port));
		port->next = port2;
		port2->next = port3;
		port2->min += 0x400;
		port2->max += 0x400;
		port3->min += 0x800;
		port3->max += 0x800;
	}
	printk(KERN_INFO "pnp: AWE32 quirk - adding two ports\n");
}

static void quirk_cmi8330_resources(struct pnp_dev *dev)
{
	struct pnp_option *res = dev->dependent;
	unsigned long tmp;

	for (; res; res = res->next) {

		struct pnp_irq *irq;
		struct pnp_dma *dma;

		for (irq = res->irq; irq; irq = irq->next) {	// Valid irqs are 5, 7, 10
			tmp = 0x04A0;
			bitmap_copy(irq->map, &tmp, 16);	// 0000 0100 1010 0000
		}

		for (dma = res->dma; dma; dma = dma->next)	// Valid 8bit dma channels are 1,3
			if ((dma->flags & IORESOURCE_DMA_TYPE_MASK) ==
			    IORESOURCE_DMA_8BIT)
				dma->map = 0x000A;
	}
	printk(KERN_INFO "pnp: CMI8330 quirk - fixing interrupts and dma\n");
}

static void quirk_sb16audio_resources(struct pnp_dev *dev)
{
	struct pnp_port *port;
	struct pnp_option *res = dev->dependent;
	int changed = 0;

	/*
	 * The default range on the mpu port for these devices is 0x388-0x388.
	 * Here we increase that range so that two such cards can be
	 * auto-configured.
	 */

	for (; res; res = res->next) {
		port = res->port;
		if (!port)
			continue;
		port = port->next;
		if (!port)
			continue;
		port = port->next;
		if (!port)
			continue;
		if (port->min != port->max)
			continue;
		port->max += 0x70;
		changed = 1;
	}
	if (changed)
		printk(KERN_INFO
		       "pnp: SB audio device quirk - increasing port range\n");
}

static void quirk_supermicro_h8dce_system(struct pnp_dev *dev)
{
	int i;
	static struct dmi_system_id supermicro_h8dce[] = {
		{
			.ident = "Supermicro H8DCE",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "Supermicro"),
				DMI_MATCH(DMI_PRODUCT_NAME, "H8DCE"),
			},
		},
		{ }
	};

	if (!dmi_check_system(supermicro_h8dce))
		return;

	/*
	 * On the Supermicro H8DCE, there's a system device with resources
	 * that overlap BAR 6 of the built-in SATA PCI adapter.  If the PNP
	 * system device claims them, the sata_nv driver won't be able to.
	 * More details at:
	 *     https://bugzilla.redhat.com/show_bug.cgi?id=280641
	 *     https://bugzilla.redhat.com/show_bug.cgi?id=313491
	 *     http://lkml.org/lkml/2008/1/9/449
	 *     http://thread.gmane.org/gmane.linux.acpi.devel/27312
	 */
	for (i = 0; i < PNP_MAX_MEM; i++) {
		if (pnp_mem_valid(dev, i) && pnp_mem_len(dev, i) &&
		    (pnp_mem_start(dev, i) & 0xdfef0000) == 0xdfef0000) {
			dev_warn(&dev->dev, "disabling 0x%llx-0x%llx to prevent"
				" conflict with sata_nv PCI device\n",
				(unsigned long long) pnp_mem_start(dev, i),
				(unsigned long long) (pnp_mem_start(dev, i) +
					pnp_mem_len(dev, i) - 1));
			pnp_mem_flags(dev, i) = 0;
		}
	}
}

/*
 *  PnP Quirks
 *  Cards or devices that need some tweaking due to incomplete resource info
 */

static struct pnp_fixup pnp_fixups[] = {
	/* Soundblaster awe io port quirk */
	{"CTL0021", quirk_awe32_resources},
	{"CTL0022", quirk_awe32_resources},
	{"CTL0023", quirk_awe32_resources},
	/* CMI 8330 interrupt and dma fix */
	{"@X@0001", quirk_cmi8330_resources},
	/* Soundblaster audio device io port range quirk */
	{"CTL0001", quirk_sb16audio_resources},
	{"CTL0031", quirk_sb16audio_resources},
	{"CTL0041", quirk_sb16audio_resources},
	{"CTL0042", quirk_sb16audio_resources},
	{"CTL0043", quirk_sb16audio_resources},
	{"CTL0044", quirk_sb16audio_resources},
	{"CTL0045", quirk_sb16audio_resources},
	{"PNP0c01", quirk_supermicro_h8dce_system},
	{"PNP0c02", quirk_supermicro_h8dce_system},
	{""}
};

void pnp_fixup_device(struct pnp_dev *dev)
{
	int i = 0;
	void (*quirk)(struct pnp_dev *);

	while (*pnp_fixups[i].id) {
		if (compare_pnp_id(dev->id, pnp_fixups[i].id)) {
			quirk = pnp_fixups[i].quirk_function;

#ifdef DEBUG
			dev_dbg(&dev->dev, "calling quirk 0x%p", quirk);
			print_fn_descriptor_symbol(": %s()\n",
				(unsigned long) *quirk);
#endif
			(*quirk)(dev);
		}
		i++;
	}
}
