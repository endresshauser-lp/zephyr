/*
 * Copyright (c) 2025 Endress+Hauser GmbH+Co. KG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* TODO: Rename this driver if it specifically requires STM32 FMC NAND */
#define DT_DRV_COMPAT micron_mt29f4g08

#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/flash/nand_flash_api_ex.h>

#include "flash_stm32_fmc_nand.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(flash_mt29f4g08, CONFIG_FLASH_LOG_LEVEL);

#define ECC_FEATURE_ADDR 0x90
#define ECC_FEATURE_DATA {0x08, 0x00, 0x00, 0x00}

struct flash_mt29f4g08_config {
	const struct device *controller;
	size_t page_size;
	size_t spare_area_size;
	size_t block_size;
	size_t plane_size;
	size_t flash_size;
};

static struct nand_flash_address
flash_mt29f4g08_calculate_address(const struct flash_mt29f4g08_config *config, off_t offset)
{
	off_t page_index = offset / config->page_size;
	off_t block_index = offset / config->block_size;
	off_t plane_index = offset / config->plane_size;

	return (struct nand_flash_address){
		.page = page_index % (config->block_size / config->page_size),
		.block = block_index % (config->plane_size / config->block_size),
		.plane = plane_index % (config->flash_size / config->plane_size),
	};
}

static int flash_mt29f4g08_read(const struct device *dev, off_t offset, void *data, size_t len)
{
	const struct flash_mt29f4g08_config *config = dev->config;
	const struct device *controller = config->controller;
	const struct flash_driver_api *controller_api =
		(const struct flash_driver_api *)controller->api;

	return controller_api->read(controller, offset, data, len);
}

static int flash_mt29f4g08_write(const struct device *dev, off_t offset, const void *data,
				 size_t len)
{
	const struct flash_mt29f4g08_config *config = dev->config;
	const struct device *controller = config->controller;
	const struct flash_driver_api *controller_api =
		(const struct flash_driver_api *)controller->api;

	return controller_api->write(controller, offset, data, len);
}

static int flash_mt29f4g08_erase(const struct device *dev, off_t offset, size_t size)
{
	const struct flash_mt29f4g08_config *config = dev->config;
	const struct device *controller = config->controller;

	if ((offset < 0) || (offset >= config->flash_size) ||
	    (size > (config->flash_size - offset))) {
		return -EINVAL;
	}

	if (((offset % config->block_size) != 0) || ((size % config->block_size) != 0)) {
		return -EINVAL;
	}

	while (size > 0) {
		struct nand_flash_address address =
			flash_mt29f4g08_calculate_address(config, offset);
		int ret = flash_stm32_fmc_nand_erase_block(controller, &address);
		if (ret != 0) {
			LOG_ERR("Erasing block at page %d, block %d, plane %d failed with error %d",
				address.page, address.block, address.plane, ret);
			return ret;
		}
		offset += config->block_size;
		size -= config->block_size;
	}

	return 0;
}

static const struct flash_parameters *flash_mt29f4g08_get_parameters(const struct device *dev)
{
	const struct flash_mt29f4g08_config *config = dev->config;
	const struct device *controller = config->controller;
	const struct flash_driver_api *controller_api =
		(const struct flash_driver_api *)controller->api;

	return controller_api->get_parameters(controller);
}

static int flash_mt29f4g08_get_size(const struct device *dev, uint64_t *size)
{
	const struct flash_mt29f4g08_config *config = dev->config;
	const struct device *controller = config->controller;
	const struct flash_driver_api *controller_api =
		(const struct flash_driver_api *)controller->api;

	return controller_api->get_size(controller, size);
}

#if CONFIG_FLASH_PAGE_LAYOUT
static void flash_mt29f4g08_page_layout(const struct device *dev,
					const struct flash_pages_layout **layout,
					size_t *layout_size)
{
	const struct flash_mt29f4g08_config *config = dev->config;
	const struct device *controller = config->controller;
	const struct flash_driver_api *controller_api =
		(const struct flash_driver_api *)controller->api;

	controller_api->page_layout(controller, layout, layout_size);
}
#endif /* CONFIG_FLASH_PAGE_LAYOUT */

static int flash_mt29f4g08_init(const struct device *dev)
{
	const struct flash_mt29f4g08_config *config = dev->config;
	const struct device *controller = config->controller;
	const struct flash_driver_api *controller_api =
		(const struct flash_driver_api *)controller->api;

	if (!device_is_ready(controller)) {
		LOG_ERR("Parent flash controller %s is not ready", controller->name);
		return -ENODEV;
	}

	/* Reset NAND flash */
	int ret = controller_api->ex_op(controller, FLASH_EX_OP_RESET, 0, NULL);
	if (ret != 0) {
		LOG_ERR("NAND flash reset failed with error %d", ret);
		return -EIO;
	}

#ifdef CONFIG_FLASH_MT29F4G08_ECC
	/* Enable on-die ECC feature */
	struct nand_flash_feature ecc_feature = {
		.feature_addr = ECC_FEATURE_ADDR,
		.feature_data = ECC_FEATURE_DATA,
	};
	ret = controller_api->ex_op(controller, NAND_FLASH_SET_FEATURE, (uintptr_t)&ecc_feature,
				    NULL);
	if (ret != 0) {
		LOG_ERR("Enabling on-die ECC failed with error %d", ret);
		return -EIO;
	}
#endif /* CONFIG_FLASH_MT29F4G08_ECC */

	/* Check initial bad blocks */
	ret = controller_api->ex_op(controller, NAND_FLASH_CHECK_BLOCKS, 0, NULL);
	if (ret != 0) {
		LOG_ERR("Checking bad blocks failed with error %d", ret);
		return -EIO;
	}

	LOG_INF("MT29F4G08 flash initialized with controller %s", controller->name);

	return 0;
}

static DEVICE_API(flash, flash_mt29f4g08_api) = {
	.read = flash_mt29f4g08_read,
	.write = flash_mt29f4g08_write,
	.erase = flash_mt29f4g08_erase,
	.get_parameters = flash_mt29f4g08_get_parameters,
	.get_size = flash_mt29f4g08_get_size,
#ifdef CONFIG_FLASH_PAGE_LAYOUT
	.page_layout = flash_mt29f4g08_page_layout,
#endif
};

#define FLASH_MT29F4G08_INIT(n)                                                                    \
	static const struct flash_mt29f4g08_config flash_mt29f4g08_config_##n = {                  \
		.controller = DEVICE_DT_GET(DT_PARENT(DT_DRV_INST(n))),                            \
		.page_size = DT_PROP(DT_DRV_INST(n), page_size),                                   \
		.spare_area_size = DT_PROP(DT_DRV_INST(n), spare_area_size),                       \
		.block_size = DT_PROP(DT_DRV_INST(n), block_size),                                 \
		.plane_size = DT_PROP(DT_DRV_INST(n), plane_size),                                 \
		.flash_size = DT_PROP(DT_DRV_INST(n), flash_size),                                 \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, flash_mt29f4g08_init, NULL, NULL, &flash_mt29f4g08_config_##n,    \
			      POST_KERNEL, CONFIG_FLASH_INIT_PRIORITY, &flash_mt29f4g08_api);

DT_INST_FOREACH_STATUS_OKAY(FLASH_MT29F4G08_INIT)
