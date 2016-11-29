/*
 * Copyright (c) 2016, Linaro Limited
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <keep.h>
#include <kernel/interrupt.h>
#include <kernel/misc.h>
#include <initcall.h>
#include <io.h>
#include <kernel/mutex.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <platform_config.h>
#include <rng_support.h>

#define	RNG_OUTPUT_L            0x0000
#define	RNG_OUTPUT_H            0x0004
#define	RNG_STATUS              0x0008
#  define RNG_READY             BIT(0)
#  define SHUTDOWN_OFLO         BIT(1)
#define	RNG_INTMASK             0x000C
#define	RNG_INTACK              0x0010
#define	RNG_CONTROL             0x0014
#  define ENABLE_TRNG           BIT(10)
#define	RNG_CONFIG              0x0018
#define	RNG_ALARMCNT            0x001C
#define	RNG_FROENABLE           0x0020
#define	RNG_FRODETUNE           0x0024
#define	RNG_ALARMMASK           0x0028
#define	RNG_ALARMSTOP           0x002C
#define	RNG_LFSR_L              0x0030
#define	RNG_LFSR_M              0x0034
#define	RNG_LFSR_H              0x0038
#define	RNG_COUNT               0x003C
#define	RNG_OPTIONS             0x0078
#define	RNG_EIP_REV             0x007C
#define	RNG_MMR_STATUS_EN       0x1FD8
#define	RNG_REV                 0x1FE0
#define	RNG_SYS_CONFIG_REG      0x1FE4
#  define RNG_AUTOIDLE          BIT(0)
#define	RNG_MMR_STATUS_SET      0x1FEC
#define	RNG_SOFT_RESET_REG      0x1FF0
#  define RNG_SOFT_RESET        BIT(0)
#define	RNG_IRQ_EOI_REG         0x1FF4
#define	RNG_IRQSTATUS           0x1FF8

#define RNG_CONTROL_STARTUP_CYCLES_SHIFT        16
#define RNG_CONTROL_STARTUP_CYCLES_MASK         (0xffff << 16)

#define RNG_CONFIG_MAX_REFIL_CYCLES_SHIFT       16
#define RNG_CONFIG_MAX_REFIL_CYCLES_MASK        (0xffff << 16)
#define RNG_CONFIG_MIN_REFIL_CYCLES_SHIFT       0
#define RNG_CONFIG_MIN_REFIL_CYCLES_MASK        (0xff << 0)

#define RNG_ALARMCNT_ALARM_TH_SHIFT             0x0
#define RNG_ALARMCNT_ALARM_TH_MASK              (0xff << 0)
#define RNG_ALARMCNT_SHUTDOWN_TH_SHIFT          16
#define RNG_ALARMCNT_SHUTDOWN_TH_MASK           (0x1f << 16)

#define RNG_CONTROL_STARTUP_CYCLES              0xff
#define RNG_CONFIG_MIN_REFIL_CYCLES             0x21
#define RNG_CONFIG_MAX_REFIL_CYCLES             0x22
#define RNG_ALARM_THRESHOLD                     0xff
#define RNG_SHUTDOWN_THRESHOLD                  0x4

#define RNG_FRO_MASK    0xffffff

#define RNG_REG_SIZE    0x2000

register_phys_mem(MEM_AREA_IO_SEC, RNG_BASE, RNG_REG_SIZE);

static struct mutex rng_mutex = MUTEX_INITIALIZER;

static vaddr_t rng_base(void)
{
	static void *va __early_bss;

	if (cpu_mmu_enabled()) {
		if (!va)
			va = phys_to_virt(RNG_BASE, MEM_AREA_IO_SEC);
		return (vaddr_t)va;
	}

	return RNG_BASE;
}

uint8_t hw_get_random_byte(void)
{
	static vaddr_t rng;
	static int pos;
	static union {
		uint64_t val;
		uint8_t byte[8];
	} random;
	uint8_t ret;

	mutex_lock(&rng_mutex);

	if (!rng)
		rng = rng_base();

	if (!pos) {
		/* Is the result ready (available)? */
		while (!(read32(rng + RNG_STATUS) & RNG_READY)) {
			/* Is the shutdown threshold reached? */
			if (read32(rng + RNG_STATUS) & SHUTDOWN_OFLO) {
				uint32_t alarm = read32(rng + RNG_ALARMSTOP);
				uint32_t tuning = read32(rng + RNG_FRODETUNE);
				/* Clear the alarm events */
				write32(0x0, rng + RNG_ALARMMASK);
				write32(0x0, rng + RNG_ALARMSTOP);
				/* De-tune offending FROs */
				write32(tuning ^ alarm, rng + RNG_FRODETUNE);
				/* Re-enable the shut down FROs */
				write32(RNG_FRO_MASK, rng + RNG_FROENABLE);
				/* Clear the shutdown overflow event */
				write32(SHUTDOWN_OFLO, rng + RNG_INTACK);

				DMSG("Fixed FRO shutdown\n");
			}
		}
		/* Read random value */
		random.val = read32(rng + RNG_OUTPUT_L);
		random.val |= SHIFT_U64(read32(rng + RNG_OUTPUT_H), 32);
		/* Acknowledge read complete */
		write32(RNG_READY, rng + RNG_INTACK);
	}

	ret = random.byte[pos++];

	if (pos == 8)
		pos = 0;

	mutex_unlock(&rng_mutex);

	return ret;
}

static TEE_Result dra7_rng_init(void)
{
	vaddr_t rng = rng_base();
	uint32_t val;

	/* Execute a software reset */
	write32(RNG_SOFT_RESET, rng + RNG_SOFT_RESET_REG);

	/* Wait for the software reset completion by polling */
	while (read32(rng + RNG_SOFT_RESET_REG) & RNG_SOFT_RESET)
		;

	/* Switch to low-power operating mode */
	write32(RNG_AUTOIDLE, rng + RNG_SYS_CONFIG_REG);

	/*
	 * Select the number of clock input cycles to the
	 * FROs between two samples
	 */
	val = 0;

	/* Ensure initial latency */
	val |= RNG_CONFIG_MIN_REFIL_CYCLES <<
			RNG_CONFIG_MIN_REFIL_CYCLES_SHIFT;
	val |= RNG_CONFIG_MAX_REFIL_CYCLES <<
			RNG_CONFIG_MAX_REFIL_CYCLES_SHIFT;
	write32(val, rng + RNG_CONFIG);

	/* Configure the desired FROs */
	write32(0x0, rng + RNG_FRODETUNE);

	/* Enable all FROs */
	write32(0xffffff, rng + RNG_FROENABLE);

	/* Select the maximum number of samples after
	 * which if a repeating pattern is still detected, an
	 * alarm event is generated
	 */
	val = RNG_ALARM_THRESHOLD << RNG_ALARMCNT_ALARM_TH_SHIFT;

	/* Set the shutdown threshold to the number of FROs
	 * allowed to be shut downed
	 */
	val |= RNG_SHUTDOWN_THRESHOLD << RNG_ALARMCNT_SHUTDOWN_TH_SHIFT;
	write32(val, rng + RNG_ALARMCNT);

	/* Enable the RNG module */
	val = RNG_CONTROL_STARTUP_CYCLES << RNG_CONTROL_STARTUP_CYCLES_SHIFT;
	val |= ENABLE_TRNG;
	write32(val, rng + RNG_CONTROL);

	IMSG("DRA7x TRNG initialized");

	return TEE_SUCCESS;
}
driver_init(dra7_rng_init);
