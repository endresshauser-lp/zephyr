/*
 * Copyright (c) 2025 Endress+Hauser GmbH+Co. KG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT micron_mt29f4g08

#include <zephyr/drivers/flash.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(flash_mt29f4g08, CONFIG_FLASH_LOG_LEVEL);

struct flash_mt29f4g08_config {
	const struct device *controller;
};

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
	const struct flash_driver_api *controller_api =
		(const struct flash_driver_api *)controller->api;

	return controller_api->erase(controller, offset, size);
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

	if (!device_is_ready(config->controller)) {
		LOG_ERR("Parent flash controller %s is not ready", config->controller->name);
		return -ENODEV;
	}

	LOG_INF("MT29F4G08 flash initialized with controller %s", config->controller->name);

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
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, flash_mt29f4g08_init, NULL, NULL, &flash_mt29f4g08_config_##n,    \
			      POST_KERNEL, CONFIG_FLASH_INIT_PRIORITY, &flash_mt29f4g08_api);

DT_INST_FOREACH_STATUS_OKAY(FLASH_MT29F4G08_INIT)
