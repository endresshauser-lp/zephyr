/*
 * Copyright (c) 2025 CodeWrights GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */


/*
 * __Ideas__
 *
 * - Split this driver in controller and flash as features
 *   such as On-Chip ECC and various acceleration features
 *   are vendor specific. This file should be the controller
 *   and chip-specific stuff can be handled by another chip
 *   specific driver which is a subnode of this node in DT.
 *
 * - Add ECC handling
 *   - On-Chip
 *   - Host
 *   - Software
 *
 * - Use DMA
 *
 * - Use ex_op API to expose acceleration feature for FTL
 */


#define DT_DRV_COMPAT st_stm32_fmc_nand

#include <stddef.h>
#include <stdint.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/memc/memc_stm32.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(flash_stm32_fmc_nand, CONFIG_FLASH_LOG_LEVEL);

#define PAGE_BUFFER_ALIGNMENT 4
#define PAGE_BUFFER_SIZE      2048 /* TODO: Move to kconfig */

struct flash_stm32_fmc_nand_config {
	struct flash_parameters parameters;
	size_t page_size;
	size_t block_size;
	size_t plane_size;
	size_t flash_size;
	unsigned char *page_buffer;
#ifdef CONFIG_FLASH_PAGE_LAYOUT
	struct flash_pages_layout layout;
#endif /* CONFIG_FLASH_PAGE_LAYOUT */
};

struct flash_stm32_fmc_nand_data {
	NAND_HandleTypeDef nand;
};

NAND_AddressTypeDef flash_stm32_fmc_nand_calculate_address(const struct device *dev, off_t addr)
{
	const struct flash_stm32_fmc_nand_config *config = dev->config;

	off_t page = addr / config->page_size;
	off_t block = addr / config->block_size;
	off_t plane = block / config->plane_size;

	NAND_AddressTypeDef nand_addr = {
		.Page = page % (config->block_size / config->page_size),
		.Block = block % (config->plane_size / config->block_size),
		.Plane = plane % (config->flash_size / config->plane_size),
	};

	return nand_addr;
}

static int flash_stm32_fmc_nand_erase(const struct device *dev, off_t addr, size_t size)
{
	struct flash_stm32_fmc_nand_data *data = dev->data;
	const struct flash_stm32_fmc_nand_config *config = dev->config;

	/* validate address and size */
	if ((addr < 0) || (addr >= config->flash_size) || (size > config->flash_size) ||
	    ((config->flash_size - addr) < size)) {
		return -EINVAL;
	}

	/* address must be block-aligned */
	if ((addr % config->block_size) != 0) {
		return -EINVAL;
	}

	/* size must be a multiple of blocks */
	if ((size % config->block_size) != 0) {
		return -EINVAL;
	}

	while (size > 0) {
		NAND_AddressTypeDef nand_addr = flash_stm32_fmc_nand_calculate_address(dev, addr);

		int ret = HAL_NAND_Erase_Block(&data->nand, &nand_addr);
		if (ret != HAL_OK) {
			LOG_ERR("HAL_NAND_Erase_Block() failed with error %d", ret);
			return -EIO;
		}

		addr += config->block_size;
		size -= config->block_size;
	}

	return 0;
}

static int flash_stm32_fmc_nand_write(const struct device *dev, off_t addr, const void *src,
				      size_t size)
{
	struct flash_stm32_fmc_nand_data *data = dev->data;
	const struct flash_stm32_fmc_nand_config *config = dev->config;

	/* validate address and size */
	if ((addr < 0) || (addr >= config->flash_size) || (size > config->flash_size) ||
	    ((config->flash_size - addr) < size)) {
		return -EINVAL;
	}

	/* addr must be page-aligned */
	if ((addr % config->page_size) != 0) {
		return -EINVAL;
	}

	/* size must be a multiple of page */
	if ((size % config->page_size) != 0) {
		return -EINVAL;
	}

	while (size > 0) {
		NAND_AddressTypeDef nand_addr = flash_stm32_fmc_nand_calculate_address(dev, addr);

		memcpy(config->page_buffer, src, config->page_size);

		int ret = HAL_NAND_Write_Page_8b(&data->nand, &nand_addr, config->page_buffer, 1);
		if (ret != HAL_OK) {
			LOG_ERR("HAL_NAND_Write_Page_8b() failed with error %d", ret);
			return -EIO;
		}

		src = (const uint8_t *)src + config->page_size;
		addr += config->page_size;
		size -= config->page_size;
	}

	return 0;
}

static int flash_stm32_fmc_nand_read(const struct device *dev, off_t addr, void *dest, size_t size)
{
	struct flash_stm32_fmc_nand_data *data = dev->data;
	const struct flash_stm32_fmc_nand_config *config = dev->config;

	/* validate address and size */
	if ((addr < 0) || (addr >= config->flash_size) || (size > config->flash_size) ||
	    ((config->flash_size - addr) < size)) {
		return -EINVAL;
	}

	while (size > 0) {
		off_t offset = addr % config->page_size;
		size_t chunk =
			((offset + size) < config->page_size) ? size : (config->page_size - offset);

		NAND_AddressTypeDef nand_addr = flash_stm32_fmc_nand_calculate_address(dev, addr);

		int ret = HAL_NAND_Read_Page_8b(&data->nand, &nand_addr, config->page_buffer, 1);

		if (ret != HAL_OK) {
			LOG_ERR("HAL_NAND_Read_Page_8b() failed with error %d", ret);
			return -EIO;
		}

		memcpy(dest, &config->page_buffer[offset], chunk);
		dest = (uint8_t *)dest + chunk;
		addr += chunk;
		size -= chunk;
	}

	return 0;
}

static const struct flash_parameters *flash_stm32_fmc_nand_get_parameters(const struct device *dev)
{
	const struct flash_stm32_fmc_nand_config *config = dev->config;

	return &config->parameters;
}

static int flash_stm32_fmc_nand_get_size(const struct device *dev, uint64_t *size)
{
	const struct flash_stm32_fmc_nand_config *config = dev->config;

	*size = config->flash_size;

	return 0;
}

#if CONFIG_FLASH_PAGE_LAYOUT
void flash_stm32_fmc_nand_page_layout(const struct device *dev,
				      const struct flash_pages_layout **layout, size_t *layout_size)
{
	const struct flash_stm32_fmc_nand_config *config = dev->config;

	*layout = &config->layout;
	*layout_size = 1;
}
#endif /* CONFIG_FLASH_PAGE_LAYOUT */

static int flash_stm32_fmc_nand_init(const struct device *dev)
{
	struct flash_stm32_fmc_nand_data *data = dev->data;
	const struct flash_stm32_fmc_nand_config *config = dev->config;

	LOG_DBG("flash_stm32_fmc_nand_init(%p) called", dev);

	uint32_t fmc_freq;
	memc_stm32_fmc_clock_rate(&fmc_freq);
	LOG_DBG("FMC clock rate: %d Hz", fmc_freq);

	data->nand.Instance = FMC_NAND_DEVICE;

	/* TODO: Load NAND parameters from Device Tree */
	data->nand.Init.NandBank = FMC_NAND_BANK3;
	data->nand.Init.Waitfeature = FMC_NAND_WAIT_FEATURE_ENABLE;
	data->nand.Init.MemoryDataWidth = FMC_NAND_MEM_BUS_WIDTH_8;
	data->nand.Init.EccComputation = FMC_NAND_ECC_DISABLE;
	data->nand.Init.ECCPageSize = FMC_NAND_ECC_PAGE_SIZE_512BYTE;
	data->nand.Init.TCLRSetupTime = 0;
	data->nand.Init.TARSetupTime = 0;

	data->nand.Config.PageSize = 2048;
	data->nand.Config.SpareAreaSize = 64;
	data->nand.Config.BlockSize = 64;
	data->nand.Config.BlockNbr = 2048;
	data->nand.Config.PlaneNbr = 2;
	data->nand.Config.PlaneSize = 2048;
	data->nand.Config.ExtraCommandEnable = DISABLE;

	FMC_NAND_PCC_TimingTypeDef com_space_timing = {
		.SetupTime = 0,
		.WaitSetupTime = 2,
		.HoldSetupTime = 1,
		.HiZSetupTime = 0
	};

	FMC_NAND_PCC_TimingTypeDef att_space_timing = {
		.SetupTime = 0,
		.WaitSetupTime = 2,
		.HoldSetupTime = 1,
		.HiZSetupTime = 0
	};

	int ret;

	ret = HAL_NAND_Init(&data->nand, &com_space_timing, &att_space_timing);
	if (ret != HAL_OK) {
		LOG_ERR("HAL_NAND_Init() failed with error %d", ret);
		return -EIO;
	}

	ret = HAL_NAND_Reset(&data->nand);
	if (ret != HAL_OK) {
		LOG_ERR("HAL_NAND_Reset() failed with error %d", ret);
		return -EIO;
	}

	NAND_IDTypeDef nand_id = {0};
	ret = HAL_NAND_Read_ID(&data->nand, &nand_id);
	if (ret != HAL_OK) {
		LOG_ERR("HAL_NAND_Read_ID() failed with error %d", ret);
		return -EIO;
	}

	LOG_INF("Flash found! ID: %02X %02X %02X %02X",
		nand_id.Maker_Id, nand_id.Device_Id,
		nand_id.Third_Id, nand_id.Fourth_Id);

	// TODO: Verify this works correctly, no bad blocks detected for my chip
	for (size_t block_id = 0; block_id < 4096; block_id++) {
		NAND_AddressTypeDef nand_addr;
		bool is_bad = false;

		/* Check first page of block */
		nand_addr = flash_stm32_fmc_nand_calculate_address(dev, block_id * 64 *
										config->page_size);
		ret = HAL_NAND_Read_SpareArea_8b(&data->nand, &nand_addr, config->page_buffer, 1);

		if (ret != HAL_OK) {
			LOG_ERR("HAL_NAND_Read_SpareArea_8b() failed with error %d", ret);
			return -EIO;
		}

		for (size_t i = 0; i < 64; i++) {
			if (config->page_buffer[i] == 0x00) {
				is_bad = true;
				LOG_HEXDUMP_DBG(config->page_buffer, 64, "Spare area data:");
				break;
			}
		}

		/* Check last page of block */
		nand_addr = flash_stm32_fmc_nand_calculate_address(
			dev, block_id * 64 * config->page_size + 63 * config->page_size);
		ret = HAL_NAND_Read_SpareArea_8b(&data->nand, &nand_addr, config->page_buffer, 1);

		if (ret != HAL_OK) {
			LOG_ERR("HAL_NAND_Read_SpareArea_8b() failed with error %d", ret);
			return -EIO;
		}

		for (size_t i = 0; i < 64; i++) {
			if (config->page_buffer[i] == 0x00) {
				is_bad = true;
				LOG_HEXDUMP_DBG(config->page_buffer, 64, "Spare area data:");
				break;
			}
		}

		if (is_bad) {
			LOG_WRN("Block %zu is bad!", block_id);
		}
	}

	return 0;
}

static DEVICE_API(flash, flash_stm32_fmc_nand_api) = {
	.erase = flash_stm32_fmc_nand_erase,
	.write = flash_stm32_fmc_nand_write,
	.read = flash_stm32_fmc_nand_read,
	.get_parameters = flash_stm32_fmc_nand_get_parameters,
	.get_size = flash_stm32_fmc_nand_get_size,
#ifdef CONFIG_FLASH_PAGE_LAYOUT
	.page_layout = flash_stm32_fmc_nand_page_layout,
#endif
};

/* TODO: Adjust parameters based on Device Tree */
#define LAYOUT_PAGES_PROP(n)                                                                       \
	IF_ENABLED(CONFIG_FLASH_PAGE_LAYOUT,                                                       \
		(.layout = {                                                                       \
			.pages_count = 2048 * 2,                                                   \
			.pages_size = 2048 * 64,                                                   \
		},                                                                                 \
		))

#define FLASH_STM32_FMC_NAND_INIT(n)                                                               \
	static unsigned char __nocache __aligned(PAGE_BUFFER_ALIGNMENT)                            \
	flash_stm32_fmc_nand_page_buffer_##n[PAGE_BUFFER_SIZE];                                    \
                                                                                                   \
	static const struct flash_stm32_fmc_nand_config flash_stm32_fmc_nand_config_##n = {        \
		.parameters =                                                                      \
			{                                                                          \
				.write_block_size = 2048,                                          \
				.erase_value = 0xff,                                               \
			},                                                                         \
		.page_size = 2048,                                                                 \
		.block_size = 2048 * 64,                                                           \
		.plane_size = 2048 * 64 * 2048,                                                    \
		.flash_size = 2048 * 64 * 2048 * 2,                                                \
		.page_buffer = flash_stm32_fmc_nand_page_buffer_##n,                               \
		LAYOUT_PAGES_PROP(n)};                                                             \
                                                                                                   \
	static struct flash_stm32_fmc_nand_data flash_stm32_fmc_nand_data_##n;                     \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, flash_stm32_fmc_nand_init, NULL, &flash_stm32_fmc_nand_data_##n,  \
			      &flash_stm32_fmc_nand_config_##n, POST_KERNEL,                       \
			      CONFIG_FLASH_INIT_PRIORITY, &flash_stm32_fmc_nand_api);

DT_INST_FOREACH_STATUS_OKAY(FLASH_STM32_FMC_NAND_INIT)
