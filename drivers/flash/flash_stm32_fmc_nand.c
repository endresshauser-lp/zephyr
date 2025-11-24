/*
 * Copyright (c) 2025 Endress+Hauser GmbH+Co. KG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT st_stm32_fmc_nand

#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/flash/nand_flash_api_ex.h>
#include <zephyr/drivers/memc/memc_stm32.h>

#include "flash_stm32_fmc_nand.h"

/* TODO: Does not work with multiple driver instances */
#define STM32_FMC_NAND_USE_DMA DT_NODE_HAS_PROP(DT_DRV_INST(0), dmas)

#if STM32_FMC_NAND_USE_DMA
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/dma_stm32.h>
#include <stm32_ll_dma.h>
#endif /* STM32_FMC_NAND_USE_DMA */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(flash_stm32_fmc_nand, CONFIG_FLASH_LOG_LEVEL);

#define PAGE_BUFFER_ALIGNMENT 4

#if STM32_FMC_NAND_USE_DMA
static const uint32_t table_src_size[] = {
	LL_DMA_SRC_DATAWIDTH_BYTE,
	LL_DMA_SRC_DATAWIDTH_HALFWORD,
	LL_DMA_SRC_DATAWIDTH_WORD,
};

static const uint32_t table_dest_size[] = {
	LL_DMA_DEST_DATAWIDTH_BYTE,
	LL_DMA_DEST_DATAWIDTH_HALFWORD,
	LL_DMA_DEST_DATAWIDTH_WORD,
};

/* Lookup table to set dma priority from the DTS */
static const uint32_t table_priority[] = {
	LL_DMA_LOW_PRIORITY_LOW_WEIGHT,
	LL_DMA_LOW_PRIORITY_MID_WEIGHT,
	LL_DMA_LOW_PRIORITY_HIGH_WEIGHT,
	LL_DMA_HIGH_PRIORITY,
};

struct stream {
	DMA_HandleTypeDef handle;
	DMA_TypeDef *reg;
	const struct device *dev;
	uint32_t channel;
	struct dma_config cfg;
	struct dma_block_config block_cfg;
};
#endif /* STM32_FMC_NAND_USE_DMA */

struct flash_stm32_fmc_nand_config {
	struct flash_parameters parameters;
	size_t page_size;
	size_t spare_area_size;
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
#if STM32_FMC_NAND_USE_DMA
	struct stream dma;
#endif /* STM32_FMC_NAND_USE_DMA */
};

static int flash_stm32_fmc_nand_set_feature(NAND_HandleTypeDef *hnand,
					    const struct nand_flash_feature *feature)
{
	uint32_t tickstart;
	uint32_t deviceaddress;

	/* Check the NAND controller state */
	if (hnand->State == HAL_NAND_STATE_BUSY) {
		return -EBUSY;
	} else if (hnand->State == HAL_NAND_STATE_READY) {
		/* Process Locked */
		__HAL_LOCK(hnand);

		/* Update the NAND controller state */
		hnand->State = HAL_NAND_STATE_BUSY;

		/* Identify the device address */
		deviceaddress = NAND_DEVICE;

		/* Send feature setting command sequence */
		*(__IO uint8_t *)((uint32_t)(deviceaddress | CMD_AREA)) = 0xEF;
		__DSB();
		*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) = feature->feature_addr;
		__DSB();
		*(__IO uint8_t *)deviceaddress = feature->feature_data[0];
		__DSB();
		*(__IO uint8_t *)deviceaddress = feature->feature_data[1];
		__DSB();
		*(__IO uint8_t *)deviceaddress = feature->feature_data[2];
		__DSB();
		*(__IO uint8_t *)deviceaddress = feature->feature_data[3];
		__DSB();

		/* Get tick */
		tickstart = HAL_GetTick();

		/* Read status until NAND is ready */
		while (HAL_NAND_Read_Status(hnand) != NAND_READY) {
			if ((HAL_GetTick() - tickstart) > NAND_WRITE_TIMEOUT) {
				/* Update the NAND controller state */
				hnand->State = HAL_NAND_STATE_ERROR;

				/* Process unlocked */
				__HAL_UNLOCK(hnand);

				return -ETIMEDOUT;
			}
		}

		/* Update the NAND controller state */
		hnand->State = HAL_NAND_STATE_READY;

		/* Process unlocked */
		__HAL_UNLOCK(hnand);
	} else {
		return -EIO;
	}

	return 0;
}

/* Copied HAL_NAND_Read_Page_8b() from stm32h5xx_hal_nand.c and modified to read one single page
 * with DMA transfer */
static HAL_StatusTypeDef flash_stm32_fmc_nand_read_page(NAND_HandleTypeDef *hnand,
							const NAND_AddressTypeDef *pAddress,
							uint8_t *pBuffer, DMA_HandleTypeDef *hdma)
{
	uint32_t tickstart;
	uint32_t deviceaddress;
	uint32_t nandaddress;

	/* Check the NAND controller state */
	if (hnand->State == HAL_NAND_STATE_BUSY) {
		return HAL_BUSY;
	} else if (hnand->State == HAL_NAND_STATE_READY) {
		/* Process Locked */
		__HAL_LOCK(hnand);

		/* Update the NAND controller state */
		hnand->State = HAL_NAND_STATE_BUSY;

		/* Identify the device address */
		deviceaddress = NAND_DEVICE;

		/* NAND raw address calculation */
		nandaddress = ARRAY_ADDRESS(pAddress, hnand);

		/* Send read page command sequence */
		*(__IO uint8_t *)((uint32_t)(deviceaddress | CMD_AREA)) = NAND_CMD_AREA_A;
		__DSB();

		/* Cards with page size <= 512 bytes */
		if ((hnand->Config.PageSize) <= 512U) {
			if (((hnand->Config.BlockSize) * (hnand->Config.BlockNbr)) <= 65535U) {
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) = 0x00U;
				__DSB();
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) =
					ADDR_1ST_CYCLE(nandaddress);
				__DSB();
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) =
					ADDR_2ND_CYCLE(nandaddress);
				__DSB();
			} else /* ((hnand->Config.BlockSize)*(hnand->Config.BlockNbr)) >
				  65535 */
			{
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) = 0x00U;
				__DSB();
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) =
					ADDR_1ST_CYCLE(nandaddress);
				__DSB();
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) =
					ADDR_2ND_CYCLE(nandaddress);
				__DSB();
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) =
					ADDR_3RD_CYCLE(nandaddress);
				__DSB();
			}
		} else /* (hnand->Config.PageSize) > 512 */
		{
			if (((hnand->Config.BlockSize) * (hnand->Config.BlockNbr)) <= 65535U) {
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) = 0x00U;
				__DSB();
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) = 0x00U;
				__DSB();
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) =
					ADDR_1ST_CYCLE(nandaddress);
				__DSB();
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) =
					ADDR_2ND_CYCLE(nandaddress);
				__DSB();
			} else /* ((hnand->Config.BlockSize)*(hnand->Config.BlockNbr)) >
				  65535 */
			{
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) = 0x00U;
				__DSB();
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) = 0x00U;
				__DSB();
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) =
					ADDR_1ST_CYCLE(nandaddress);
				__DSB();
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) =
					ADDR_2ND_CYCLE(nandaddress);
				__DSB();
				*(__IO uint8_t *)((uint32_t)(deviceaddress | ADDR_AREA)) =
					ADDR_3RD_CYCLE(nandaddress);
				__DSB();
			}
		}

		*(__IO uint8_t *)((uint32_t)(deviceaddress | CMD_AREA)) = NAND_CMD_AREA_TRUE1;
		__DSB();

		/* Get tick */
		tickstart = HAL_GetTick();

		/* Read status until NAND is ready */
		while (true) {
			uint32_t status = HAL_NAND_Read_Status(hnand);

			if (status == NAND_READY) {
				break;
			} else if (status == NAND_ERROR) {
				LOG_ERR("Uncorrectable ECC error detected");

				/* Update the NAND controller state
				   TODO: Correct error handling */
				hnand->State = HAL_NAND_STATE_READY;

				/* Process unlocked */
				__HAL_UNLOCK(hnand);

				return HAL_ERROR;
			} else if ((HAL_GetTick() - tickstart) > NAND_WRITE_TIMEOUT) {
				/* Update the NAND controller state */
				hnand->State = HAL_NAND_STATE_ERROR;

				/* Process unlocked */
				__HAL_UNLOCK(hnand);

				return HAL_TIMEOUT;
			}
		}

		/* Go back to read mode */
		*(__IO uint8_t *)((uint32_t)(deviceaddress | CMD_AREA)) = ((uint8_t)0x00);
		__DSB();

		/* Get Data into Buffer */
		if (HAL_DMA_Start(hdma, deviceaddress, (uint32_t)pBuffer, hnand->Config.PageSize) !=
		    HAL_OK) {
			return HAL_ERROR;
		}

		if (HAL_DMA_PollForTransfer(hdma, HAL_DMA_FULL_TRANSFER, 1000) != HAL_OK) {
			return HAL_ERROR;
		}

		/* Update the NAND controller state */
		hnand->State = HAL_NAND_STATE_READY;

		/* Process unlocked */
		__HAL_UNLOCK(hnand);
	} else {
		return HAL_ERROR;
	}

	return HAL_OK;
}

static NAND_AddressTypeDef flash_stm32_fmc_nand_calculate_address(const struct device *dev,
								  off_t addr)
{
	const struct flash_stm32_fmc_nand_config *config = dev->config;

	off_t page_index = addr / config->page_size;
	off_t block_index = addr / config->block_size;
	off_t plane_index = addr / config->plane_size;

	NAND_AddressTypeDef nand_addr = {
		.Page = page_index % (config->block_size / config->page_size),
		.Block = block_index % (config->plane_size / config->block_size),
		.Plane = plane_index % (config->flash_size / config->plane_size),
	};

	return nand_addr;
}

int flash_stm32_fmc_nand_erase_block(const struct device *dev, const struct nand_flash_address *address)
{
	struct flash_stm32_fmc_nand_data *dev_data = dev->data;
	NAND_AddressTypeDef nand_addr = {
		.Page = address->page,
		.Plane = address->plane,
		.Block = address->block,
	};

	int ret = HAL_NAND_Erase_Block(&dev_data->nand, &nand_addr);
	if (ret != HAL_OK) {
		LOG_ERR("HAL_NAND_Erase_Block() failed with error %d", ret);
		return -EIO;
	}

	return 0;
}

static int flash_stm32_fmc_nand_erase(const struct device *dev, off_t offset, size_t size)
{
	struct flash_stm32_fmc_nand_data *dev_data = dev->data;
	const struct flash_stm32_fmc_nand_config *config = dev->config;

	/* validate address and size */
	if ((offset < 0) || (offset >= config->flash_size) ||
	    (size > (config->flash_size - offset))) {
		return -EINVAL;
	}

	/* address must be block-aligned */
	if ((offset % config->block_size) != 0) {
		return -EINVAL;
	}

	/* size must be a multiple of blocks */
	if ((size % config->block_size) != 0) {
		return -EINVAL;
	}

	while (size > 0) {
		NAND_AddressTypeDef nand_addr = flash_stm32_fmc_nand_calculate_address(dev, offset);

		int ret = HAL_NAND_Erase_Block(&dev_data->nand, &nand_addr);
		if (ret != HAL_OK) {
			LOG_ERR("HAL_NAND_Erase_Block() failed with error %d", ret);
			return -EIO;
		}

		offset += config->block_size;
		size -= config->block_size;
	}

	return 0;
}

static int flash_stm32_fmc_nand_write(const struct device *dev, off_t offset, const void *data,
				      size_t len)
{
	struct flash_stm32_fmc_nand_data *dev_data = dev->data;
	const struct flash_stm32_fmc_nand_config *config = dev->config;

	/* validate address and size */
	if ((offset < 0) || (offset >= config->flash_size) || (len > config->flash_size) ||
	    ((config->flash_size - offset) < len)) {
		return -EINVAL;
	}

	/* address must be page-aligned */
	if ((offset % config->page_size) != 0) {
		return -EINVAL;
	}

	/* size must be a multiple of page */
	if ((len % config->page_size) != 0) {
		return -EINVAL;
	}

	while (len > 0) {
		int ret;
		NAND_AddressTypeDef nand_addr = flash_stm32_fmc_nand_calculate_address(dev, offset);

#if STM32_FMC_NAND_USE_DMA
		ret = dma_reload(dev_data->dma.dev, dev_data->dma.channel, (uint32_t)data,
				 (uint32_t)config->page_buffer, config->page_size);
		if (ret != 0) {
			LOG_ERR("Failed to reload DMA transfer on channel %d with error %d",
				dev_data->dma.channel, ret);
			return -EIO;
		}
#else
		memcpy(config->page_buffer, data, config->page_size);
#endif /* STM32_FMC_NAND_USE_DMA */

		ret = HAL_NAND_Write_Page_8b(&dev_data->nand, &nand_addr, config->page_buffer, 1);
		if (ret != HAL_OK) {
			LOG_ERR("HAL_NAND_Write_Page_8b() failed with error %d", ret);
			return -EIO;
		}

		data = (const uint8_t *)data + config->page_size;
		offset += config->page_size;
		len -= config->page_size;
	}

	return 0;
}

static int flash_stm32_fmc_nand_read(const struct device *dev, off_t offset, void *data, size_t len)
{
	struct flash_stm32_fmc_nand_data *dev_data = dev->data;
	const struct flash_stm32_fmc_nand_config *config = dev->config;

	/* validate address and size */
	if ((offset < 0) || (offset >= config->flash_size) || (len > config->flash_size) ||
	    ((config->flash_size - offset) < len)) {
		return -EINVAL;
	}

	while (len > 0) {
		off_t page_offset = offset % config->page_size;
		size_t chunk = ((page_offset + len) < config->page_size)
				       ? len
				       : (config->page_size - page_offset);

		NAND_AddressTypeDef nand_addr = flash_stm32_fmc_nand_calculate_address(dev, offset);

#if STM32_FMC_NAND_USE_DMA
		int ret = flash_stm32_fmc_nand_read_page(
			&dev_data->nand, &nand_addr, config->page_buffer, &dev_data->dma.handle);
		if (ret != HAL_OK) {
			LOG_ERR("Reading page from NAND failed with error %d", ret);
			return -EIO;
		}

		ret = dma_reload(dev_data->dma.dev, dev_data->dma.channel,
				 (uint32_t)(config->page_buffer + page_offset), (uint32_t)data,
				 chunk);
		if (ret != 0) {
			LOG_ERR("Failed to reload DMA transfer on channel %d with error %d",
				dev_data->dma.channel, ret);
			return -EIO;
		}
#else
		int ret =
			HAL_NAND_Read_Page_8b(&dev_data->nand, &nand_addr, config->page_buffer, 1);
		if (ret != HAL_OK) {
			LOG_ERR("HAL_NAND_Read_Page_8b() failed with error %d", ret);
			return -EIO;
		}

		memcpy(data, &config->page_buffer[page_offset], chunk);
#endif /* STM32_FMC_NAND_USE_DMA */

		data = (uint8_t *)data + chunk;
		offset += chunk;
		len -= chunk;
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
static void flash_stm32_fmc_nand_page_layout(const struct device *dev,
					     const struct flash_pages_layout **layout,
					     size_t *layout_size)
{
	const struct flash_stm32_fmc_nand_config *config = dev->config;

	*layout = &config->layout;
	*layout_size = 1;
}
#endif /* CONFIG_FLASH_PAGE_LAYOUT */

#if CONFIG_FLASH_EX_OP_ENABLED
int flash_stm32_fmc_nand_ex_op(const struct device *dev, uint16_t code, const uintptr_t in,
			       void *out)
{
	ARG_UNUSED(out);

	struct flash_stm32_fmc_nand_data *dev_data = dev->data;
	const struct flash_stm32_fmc_nand_config *config = dev->config;
	int ret = 0;

	switch (code) {
	case FLASH_EX_OP_RESET:
		/* Reset NAND flash */
		ret = HAL_NAND_Reset(&dev_data->nand);
		break;

	case NAND_FLASH_CHECK_BLOCKS:
		/* Check bad blocks in first page of each */
		const size_t block_count = config->flash_size / config->block_size;

		for (size_t block_id = 0; block_id < block_count; block_id++) {
			NAND_AddressTypeDef nand_addr = flash_stm32_fmc_nand_calculate_address(
				dev, block_id * config->block_size);
			ret = HAL_NAND_Read_SpareArea_8b(&dev_data->nand, &nand_addr,
							 config->page_buffer, 1);
			if (ret == HAL_OK && config->page_buffer[0] != 0xFF) {
				LOG_WRN("Block %zu is bad!", block_id);
				LOG_HEXDUMP_INF(config->page_buffer, config->spare_area_size,
						"Spare area data:");
			}
		}
		break;

	case NAND_FLASH_SET_FEATURE:
		/* Set feature */
		struct nand_flash_feature *feature = (struct nand_flash_feature *)in;
		ret = flash_stm32_fmc_nand_set_feature(&dev_data->nand, feature);
		break;

	default:
		ret = -ENOTSUP;
		break;
	}

	return -ret;
}
#endif /* CONFIG_FLASH_EX_OP_ENABLED */

static int flash_stm32_fmc_nand_init(const struct device *dev)
{
	struct flash_stm32_fmc_nand_data *dev_data = dev->data;
	const struct flash_stm32_fmc_nand_config *config = dev->config;

	/* TODO: Remove this and according Kconfig/header */
	uint32_t fmc_freq;
	memc_stm32_fmc_clock_rate(&fmc_freq);
	LOG_DBG("FMC clock rate: %d Hz", fmc_freq);

	dev_data->nand.Instance = FMC_NAND_DEVICE;

	dev_data->nand.Init.NandBank = FMC_NAND_BANK3;
	dev_data->nand.Init.Waitfeature = FMC_NAND_WAIT_FEATURE_ENABLE;
	dev_data->nand.Init.MemoryDataWidth = FMC_NAND_MEM_BUS_WIDTH_8;
	dev_data->nand.Init.EccComputation = FMC_NAND_ECC_DISABLE;
	dev_data->nand.Init.ECCPageSize = FMC_NAND_ECC_PAGE_SIZE_2048BYTE;
	dev_data->nand.Init.TCLRSetupTime = 0;
	dev_data->nand.Init.TARSetupTime = 0;

	dev_data->nand.Config.PageSize = (uint32_t)config->page_size;
	dev_data->nand.Config.SpareAreaSize = (uint32_t)config->spare_area_size;
	dev_data->nand.Config.BlockSize = (uint32_t)(config->block_size / config->page_size);
	dev_data->nand.Config.BlockNbr = (uint32_t)(config->flash_size / config->block_size);
	dev_data->nand.Config.PlaneNbr = (uint32_t)(config->flash_size / config->plane_size);
	dev_data->nand.Config.PlaneSize = (uint32_t)(config->plane_size / config->block_size);
	dev_data->nand.Config.ExtraCommandEnable = DISABLE;

	FMC_NAND_PCC_TimingTypeDef com_space_timing = {
		.SetupTime = 0, .WaitSetupTime = 2, .HoldSetupTime = 1, .HiZSetupTime = 0};

	FMC_NAND_PCC_TimingTypeDef att_space_timing = {
		.SetupTime = 0, .WaitSetupTime = 2, .HoldSetupTime = 1, .HiZSetupTime = 0};

	int ret;

	ret = HAL_NAND_Init(&dev_data->nand, &com_space_timing, &att_space_timing);
	if (ret != HAL_OK) {
		LOG_ERR("HAL_NAND_Init() failed with error %d", ret);
		return -EIO;
	}

	NAND_IDTypeDef nand_id = {0};
	ret = HAL_NAND_Read_ID(&dev_data->nand, &nand_id);
	if (ret != HAL_OK) {
		LOG_ERR("HAL_NAND_Read_ID() failed with error %d", ret);
		return -EIO;
	}

	LOG_INF("Flash found! ID: %02X %02X %02X %02X", nand_id.Maker_Id, nand_id.Device_Id,
		nand_id.Third_Id, nand_id.Fourth_Id);

#if STM32_FMC_NAND_USE_DMA
	/*
	 * DMA configuration
	 * Due to use of NAND HAL and Zephyr API in current driver,
	 * both HAL and Zephyr DMA drivers should be configured.
	 */
	if (!device_is_ready(dev_data->dma.dev)) {
		LOG_ERR("DMA %s device is not ready", dev_data->dma.dev->name);
		return -ENODEV;
	}

	/* Proceed to the Zephyr DMA driver init */
	/* Dummy configuration to avoid warnings in dma_config(). The correct addresses are set
	 * with dma_reload(). */
	dev_data->dma.block_cfg.source_address = (uint32_t)config->page_buffer;
	dev_data->dma.block_cfg.dest_address = (uint32_t)config->page_buffer;

	dev_data->dma.cfg.head_block = &dev_data->dma.block_cfg;

	ret = dma_config(dev_data->dma.dev, dev_data->dma.channel, &dev_data->dma.cfg);
	if (ret != 0) {
		LOG_ERR("Failed to configure DMA channel %d with error %d", dev_data->dma.channel,
			ret);
		return -EIO;
	}

	/* Proceed to the HAL DMA driver init */
	int index = find_lsb_set(dev_data->dma.cfg.source_data_size) - 1;

	/* Fill the structure for dma init */
	dev_data->dma.handle.Init.Request = 0; /* Zero for memory-to-memory transfer */
	dev_data->dma.handle.Init.Direction = DMA_MEMORY_TO_MEMORY;
	dev_data->dma.handle.Init.SrcInc = DMA_SINC_INCREMENTED;
	dev_data->dma.handle.Init.DestInc = DMA_DINC_INCREMENTED;
	dev_data->dma.handle.Init.SrcDataWidth = table_src_size[index];
	dev_data->dma.handle.Init.DestDataWidth = table_dest_size[index];
	dev_data->dma.handle.Init.Priority = table_priority[dev_data->dma.cfg.channel_priority];
	dev_data->dma.handle.Init.SrcBurstLength = 64;
	dev_data->dma.handle.Init.DestBurstLength = 64;
	dev_data->dma.handle.Init.TransferAllocatedPort =
		DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1;
	dev_data->dma.handle.Init.TransferEventMode = DMA_TCEM_BLOCK_TRANSFER;
	dev_data->dma.handle.Init.Mode = DMA_NORMAL;
	dev_data->dma.handle.Instance =
		STM32_DMA_GET_INSTANCE(dev_data->dma.reg, dev_data->dma.channel);

	if (HAL_DMA_Init(&dev_data->dma.handle) != HAL_OK) {
		LOG_ERR("FMC NAND DMA Init failed");
		return -EIO;
	}

	LOG_INF("FMC NAND with DMA transfer");
#endif /* STM32_FMC_NAND_USE_DMA */

	return 0;
}

/* This function is executed in the interrupt context */
#if STM32_FMC_NAND_USE_DMA
static void fmc_nand_dma_callback(const struct device *dev, void *user_data, uint32_t channel,
				  int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (status < 0) {
		LOG_ERR("DMA callback error %d with channel %d", status, channel);
	}
}
#endif /* STM32_FMC_NAND_USE_DMA */

/* TODO: Remove flash API */
static DEVICE_API(flash, flash_stm32_fmc_nand_api) = {
	.read = flash_stm32_fmc_nand_read,
	.write = flash_stm32_fmc_nand_write,
	.erase = flash_stm32_fmc_nand_erase,
	.get_parameters = flash_stm32_fmc_nand_get_parameters,
	.get_size = flash_stm32_fmc_nand_get_size,
#ifdef CONFIG_FLASH_PAGE_LAYOUT
	.page_layout = flash_stm32_fmc_nand_page_layout,
#endif /* CONFIG_FLASH_PAGE_LAYOUT */
#ifdef CONFIG_FLASH_EX_OP_ENABLED
	.ex_op = flash_stm32_fmc_nand_ex_op,
#endif /* CONFIG_FLASH_EX_OP_ENABLED */
};

#if STM32_FMC_NAND_USE_DMA
#define DMA_CHANNEL_CONFIG(node, dir) DT_DMAS_CELL_BY_NAME(node, dir, channel_config)

#define FMC_NAND_DMA_CHANNEL_INIT(node, dir)                                                       \
	.reg = (DMA_TypeDef *)DT_REG_ADDR(DT_PHANDLE_BY_NAME(node, dmas, dir)),                    \
	.dev = DEVICE_DT_GET(DT_DMAS_CTLR(node)),                                                  \
	.channel = DT_DMAS_CELL_BY_NAME(node, dir, channel),                                       \
	.cfg = {                                                                                   \
		.channel_direction = MEMORY_TO_MEMORY,                                             \
		.channel_priority = STM32_DMA_CONFIG_PRIORITY(DMA_CHANNEL_CONFIG(node, dir)),      \
		.source_data_size =                                                                \
			STM32_DMA_CONFIG_PERIPHERAL_DATA_SIZE(DMA_CHANNEL_CONFIG(node, dir)),      \
		.dest_data_size =                                                                  \
			STM32_DMA_CONFIG_MEMORY_DATA_SIZE(DMA_CHANNEL_CONFIG(node, dir)),          \
		.source_burst_length = 64,                                                         \
		.dest_burst_length = 64,                                                           \
		.block_count = 1,                                                                  \
		.dma_callback = fmc_nand_dma_callback,                                             \
	}

#define FMC_NAND_DMA_CHANNEL(node, dir)                                                            \
	.dma = {COND_CODE_1(DT_DMAS_HAS_NAME(node, dir),                                           \
			    (FMC_NAND_DMA_CHANNEL_INIT(node, dir)),                                \
			    NULL)},
#else
#define FMC_NAND_DMA_CHANNEL(node, dir)
#endif /* STM32_FMC_NAND_USE_DMA */

/* A page in this context corresponds to the smallest erasable area which is a block */
#define LAYOUT_PAGES_PROP(n)                                                                       \
	IF_ENABLED(CONFIG_FLASH_PAGE_LAYOUT,                                                       \
		(.layout = {                                                                       \
			.pages_count = DT_PROP(DT_DRV_INST(n), flash_size) /                       \
				       DT_PROP(DT_DRV_INST(n), block_size),                        \
			.pages_size = DT_PROP(DT_DRV_INST(n), block_size),                         \
		}))

#define FLASH_STM32_FMC_NAND_INIT(n)                                                               \
	static unsigned char __nocache __aligned(PAGE_BUFFER_ALIGNMENT)                            \
	flash_stm32_fmc_nand_page_buffer_##n[DT_PROP(DT_DRV_INST(n), page_size)];                  \
                                                                                                   \
	static const struct flash_stm32_fmc_nand_config flash_stm32_fmc_nand_config_##n = {        \
		.parameters =                                                                      \
			{                                                                          \
				.write_block_size = DT_PROP(DT_DRV_INST(n), page_size),            \
				.erase_value = 0xff,                                               \
			},                                                                         \
		.page_size = DT_PROP(DT_DRV_INST(n), page_size),                                   \
		.spare_area_size = DT_PROP(DT_DRV_INST(n), spare_area_size),                       \
		.block_size = DT_PROP(DT_DRV_INST(n), block_size),                                 \
		.plane_size = DT_PROP(DT_DRV_INST(n), plane_size),                                 \
		.flash_size = DT_PROP(DT_DRV_INST(n), flash_size),                                 \
		.page_buffer = flash_stm32_fmc_nand_page_buffer_##n,                               \
		LAYOUT_PAGES_PROP(n),                                                              \
	};                                                                                         \
                                                                                                   \
	static struct flash_stm32_fmc_nand_data flash_stm32_fmc_nand_data_##n = {                  \
		FMC_NAND_DMA_CHANNEL(DT_DRV_INST(n), tx_rx)};                                      \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, flash_stm32_fmc_nand_init, NULL, &flash_stm32_fmc_nand_data_##n,  \
			      &flash_stm32_fmc_nand_config_##n, POST_KERNEL,                       \
			      CONFIG_FLASH_INIT_PRIORITY, &flash_stm32_fmc_nand_api);

DT_INST_FOREACH_STATUS_OKAY(FLASH_STM32_FMC_NAND_INIT)
