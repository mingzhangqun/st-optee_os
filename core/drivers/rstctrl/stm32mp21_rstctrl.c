// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2021-2022, Linaro Limited
 * Copyright (c) 2018-2024, STMicroelectronics
 */

#include <drivers/rstctrl.h>
#include <drivers/stm32mp_dt_bindings.h>
#include <drivers/stm32mp21_rcc.h>
#include <io.h>
#include <kernel/delay.h>
#include <kernel/dt.h>
#include <kernel/dt_driver.h>
#include <kernel/panic.h>
#include <mm/core_memprot.h>
#include <stm32_util.h>

#include "stm32_rstctrl.h"

static TEE_Result stm32_reset_update(struct rstctrl *rstctrl, bool status,
				     unsigned int to_us)
{
	unsigned int id = to_rstline(rstctrl)->id;
	const struct stm32_reset_data *data = NULL;
	const struct stm32_reset_cfg *rst_line = NULL;
	vaddr_t address = stm32_rcc_base();
	uint32_t bit_mask = 0;

	data = to_rstline(rstctrl)->data;

	rst_line = data->rst_lines[id];
	if (!rst_line)
		return TEE_SUCCESS;

	address += rst_line->offset;
	bit_mask = BIT(rst_line->bit_idx);

	if (!status && rst_line->no_deassert)
		return TEE_SUCCESS;

	status = rst_line->inverted ^ status;

	if (status) {
		if (rst_line->set_clr)
			io_write32(address, bit_mask);
		else
			io_setbits32(address, bit_mask);
	} else {
		if (rst_line->set_clr)
			io_write32(address + RCC_MP_ENCLRR_OFFSET,
				   bit_mask);
		else
			io_clrbits32(address, bit_mask);
	}

	if (to_us && !rst_line->no_timeout) {
		uint64_t timeout_ref = timeout_init_us(to_us);

		while (((io_read32(address) & bit_mask) == bit_mask) ^ status)
			if (timeout_elapsed(timeout_ref))
				break;

		if (((io_read32(address) & bit_mask) == bit_mask) ^ status)
			return TEE_ERROR_SECURITY;
	}

	return TEE_SUCCESS;
}

static TEE_Result stm32_reset_assert(struct rstctrl *rstctrl,
				     unsigned int to_us)
{
	return stm32_reset_update(rstctrl, true, to_us);
}

static TEE_Result stm32_reset_deassert(struct rstctrl *rstctrl,
				       unsigned int to_us)
{
	return stm32_reset_update(rstctrl, false, to_us);
}

static struct rstctrl_ops stm32_rstctrl_ops = {
	.assert_level = stm32_reset_assert,
	.deassert_level = stm32_reset_deassert,
};

#define STM32_RESET(id, _offset, _bit_idx, _set_clr, _inverted, _no_deassert,\
		    _no_timeout)\
	[id] = &(struct stm32_reset_cfg){\
		.offset		= (_offset),\
		.bit_idx	= (_bit_idx),\
		.set_clr	= (_set_clr),\
		.inverted	= (_inverted),\
		.no_deassert	= (_no_deassert),\
		.no_timeout	= (_no_timeout),\
	}

#define RST(id, _offset, _bit_idx)\
	STM32_RESET(id, _offset, _bit_idx, false, false, false, false)

#define RST_SETR(id, _offset, _bit_idx)\
	STM32_RESET(id, _offset, _bit_idx, true, false, false, false)

#define RST_INV(id, _offset, _bit_idx)\
	STM32_RESET(id, _offset, _bit_idx, false, true, false, false)

#define RST_SETR_NO_DEASSERT(id, _offset, _bit_idx)\
	STM32_RESET(id, _offset, _bit_idx, false, false, true, false)

#define RST_SETR_NO_DEASSERT_TIMEOUT(id, _offset, _bit_idx)\
	STM32_RESET(id, _offset, _bit_idx, false, false, true, true)

static
const struct stm32_reset_cfg *stm32mp21_reset_cfg[STM32MP21_LAST_RESET] = {
	RST(TIM1_R,		RCC_TIM1CFGR,		0),
	RST(TIM2_R,		RCC_TIM2CFGR,		0),
	RST(TIM3_R,		RCC_TIM3CFGR,		0),
	RST(TIM4_R,		RCC_TIM4CFGR,		0),
	RST(TIM5_R,		RCC_TIM5CFGR,		0),
	RST(TIM6_R,		RCC_TIM6CFGR,		0),
	RST(TIM7_R,		RCC_TIM7CFGR,		0),
	RST(TIM8_R,		RCC_TIM8CFGR,		0),
	RST(TIM10_R,		RCC_TIM10CFGR,		0),
	RST(TIM11_R,		RCC_TIM11CFGR,		0),
	RST(TIM12_R,		RCC_TIM12CFGR,		0),
	RST(TIM13_R,		RCC_TIM13CFGR,		0),
	RST(TIM14_R,		RCC_TIM14CFGR,		0),
	RST(TIM15_R,		RCC_TIM15CFGR,		0),
	RST(TIM16_R,		RCC_TIM16CFGR,		0),
	RST(TIM17_R,		RCC_TIM17CFGR,		0),
	RST(LPTIM1_R,		RCC_LPTIM1CFGR,		0),
	RST(LPTIM2_R,		RCC_LPTIM2CFGR,		0),
	RST(LPTIM3_R,		RCC_LPTIM3CFGR,		0),
	RST(LPTIM4_R,		RCC_LPTIM4CFGR,		0),
	RST(LPTIM5_R,		RCC_LPTIM5CFGR,		0),
	RST(SPI1_R,		RCC_SPI1CFGR,		0),
	RST(SPI2_R,		RCC_SPI2CFGR,		0),
	RST(SPI3_R,		RCC_SPI3CFGR,		0),
	RST(SPI4_R,		RCC_SPI4CFGR,		0),
	RST(SPI5_R,		RCC_SPI5CFGR,		0),
	RST(SPI6_R,		RCC_SPI6CFGR,		0),
	RST(SPDIFRX_R,		RCC_SPDIFRXCFGR,	0),
	RST(USART1_R,		RCC_USART1CFGR,		0),
	RST(USART2_R,		RCC_USART2CFGR,		0),
	RST(USART3_R,		RCC_USART3CFGR,		0),
	RST(UART4_R,		RCC_UART4CFGR,		0),
	RST(UART5_R,		RCC_UART5CFGR,		0),
	RST(USART6_R,		RCC_USART6CFGR,		0),
	RST(UART7_R,		RCC_UART7CFGR,		0),
	RST(LPUART1_R,		RCC_LPUART1CFGR,	0),
	RST(I2C1_R,		RCC_I2C1CFGR,		0),
	RST(I2C2_R,		RCC_I2C2CFGR,		0),
	RST(I2C3_R,		RCC_I2C3CFGR,		0),
	RST(SAI1_R,		RCC_SAI1CFGR,		0),
	RST(SAI2_R,		RCC_SAI2CFGR,		0),
	RST(SAI3_R,		RCC_SAI3CFGR,		0),
	RST(SAI4_R,		RCC_SAI4CFGR,		0),
	RST(MDF1_R,		RCC_MDF1CFGR,		0),
	RST(FDCAN_R,		RCC_FDCANCFGR,		0),
	RST(HDP_R,		RCC_HDPCFGR,		0),
	RST(ADC1_R,		RCC_ADC1CFGR,		0),
	RST(ADC2_R,		RCC_ADC2CFGR,		0),
	RST(ETH1_R,		RCC_ETH1CFGR,		0),
	RST(ETH2_R,		RCC_ETH2CFGR,		0),
	RST(USBH_R,		RCC_USBHCFGR,		0),
	RST(OTG_R,		RCC_OTGCFGR,		0),
	RST(USB2PHY1_R,		RCC_USB2PHY1CFGR,	0),
	RST(USB2PHY2_R,		RCC_USB2PHY2CFGR,	0),
	RST(SDMMC1_R,		RCC_SDMMC1CFGR,		0),
	RST(SDMMC1DLL_R,	RCC_SDMMC1CFGR,		16),
	RST(SDMMC2_R,		RCC_SDMMC2CFGR,		0),
	RST(SDMMC2DLL_R,	RCC_SDMMC2CFGR,		16),
	RST(SDMMC3_R,		RCC_SDMMC3CFGR,		0),
	RST(SDMMC3DLL_R,	RCC_SDMMC3CFGR,		16),
	RST(LTDC_R,		RCC_LTDCCFGR,		0),
	RST(CSI_R,		RCC_CSICFGR,		0),
	RST(DCMIPP_R,		RCC_DCMIPPCFGR,		0),
	RST(DCMIPSSI_R,		RCC_DCMIPSSICFGR,	0),
	RST(WWDG1_R,		RCC_WWDG1CFGR,		0),
	RST(VREF_R,		RCC_VREFCFGR,		0),
	RST(DTS_R,		RCC_DTSCFGR,		0),
	RST(CRC_R,		RCC_CRCCFGR,		0),
	RST(SERC_R,		RCC_SERCCFGR,		0),
	RST(I3C1_R,		RCC_I3C1CFGR,		0),
	RST(I3C2_R,		RCC_I3C2CFGR,		0),
	RST(I3C3_R,		RCC_I3C3CFGR,		0),
	RST(RNG1_R,		RCC_RNG1CFGR,		0),
	RST(RNG2_R,		RCC_RNG2CFGR,		0),
	RST(PKA_R,		RCC_PKACFGR,		0),
	RST(SAES_R,		RCC_SAESCFGR,		0),
	RST(HASH1_R,		RCC_HASH1CFGR,		0),
	RST(HASH2_R,		RCC_HASH2CFGR,		0),
	RST(CRYP1_R,		RCC_CRYP1CFGR,		0),
	RST(CRYP2_R,		RCC_CRYP2CFGR,		0),
	RST(OSPI1_R,		RCC_OSPI1CFGR,		0),
	RST(OSPI1DLL_R,		RCC_OSPI1CFGR,		16),
	RST(DBG_R,		RCC_DBGCFGR,		12),
	RST_SETR(IWDG2_KER_R,	RCC_IWDGC1CFGSETR,	18),
	RST_SETR(IWDG4_KER_R,	RCC_IWDGC2CFGSETR,	18),
	RST_SETR(IWDG1_SYS_R,	RCC_IWDGC1CFGSETR,	0),
	RST_SETR(IWDG2_SYS_R,	RCC_IWDGC1CFGSETR,	2),
	RST_SETR(IWDG3_SYS_R,	RCC_IWDGC2CFGSETR,	0),
	RST_SETR(IWDG4_SYS_R,	RCC_IWDGC2CFGSETR,	2),

	RST_INV(C2_HOLDBOOT_R,	RCC_CPUBOOTCR,		0),
	RST_INV(C1_HOLDBOOT_R,	RCC_CPUBOOTCR,		1),
	RST_SETR_NO_DEASSERT_TIMEOUT(C1_R,	RCC_C1RSTCSETR,		0),
	RST_SETR_NO_DEASSERT_TIMEOUT(C2_R,	RCC_C2RSTCSETR,		0),
	RST_SETR_NO_DEASSERT_TIMEOUT(SYS_R,	RCC_GRSTCSETR,		0),

	/* Don't manage reset lines of RIF aware IP
	 * DDRCP_R, DDRCAPB_R, DDRPHYCAPB_R, DDRCFG_R, DDR_R,
	 * IPCC1_R,
	 * HPDMA1_R, HPDMA2_R, HPDMA3_R,
	 * GPIOA_R, GPIOB_R, GPIOC_R, GPIOD_R,
	 * GPIOE_R, GPIOF_R, GPIOG_R, GPIOH_R,
	 * GPIOI_R, GPIOZ_R,
	 * FMC_R,
	 */
};

static struct rstctrl_ops *stm32_reset_get_ops(unsigned int id __unused)
{
	return &stm32_rstctrl_ops;
}

static const struct stm32_reset_data stm32mp21_reset_data = {
	.nb_lines = ARRAY_SIZE(stm32mp21_reset_cfg),
	.rst_lines = stm32mp21_reset_cfg,
	.get_rstctrl_ops = stm32_reset_get_ops
};

static const struct dt_device_match stm32_rstctrl_match_table[] = {
	{
		.compatible = "st,stm32mp21-rcc",
		.compat_data = &stm32mp21_reset_data,
	},
	{ }
};

DEFINE_DT_DRIVER(stm32_rstctrl_dt_driver) = {
	.name = "stm32_rstctrl",
	.type = DT_DRIVER_RSTCTRL,
	.match_table = stm32_rstctrl_match_table,
	.probe = stm32_rstctrl_provider_probe,
};
