// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (C) 2017, Fuzhou Rockchip Electronics Co., Ltd.
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

#include <common.h>
#include <console.h>
#include <cru.h>
#include <grf.h>
#include <initcall.h>
#include <io.h>
#include <kernel/delay.h>
#include <kernel/generic_boot.h>
#include <kernel/misc.h>
#include <kernel/panic.h>
#include <mm/core_mmu.h>
#include <mm/core_memprot.h>
#include <platform_config.h>
#include <sm/optee_smc.h>
#include <sm/psci.h>
#include <stdint.h>
#include <tee/entry_std.h>
#include <tee/entry_fast.h>

struct dram_data {
	uint32_t cru_mode_con;
	uint32_t cru_clksel0;
	uint32_t cru_clksel1;
	uint32_t cru_clksel10;
	uint32_t cru_clksel21;
	uint32_t cru_clkgate[CRU_CLKGATE_CON_CNT];
};

static struct dram_data dram_d;

static const uint32_t clks_gating_table[CRU_CLKGATE_CON_CNT] = {
	/* gate: 0-3 */
	0xefb8,
	0x0ff7,
	0xfff4,
	0x887f,
	/* gate: 4-7 */
	0x0030,
	0x00f8,
	0x07e0,
	0xc000,
	/* gate: 8-11 */
	0xff84,
	0xb047,
	0x1ca0,
	0x57ff,
	/* gate: 12-15 */
	0x0000,
	0x00ff,
	0x1cc0,
	0x000f,
};

static void clks_disable(void)
{
	uint32_t i;
	vaddr_t va_base = (vaddr_t)phys_to_virt_io(CRU_BASE);

	for (i = 0; i < CRU_CLKGATE_CON_CNT; i++) {
		dram_d.cru_clkgate[i] = read32(va_base + CRU_CLKGATE_CON(i));
		write32(BITS_WITH_WMASK(clks_gating_table[i], 0xffff, 0),
			va_base + CRU_CLKGATE_CON(i));
	}
}

static void clks_restore(void)
{
	uint32_t i;
	vaddr_t va_base = (vaddr_t)phys_to_virt_io(CRU_BASE);

	for (i = 0; i < CRU_CLKGATE_CON_CNT; i++)
		write32(BITS_WITH_WMASK(dram_d.cru_clkgate[i], 0xffff, 0),
			va_base + CRU_CLKGATE_CON(i));
}

static void pll_power_down(uint32_t pll)
{
	vaddr_t va_base = (vaddr_t)phys_to_virt_io(CRU_BASE);

	write32(PLL_SLOW_MODE(pll), va_base + CRU_MODE_CON);
	write32(PLL_POWER_DOWN, va_base + CRU_PLL_CON1(pll));
}

static void pll_power_up(uint32_t pll)
{
	vaddr_t va_base = (vaddr_t)phys_to_virt_io(CRU_BASE);

	write32(PLL_POWER_UP, va_base + CRU_PLL_CON1(pll));
}

static void pll_wait_lock(uint32_t pll)
{
	uint32_t loop = 0;
	vaddr_t va_base = (vaddr_t)phys_to_virt_io(CRU_BASE);

	while (!(read32(va_base + CRU_PLL_CON1(pll)) & PLL_LOCK) &&
	       (loop < 500)) {
		udelay(2);
		loop++;
	}

	if (!(read32(va_base + CRU_PLL_CON1(pll)) & PLL_LOCK)) {
		EMSG("PLL can't lock, index = %" PRIu32, pll);
		panic();
	}
}

/*
 * Select clock from external 24MHz OSC(slow mode) and power down plls,
 * then set frequency division of relevant bus to 24MHz.
 */
static void plls_power_down(void)
{
	vaddr_t va_base = (vaddr_t)phys_to_virt_io(CRU_BASE);

	dram_d.cru_clksel0 = read32(va_base + CRU_CLKSEL_CON(0));
	dram_d.cru_clksel1 = read32(va_base + CRU_CLKSEL_CON(1));
	dram_d.cru_clksel10 = read32(va_base + CRU_CLKSEL_CON(10));
	dram_d.cru_clksel21 = read32(va_base + CRU_CLKSEL_CON(21));
	dram_d.cru_mode_con = read32(va_base + CRU_MODE_CON);

	pll_power_down(GPLL_ID);
	pll_power_down(CPLL_ID);
	pll_power_down(APLL_ID);

	/* core */
	write32(BITS_WITH_WMASK(0, 0x1f, 0), va_base + CRU_CLKSEL_CON(0));
	write32(BITS_WITH_WMASK(0, 0xf, 0) | BITS_WITH_WMASK(0, 0x7, 4),
		va_base + CRU_CLKSEL_CON(1));

	/* peri aclk, hclk, pclk */
	write32(BITS_WITH_WMASK(0, 0x1f, 0) | BITS_WITH_WMASK(0, 0x3, 8) |
		BITS_WITH_WMASK(0, 0x7, 12),
		va_base + CRU_CLKSEL_CON(10));

	/* pdbus */
	write32(BITS_WITH_WMASK(0, 0x1f, 8), va_base + CRU_CLKSEL_CON(0));
	write32(BITS_WITH_WMASK(0, 0x3, 8) | BITS_WITH_WMASK(0, 0x7, 12),
		va_base + CRU_CLKSEL_CON(1));

	/* hdmi cec 32k */
	write32(BITS_WITH_WMASK(732, 0x3fff, 0) | BITS_WITH_WMASK(2, 0x3, 14),
		va_base + CRU_CLKSEL_CON(21));
}

static void plls_restore(void)
{
	vaddr_t va_base = (vaddr_t)phys_to_virt_io(CRU_BASE);

	/* power up plls */
	pll_power_up(APLL_ID);
	pll_power_up(GPLL_ID);
	pll_power_up(CPLL_ID);

	udelay(200);

	/* wait lock*/
	pll_wait_lock(APLL_ID);
	pll_wait_lock(GPLL_ID);
	pll_wait_lock(CPLL_ID);

	/* hdmi cec 32k */
	write32(dram_d.cru_clksel21 | BITS_WMSK(0x3fff, 0) | BITS_WMSK(0x3, 14),
		va_base + CRU_CLKSEL_CON(21));

	/* pdbus */
	write32(dram_d.cru_clksel0 | BITS_WMSK(0x1f, 8),
		va_base + CRU_CLKSEL_CON(0));
	write32(dram_d.cru_clksel1 | BITS_WMSK(0x3, 8) | BITS_WMSK(0x7, 12),
		va_base + CRU_CLKSEL_CON(1));

	/* peri aclk, hclk, pclk */
	write32(dram_d.cru_clksel10 | BITS_WMSK(0x1f, 0) | BITS_WMSK(0x3, 8) |
		BITS_WMSK(0x7, 12),
		va_base + CRU_CLKSEL_CON(10));

	/* core */
	write32(dram_d.cru_clksel0 | BITS_WMSK(0x1f, 0),
		va_base + CRU_CLKSEL_CON(0));
	write32(dram_d.cru_clksel1 | BITS_WMSK(0xf, 0) | BITS_WMSK(0x7, 4),
		va_base + CRU_CLKSEL_CON(1));

	/* resume plls mode */
	write32(dram_d.cru_mode_con | BITS_WMSK(0x1, PLL_MODE_BIT(APLL_ID)),
		va_base + CRU_MODE_CON);
	write32(dram_d.cru_mode_con | BITS_WMSK(0x1, PLL_MODE_BIT(CPLL_ID)),
		va_base + CRU_MODE_CON);
	write32(dram_d.cru_mode_con | BITS_WMSK(0x1, PLL_MODE_BIT(GPLL_ID)),
		va_base + CRU_MODE_CON);
}

static bool wait_core_wfe_i(uint32_t core)
{
	uint32_t wfei_mask, loop = 0;
	vaddr_t va_base = (vaddr_t)phys_to_virt_io(GRF_BASE);

	wfei_mask = CORE_WFE_I_MASK(core);
	while (!(read32(va_base + GRF_CPU_STATUS1) & wfei_mask) && loop < 500) {
		udelay(2);
		loop++;
	}

	return read32(va_base + GRF_CPU_STATUS1) & wfei_mask;
}

static bool core_held_in_reset(uint32_t core)
{
	uint32_t val;
	vaddr_t va_base = (vaddr_t)phys_to_virt_io(CRU_BASE);

	val = read32(va_base + CRU_SOFTRST_CON(0));

	return val & CORE_HELD_IN_RESET(core);
}

uint32_t psci_version(void)
{
	return PSCI_VERSION_1_0;
}

int psci_features(uint32_t psci_fid)
{
	switch (psci_fid) {
	case PSCI_PSCI_FEATURES:
	case PSCI_VERSION:
	case PSCI_CPU_ON:
	case PSCI_CPU_OFF:
	case PSCI_SYSTEM_SUSPEND:
	case PSCI_SYSTEM_RESET:
		return PSCI_RET_SUCCESS;
	default:
		return PSCI_RET_NOT_SUPPORTED;
	}
}

int psci_cpu_on(uint32_t core_idx, uint32_t entry,
		uint32_t context_id __unused)
{
	bool wfei;
	vaddr_t cru_base = (vaddr_t)phys_to_virt_io(CRU_BASE);
	vaddr_t isram_base = (vaddr_t)phys_to_virt_io(ISRAM_BASE);

	core_idx &= MPIDR_CPU_MASK;
	if ((core_idx == 0) || (core_idx >= CFG_TEE_CORE_NB_CORE))
		return PSCI_RET_INVALID_PARAMETERS;

	DMSG("core_id: %" PRIu32, core_idx);

	/* set secondary cores' NS entry addresses */
	ns_entry_addrs[core_idx] = entry;

	/* wait */
	if (!core_held_in_reset(core_idx)) {
		wfei = wait_core_wfe_i(core_idx);
		if (!wfei) {
			EMSG("Can't wait cpu%" PRIu32 " wfei before softrst",
			     core_idx);
			return PSCI_RET_DENIED;
		}
	}

	/* soft reset core */
	write32(CORE_SOFT_RESET(core_idx), cru_base + CRU_SOFTRST_CON(0));
	dsb();

	udelay(2);

	/* soft release core */
	write32(CORE_SOFT_RELEASE(core_idx), cru_base + CRU_SOFTRST_CON(0));
	dsb();

	/* wait */
	wfei = wait_core_wfe_i(core_idx);
	if (!wfei) {
		EMSG("Can't wait cpu%" PRIu32 " wfei after softrst", core_idx);
		return PSCI_RET_DENIED;
	}

	/* set secondary secure entry address and lock tag */
	write32(CFG_TEE_LOAD_ADDR, isram_base + BOOT_ADDR_OFFSET);
	write32(LOCK_TAG, isram_base + LOCK_ADDR_OFFSET);
	dsb();

	sev();
	dsb();

	return PSCI_RET_SUCCESS;
}

int psci_cpu_off(void)
{
	uint32_t core = get_core_pos();

	if ((core == 0) || (core >= CFG_TEE_CORE_NB_CORE))
		return PSCI_RET_INVALID_PARAMETERS;

	DMSG("core_id: %" PRIu32, core);

	psci_armv7_cpu_off();
	thread_mask_exceptions(THREAD_EXCP_ALL);

	while (1)
		wfi();

	return PSCI_RET_INTERNAL_FAILURE;
}

int psci_affinity_info(uint32_t affinity,
		       uint32_t lowest_affnity_level __unused)
{
	uint32_t core_idx = affinity & MPIDR_CPU_MASK;
	uint32_t wfi_mask = CORE_WFI_MASK(core_idx);
	vaddr_t va_base = (vaddr_t)phys_to_virt_io(GRF_BASE);

	DMSG("core_id: %" PRIu32 " STATUS: %" PRIx32 " MASK: %" PRIx32,
	     core_idx, read32(va_base + GRF_CPU_STATUS1), wfi_mask);

	return (read32(va_base + GRF_CPU_STATUS1) & wfi_mask) ?
		PSCI_AFFINITY_LEVEL_OFF : PSCI_AFFINITY_LEVEL_ON;
}

void psci_system_reset(void)
{
	vaddr_t va_base = (vaddr_t)phys_to_virt_io(CRU_BASE);

	/* PLLs enter slow mode */
	write32(PLLS_SLOW_MODE, va_base + CRU_MODE_CON);
	dsb();

	/* Global second reset */
	write32(CRU_SNDRST_VAL, va_base + CRU_SNDRST_VAL_BASE);
	dsb();
}

int psci_system_suspend(uintptr_t entry __unused,
			uint32_t context_id __unused,
			struct sm_nsec_ctx *nsec __unused)
{
	DMSG("system suspend");

	clks_disable();
	plls_power_down();

	cache_op_inner(DCACHE_CLEAN_INV, NULL, 0);

	wfi();

	plls_restore();
	clks_restore();

	return PSCI_RET_SUCCESS;
}

/* When SMP bootup, we release cores one by one */
static TEE_Result reset_nonboot_cores(void)
{
	vaddr_t va_base = (vaddr_t)phys_to_virt_io(CRU_BASE);

	write32(NONBOOT_CORES_SOFT_RESET, va_base + CRU_SOFTRST_CON(0));

	return TEE_SUCCESS;
}

service_init_late(reset_nonboot_cores);
