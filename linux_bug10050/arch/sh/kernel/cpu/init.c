/*
 * arch/sh/kernel/cpu/init.c
 *
 * CPU init code
 *
 * Copyright (C) 2002 - 2007  Paul Mundt
 * Copyright (C) 2003  Richard Curnow
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/log2.h>
#include <asm/mmu_context.h>
#include <asm/processor.h>
#include <asm/uaccess.h>
#include <asm/page.h>
#include <asm/system.h>
#include <asm/cacheflush.h>
#include <asm/cache.h>
#include <asm/elf.h>
#include <asm/io.h>
#include <asm/smp.h>
#ifdef CONFIG_SUPERH32
#include <asm/ubc.h>
#endif

/*
 * Generic wrapper for command line arguments to disable on-chip
 * peripherals (nofpu, nodsp, and so forth).
 */
#define onchip_setup(x)				\
static int x##_disabled __initdata = 0;		\
						\
static int __init x##_setup(char *opts)		\
{						\
	x##_disabled = 1;			\
	return 1;				\
}						\
__setup("no" __stringify(x), x##_setup);

onchip_setup(fpu);
onchip_setup(dsp);

#ifdef CONFIG_SPECULATIVE_EXECUTION
#define CPUOPM		0xff2f0000
#define CPUOPM_RABD	(1 << 5)

static void __init speculative_execution_init(void)
{
	/* Clear RABD */
	ctrl_outl(ctrl_inl(CPUOPM) & ~CPUOPM_RABD, CPUOPM);

	/* Flush the update */
	(void)ctrl_inl(CPUOPM);
	ctrl_barrier();
}
#else
#define speculative_execution_init()	do { } while (0)
#endif

/*
 * Generic first-level cache init
 */
#ifdef CONFIG_SUPERH32
static void __uses_jump_to_uncached cache_init(void)
{
	unsigned long ccr, flags;

	jump_to_uncached();
	ccr = ctrl_inl(CCR);

	/*
	 * At this point we don't know whether the cache is enabled or not - a
	 * bootloader may have enabled it.  There are at least 2 things that
	 * could be dirty in the cache at this point:
	 * 1. kernel command line set up by boot loader
	 * 2. spilled registers from the prolog of this function
	 * => before re-initialising the cache, we must do a purge of the whole
	 * cache out to memory for safety.  As long as nothing is spilled
	 * during the loop to lines that have already been done, this is safe.
	 * - RPC
	 */
	if (ccr & CCR_CACHE_ENABLE) {
		unsigned long ways, waysize, addrstart;

		waysize = current_cpu_data.dcache.sets;

#ifdef CCR_CACHE_ORA
		/*
		 * If the OC is already in RAM mode, we only have
		 * half of the entries to flush..
		 */
		if (ccr & CCR_CACHE_ORA)
			waysize >>= 1;
#endif

		waysize <<= current_cpu_data.dcache.entry_shift;

#ifdef CCR_CACHE_EMODE
		/* If EMODE is not set, we only have 1 way to flush. */
		if (!(ccr & CCR_CACHE_EMODE))
			ways = 1;
		else
#endif
			ways = current_cpu_data.dcache.ways;

		addrstart = CACHE_OC_ADDRESS_ARRAY;
		do {
			unsigned long addr;

			for (addr = addrstart;
			     addr < addrstart + waysize;
			     addr += current_cpu_data.dcache.linesz)
				ctrl_outl(0, addr);

			addrstart += current_cpu_data.dcache.way_incr;
		} while (--ways);
	}

	/*
	 * Default CCR values .. enable the caches
	 * and invalidate them immediately..
	 */
	flags = CCR_CACHE_ENABLE | CCR_CACHE_INVALIDATE;

#ifdef CCR_CACHE_EMODE
	/* Force EMODE if possible */
	if (current_cpu_data.dcache.ways > 1)
		flags |= CCR_CACHE_EMODE;
	else
		flags &= ~CCR_CACHE_EMODE;
#endif

#if defined(CONFIG_CACHE_WRITETHROUGH)
	/* Write-through */
	flags |= CCR_CACHE_WT;
#elif defined(CONFIG_CACHE_WRITEBACK)
	/* Write-back */
	flags |= CCR_CACHE_CB;
#else
	/* Off */
	flags &= ~CCR_CACHE_ENABLE;
#endif

	ctrl_outl(flags, CCR);
	back_to_cached();
}
#else
#define cache_init()	do { } while (0)
#endif

#define CSHAPE(totalsize, linesize, assoc) \
	((totalsize & ~0xff) | (linesize << 4) | assoc)

#define CACHE_DESC_SHAPE(desc)	\
	CSHAPE((desc).way_size * (desc).ways, ilog2((desc).linesz), (desc).ways)

static void detect_cache_shape(void)
{
	l1d_cache_shape = CACHE_DESC_SHAPE(current_cpu_data.dcache);

	if (current_cpu_data.dcache.flags & SH_CACHE_COMBINED)
		l1i_cache_shape = l1d_cache_shape;
	else
		l1i_cache_shape = CACHE_DESC_SHAPE(current_cpu_data.icache);

	if (current_cpu_data.flags & CPU_HAS_L2_CACHE)
		l2_cache_shape = CACHE_DESC_SHAPE(current_cpu_data.scache);
	else
		l2_cache_shape = -1; /* No S-cache */
}

#ifdef CONFIG_SH_DSP
static void __init release_dsp(void)
{
	unsigned long sr;

	/* Clear SR.DSP bit */
	__asm__ __volatile__ (
		"stc\tsr, %0\n\t"
		"and\t%1, %0\n\t"
		"ldc\t%0, sr\n\t"
		: "=&r" (sr)
		: "r" (~SR_DSP)
	);
}

static void __init dsp_init(void)
{
	unsigned long sr;

	/*
	 * Set the SR.DSP bit, wait for one instruction, and then read
	 * back the SR value.
	 */
	__asm__ __volatile__ (
		"stc\tsr, %0\n\t"
		"or\t%1, %0\n\t"
		"ldc\t%0, sr\n\t"
		"nop\n\t"
		"stc\tsr, %0\n\t"
		: "=&r" (sr)
		: "r" (SR_DSP)
	);

	/* If the DSP bit is still set, this CPU has a DSP */
	if (sr & SR_DSP)
		current_cpu_data.flags |= CPU_HAS_DSP;

	/* Now that we've determined the DSP status, clear the DSP bit. */
	release_dsp();
}
#endif /* CONFIG_SH_DSP */

/**
 * sh_cpu_init
 *
 * This is our initial entry point for each CPU, and is invoked on the boot
 * CPU prior to calling start_kernel(). For SMP, a combination of this and
 * start_secondary() will bring up each processor to a ready state prior
 * to hand forking the idle loop.
 *
 * We do all of the basic processor init here, including setting up the
 * caches, FPU, DSP, kicking the UBC, etc. By the time start_kernel() is
 * hit (and subsequently platform_setup()) things like determining the
 * CPU subtype and initial configuration will all be done.
 *
 * Each processor family is still responsible for doing its own probing
 * and cache configuration in detect_cpu_and_cache_system().
 */

asmlinkage void __cpuinit sh_cpu_init(void)
{
	current_thread_info()->cpu = hard_smp_processor_id();

	/* First, probe the CPU */
	detect_cpu_and_cache_system();

	if (current_cpu_data.type == CPU_SH_NONE)
		panic("Unknown CPU");

	/* First setup the rest of the I-cache info */
	current_cpu_data.icache.entry_mask = current_cpu_data.icache.way_incr -
				      current_cpu_data.icache.linesz;

	current_cpu_data.icache.way_size = current_cpu_data.icache.sets *
				    current_cpu_data.icache.linesz;

	/* And the D-cache too */
	current_cpu_data.dcache.entry_mask = current_cpu_data.dcache.way_incr -
				      current_cpu_data.dcache.linesz;

	current_cpu_data.dcache.way_size = current_cpu_data.dcache.sets *
				    current_cpu_data.dcache.linesz;

	/* Init the cache */
	cache_init();

	if (raw_smp_processor_id() == 0) {
		shm_align_mask = max_t(unsigned long,
				       current_cpu_data.dcache.way_size - 1,
				       PAGE_SIZE - 1);

		/* Boot CPU sets the cache shape */
		detect_cache_shape();
	}

	/* Disable the FPU */
	if (fpu_disabled) {
		printk("FPU Disabled\n");
		current_cpu_data.flags &= ~CPU_HAS_FPU;
		disable_fpu();
	}

	/* FPU initialization */
	if ((current_cpu_data.flags & CPU_HAS_FPU)) {
		clear_thread_flag(TIF_USEDFPU);
		clear_used_math();
	}

	/*
	 * Initialize the per-CPU ASID cache very early, since the
	 * TLB flushing routines depend on this being setup.
	 */
	current_cpu_data.asid_cache = NO_CONTEXT;

#ifdef CONFIG_SH_DSP
	/* Probe for DSP */
	dsp_init();

	/* Disable the DSP */
	if (dsp_disabled) {
		printk("DSP Disabled\n");
		current_cpu_data.flags &= ~CPU_HAS_DSP;
		release_dsp();
	}
#endif

	/*
	 * Some brain-damaged loaders decided it would be a good idea to put
	 * the UBC to sleep. This causes some issues when it comes to things
	 * like PTRACE_SINGLESTEP or doing hardware watchpoints in GDB.  So ..
	 * we wake it up and hope that all is well.
	 */
#ifdef CONFIG_SUPERH32
	if (raw_smp_processor_id() == 0)
		ubc_wakeup();
#endif

	speculative_execution_init();
}
