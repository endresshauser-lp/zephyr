/*
 * Copyright (c) 2025 Endress+Hauser GmbH+Co. KG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* TODO: Add documentation */

#ifndef __ZEPHYR_INCLUDE_DRIVERS_FLASH_NAND_FLASH_API_EX_H__
#define __ZEPHYR_INCLUDE_DRIVERS_FLASH_NAND_FLASH_API_EX_H__

#include <zephyr/drivers/flash.h>

enum nand_flash_ex_ops {
	NAND_FLASH_CHECK_BLOCKS = FLASH_EX_OP_VENDOR_BASE,
	NAND_FLASH_SET_FEATURE,
};

struct nand_flash_address {
	uint16_t page;
	uint16_t block;
	uint16_t plane;
};

struct nand_flash_feature {
	uint8_t feature_addr;
	uint8_t feature_data[4];
};

#endif /* __ZEPHYR_INCLUDE_DRIVERS_FLASH_NAND_FLASH_API_EX_H__ */
