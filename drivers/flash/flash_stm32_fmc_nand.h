/*
 * Copyright (c) 2025 Endress+Hauser GmbH+Co. KG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ZEPHYR_DRIVERS_FLASH_STM32_FMC_NAND_H__
#define __ZEPHYR_DRIVERS_FLASH_STM32_FMC_NAND_H__

#include <zephyr/drivers/flash/nand_flash_api_ex.h>

struct flash_stm32_fmc_nand_init {
	size_t page_size;
	size_t spare_area_size;
	size_t block_size;
	size_t plane_size;
	size_t flash_size;
};

int flash_stm32_fmc_nand_read_page(const struct device *dev,
				   const struct nand_flash_address *address, const uint8_t *data,
				   off_t page_offset, size_t chunk);

int flash_stm32_fmc_nand_read_spare_area(const struct device *dev,
					 const struct nand_flash_address *address, uint8_t *data);

int flash_stm32_fmc_nand_write_page(const struct device *dev,
				    const struct nand_flash_address *address, const uint8_t *data);

int flash_stm32_fmc_nand_erase_block(const struct device *dev,
				     const struct nand_flash_address *address);

int flash_stm32_fmc_nand_init_bank(const struct device *dev,
				   const struct flash_stm32_fmc_nand_init *init);

int flash_stm32_fmc_nand_reset(const struct device *dev);

int flash_stm32_fmc_nand_set_feature(const struct device *dev,
				     const struct nand_flash_feature *feature);

#endif /* __ZEPHYR_DRIVERS_FLASH_STM32_FMC_NAND_H__ */
