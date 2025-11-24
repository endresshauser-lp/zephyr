/*
 * Copyright (c) 2025 Endress+Hauser GmbH+Co. KG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ZEPHYR_DRIVERS_FLASH_STM32_FMC_NAND_H__
#define __ZEPHYR_DRIVERS_FLASH_STM32_FMC_NAND_H__

#include <zephyr/drivers/flash/nand_flash_api_ex.h>

int flash_stm32_fmc_nand_erase_block(const struct device *dev,
				     const struct nand_flash_address *address);

#endif /* __ZEPHYR_DRIVERS_FLASH_STM32_FMC_NAND_H__ */
