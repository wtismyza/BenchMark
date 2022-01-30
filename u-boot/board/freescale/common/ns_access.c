// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2014 Freescale Semiconductor
 */

#include <common.h>
#include <log.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <fsl_csu.h>
#include <asm/arch/ns_access.h>
#include <asm/arch/fsl_serdes.h>

#ifdef CONFIG_ARCH_LS1021A
static struct csu_ns_dev ns_dev[] = {
	{ CSU_CSLX_PCIE2_IO, CSU_ALL_RW },
	{ CSU_CSLX_PCIE1_IO, CSU_ALL_RW },
	{ CSU_CSLX_MG2TPR_IP, CSU_ALL_RW },
	{ CSU_CSLX_IFC_MEM, CSU_ALL_RW },
	{ CSU_CSLX_OCRAM, CSU_ALL_RW },
	{ CSU_CSLX_GIC, CSU_ALL_RW },
	{ CSU_CSLX_PCIE1, CSU_ALL_RW },
	{ CSU_CSLX_OCRAM2, CSU_ALL_RW },
	{ CSU_CSLX_QSPI_MEM, CSU_ALL_RW },
	{ CSU_CSLX_PCIE2, CSU_ALL_RW },
	{ CSU_CSLX_SATA, CSU_ALL_RW },
	{ CSU_CSLX_USB3, CSU_ALL_RW },
	{ CSU_CSLX_SERDES, CSU_ALL_RW },
	{ CSU_CSLX_QDMA, CSU_ALL_RW },
	{ CSU_CSLX_LPUART2, CSU_ALL_RW },
	{ CSU_CSLX_LPUART1, CSU_ALL_RW },
	{ CSU_CSLX_LPUART4, CSU_ALL_RW },
	{ CSU_CSLX_LPUART3, CSU_ALL_RW },
	{ CSU_CSLX_LPUART6, CSU_ALL_RW },
	{ CSU_CSLX_LPUART5, CSU_ALL_RW },
	{ CSU_CSLX_DSPI2, CSU_ALL_RW },
	{ CSU_CSLX_DSPI1, CSU_ALL_RW },
	{ CSU_CSLX_QSPI, CSU_ALL_RW },
	{ CSU_CSLX_ESDHC, CSU_ALL_RW },
	{ CSU_CSLX_2D_ACE, CSU_ALL_RW },
	{ CSU_CSLX_IFC, CSU_ALL_RW },
	{ CSU_CSLX_I2C1, CSU_ALL_RW },
	{ CSU_CSLX_USB2, CSU_ALL_RW },
	{ CSU_CSLX_I2C3, CSU_ALL_RW },
	{ CSU_CSLX_I2C2, CSU_ALL_RW },
	{ CSU_CSLX_DUART2, CSU_ALL_RW },
	{ CSU_CSLX_DUART1, CSU_ALL_RW },
	{ CSU_CSLX_WDT2, CSU_ALL_RW },
	{ CSU_CSLX_WDT1, CSU_ALL_RW },
	{ CSU_CSLX_EDMA, CSU_ALL_RW },
	{ CSU_CSLX_SYS_CNT, CSU_ALL_RW },
	{ CSU_CSLX_DMA_MUX2, CSU_ALL_RW },
	{ CSU_CSLX_DMA_MUX1, CSU_ALL_RW },
	{ CSU_CSLX_DDR, CSU_ALL_RW },
	{ CSU_CSLX_QUICC, CSU_ALL_RW },
	{ CSU_CSLX_DCFG_CCU_RCPM, CSU_ALL_RW },
	{ CSU_CSLX_SECURE_BOOTROM, CSU_ALL_RW },
	{ CSU_CSLX_SFP, CSU_ALL_RW },
	{ CSU_CSLX_TMU, CSU_ALL_RW },
	{ CSU_CSLX_SECURE_MONITOR, CSU_ALL_RW },
	{ CSU_CSLX_RESERVED0, CSU_ALL_RW },
	{ CSU_CSLX_ETSEC1, CSU_ALL_RW },
	{ CSU_CSLX_SEC5_5, CSU_ALL_RW },
	{ CSU_CSLX_ETSEC3, CSU_ALL_RW },
	{ CSU_CSLX_ETSEC2, CSU_ALL_RW },
	{ CSU_CSLX_GPIO2, CSU_ALL_RW },
	{ CSU_CSLX_GPIO1, CSU_ALL_RW },
	{ CSU_CSLX_GPIO4, CSU_ALL_RW },
	{ CSU_CSLX_GPIO3, CSU_ALL_RW },
	{ CSU_CSLX_PLATFORM_CONT, CSU_ALL_RW },
	{ CSU_CSLX_CSU, CSU_ALL_RW },
	{ CSU_CSLX_ASRC, CSU_ALL_RW },
	{ CSU_CSLX_SPDIF, CSU_ALL_RW },
	{ CSU_CSLX_FLEXCAN2, CSU_ALL_RW },
	{ CSU_CSLX_FLEXCAN1, CSU_ALL_RW },
	{ CSU_CSLX_FLEXCAN4, CSU_ALL_RW },
	{ CSU_CSLX_FLEXCAN3, CSU_ALL_RW },
	{ CSU_CSLX_SAI2, CSU_ALL_RW },
	{ CSU_CSLX_SAI1, CSU_ALL_RW },
	{ CSU_CSLX_SAI4, CSU_ALL_RW },
	{ CSU_CSLX_SAI3, CSU_ALL_RW },
	{ CSU_CSLX_FTM2, CSU_ALL_RW },
	{ CSU_CSLX_FTM1, CSU_ALL_RW },
	{ CSU_CSLX_FTM4, CSU_ALL_RW },
	{ CSU_CSLX_FTM3, CSU_ALL_RW },
	{ CSU_CSLX_FTM6, CSU_ALL_RW },
	{ CSU_CSLX_FTM5, CSU_ALL_RW },
	{ CSU_CSLX_FTM8, CSU_ALL_RW },
	{ CSU_CSLX_FTM7, CSU_ALL_RW },
	{ CSU_CSLX_COP_DCSR, CSU_ALL_RW },
	{ CSU_CSLX_EPU, CSU_ALL_RW },
	{ CSU_CSLX_GDI, CSU_ALL_RW },
	{ CSU_CSLX_DDI, CSU_ALL_RW },
	{ CSU_CSLX_RESERVED1, CSU_ALL_RW },
	{ CSU_CSLX_USB3_PHY, CSU_ALL_RW },
	{ CSU_CSLX_RESERVED2, CSU_ALL_RW },
};

#else
static struct csu_ns_dev ns_dev[] = {
	 {CSU_CSLX_PCIE2_IO, CSU_ALL_RW},
	 {CSU_CSLX_PCIE1_IO, CSU_ALL_RW},
	 {CSU_CSLX_MG2TPR_IP, CSU_ALL_RW},
	 {CSU_CSLX_IFC_MEM, CSU_ALL_RW},
	 {CSU_CSLX_OCRAM, CSU_ALL_RW},
	 {CSU_CSLX_GIC, CSU_ALL_RW},
	 {CSU_CSLX_PCIE1, CSU_ALL_RW},
	 {CSU_CSLX_OCRAM2, CSU_ALL_RW},
	 {CSU_CSLX_QSPI_MEM, CSU_ALL_RW},
	 {CSU_CSLX_PCIE2, CSU_ALL_RW},
	 {CSU_CSLX_SATA, CSU_ALL_RW},
	 {CSU_CSLX_USB1, CSU_ALL_RW},
	 {CSU_CSLX_QM_BM_SWPORTAL, CSU_ALL_RW},
	 {CSU_CSLX_PCIE3, CSU_ALL_RW},
	 {CSU_CSLX_PCIE3_IO, CSU_ALL_RW},
	 {CSU_CSLX_USB3, CSU_ALL_RW},
	 {CSU_CSLX_USB2, CSU_ALL_RW},
	 {CSU_CSLX_PFE, CSU_ALL_RW},
	 {CSU_CSLX_SERDES, CSU_ALL_RW},
	 {CSU_CSLX_QDMA, CSU_ALL_RW},
	 {CSU_CSLX_LPUART2, CSU_ALL_RW},
	 {CSU_CSLX_LPUART1, CSU_ALL_RW},
	 {CSU_CSLX_LPUART4, CSU_ALL_RW},
	 {CSU_CSLX_LPUART3, CSU_ALL_RW},
	 {CSU_CSLX_LPUART6, CSU_ALL_RW},
	 {CSU_CSLX_LPUART5, CSU_ALL_RW},
	 {CSU_CSLX_DSPI1, CSU_ALL_RW},
	 {CSU_CSLX_QSPI, CSU_ALL_RW},
	 {CSU_CSLX_ESDHC, CSU_ALL_RW},
	 {CSU_CSLX_IFC, CSU_ALL_RW},
	 {CSU_CSLX_I2C1, CSU_ALL_RW},
	 {CSU_CSLX_I2C3, CSU_ALL_RW},
	 {CSU_CSLX_I2C2, CSU_ALL_RW},
	 {CSU_CSLX_DUART2, CSU_ALL_RW},
	 {CSU_CSLX_DUART1, CSU_ALL_RW},
	 {CSU_CSLX_WDT2, CSU_ALL_RW},
	 {CSU_CSLX_WDT1, CSU_ALL_RW},
	 {CSU_CSLX_EDMA, CSU_ALL_RW},
	 {CSU_CSLX_SYS_CNT, CSU_ALL_RW},
	 {CSU_CSLX_DMA_MUX2, CSU_ALL_RW},
	 {CSU_CSLX_DMA_MUX1, CSU_ALL_RW},
	 {CSU_CSLX_DDR, CSU_ALL_RW},
	 {CSU_CSLX_QUICC, CSU_ALL_RW},
	 {CSU_CSLX_DCFG_CCU_RCPM, CSU_ALL_RW},
	 {CSU_CSLX_SECURE_BOOTROM, CSU_ALL_RW},
	 {CSU_CSLX_SFP, CSU_ALL_RW},
	 {CSU_CSLX_TMU, CSU_ALL_RW},
	 {CSU_CSLX_SECURE_MONITOR, CSU_ALL_RW},
	 {CSU_CSLX_SCFG, CSU_ALL_RW},
	 {CSU_CSLX_FM, CSU_ALL_RW},
	 {CSU_CSLX_SEC5_5, CSU_ALL_RW},
	 {CSU_CSLX_BM, CSU_ALL_RW},
	 {CSU_CSLX_QM, CSU_ALL_RW},
	 {CSU_CSLX_GPIO2, CSU_ALL_RW},
	 {CSU_CSLX_GPIO1, CSU_ALL_RW},
	 {CSU_CSLX_GPIO4, CSU_ALL_RW},
	 {CSU_CSLX_GPIO3, CSU_ALL_RW},
	 {CSU_CSLX_PLATFORM_CONT, CSU_ALL_RW},
	 {CSU_CSLX_CSU, CSU_ALL_RW},
	 {CSU_CSLX_IIC4, CSU_ALL_RW},
	 {CSU_CSLX_WDT4, CSU_ALL_RW},
	 {CSU_CSLX_WDT3, CSU_ALL_RW},
	 {CSU_CSLX_ESDHC2, CSU_ALL_RW},
	 {CSU_CSLX_WDT5, CSU_ALL_RW},
	 {CSU_CSLX_SAI2, CSU_ALL_RW},
	 {CSU_CSLX_SAI1, CSU_ALL_RW},
	 {CSU_CSLX_SAI4, CSU_ALL_RW},
	 {CSU_CSLX_SAI3, CSU_ALL_RW},
	 {CSU_CSLX_FTM2, CSU_ALL_RW},
	 {CSU_CSLX_FTM1, CSU_ALL_RW},
	 {CSU_CSLX_FTM4, CSU_ALL_RW},
	 {CSU_CSLX_FTM3, CSU_ALL_RW},
	 {CSU_CSLX_FTM6, CSU_ALL_RW},
	 {CSU_CSLX_FTM5, CSU_ALL_RW},
	 {CSU_CSLX_FTM8, CSU_ALL_RW},
	 {CSU_CSLX_FTM7, CSU_ALL_RW},
	 {CSU_CSLX_DSCR, CSU_ALL_RW},
};
#endif

void set_devices_ns_access(unsigned long index, u16 val)
{
	u32 *base = (u32 *)CONFIG_SYS_FSL_CSU_ADDR;
	u32 *reg;
	uint32_t tmp;

	reg = base + index / 2;
	tmp = in_be32(reg);
	if (index % 2 == 0) {
		tmp &= 0x0000ffff;
		tmp |= val << 16;
	} else {
		tmp &= 0xffff0000;
		tmp |= val;
	}

	out_be32(reg, tmp);
}

static void enable_devices_ns_access(struct csu_ns_dev *ns_dev, uint32_t num)
{
	int i;

	for (i = 0; i < num; i++)
		set_devices_ns_access(ns_dev[i].ind, ns_dev[i].val);
}

void enable_layerscape_ns_access(void)
{
#ifdef CONFIG_ARM64
	if (current_el() == 3)
#endif
		enable_devices_ns_access(ns_dev, ARRAY_SIZE(ns_dev));
}

void set_pcie_ns_access(int pcie, u16 val)
{
	switch (pcie) {
#ifdef CONFIG_PCIE1
	case PCIE1:
		set_devices_ns_access(CSU_CSLX_PCIE1, val);
		set_devices_ns_access(CSU_CSLX_PCIE1_IO, val);
		return;
#endif
#ifdef CONFIG_PCIE2
	case PCIE2:
		set_devices_ns_access(CSU_CSLX_PCIE2, val);
		set_devices_ns_access(CSU_CSLX_PCIE2_IO, val);
		return;
#endif
#ifdef CONFIG_PCIE3
	case PCIE3:
		set_devices_ns_access(CSU_CSLX_PCIE3, val);
		set_devices_ns_access(CSU_CSLX_PCIE3_IO, val);
		return;
#endif
	default:
		debug("The PCIE%d doesn't exist!\n", pcie);
		return;
	}
}
