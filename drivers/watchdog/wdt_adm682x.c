/*
 * Copyright (c) 2025 Analog Devices, Inc.
 * Copyright (c) 2025 Endress+Hauser AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT adi_adm6823_watchdog

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <errno.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wdt_adm682x, CONFIG_WDT_LOG_LEVEL);

// Watchdog must be triggered at least every 1.12 seconds according to the
// datasheet.
#define ADM682X_MAX_WINDOW_MS 1120

struct adm682x_wdt_config {
	struct gpio_dt_spec wdi;
};

struct adm682x_wdt_data {
	bool is_enabled;
};

static int wdt_adm682x_disable(const struct device *dev)
{
	struct adm682x_wdt_data *data = dev->data;
	const struct adm682x_wdt_config *config = dev->config;

	if (!data->is_enabled) {
		return -EFAULT;
	}

	// Setting the WDI GPIO as input SHOULD deactivate the watchdog.
	// However, if there are external pull-up or pull-down resistors this has
	// no effect as the WDI pin must float in order to stop the watchdog.
	int err = gpio_pin_configure_dt(&config->wdi, GPIO_INPUT);
	if (err != 0) {
		return err;
	}

	data->is_enabled = false;
	return 0;
}

static int wdt_adm682x_feed(const struct device *dev, int channel_id)
{
	ARG_UNUSED(channel_id);
	const struct adm682x_wdt_config *config = dev->config;

	return gpio_pin_toggle_dt(&config->wdi);
}

static int wdt_adm682x_setup(const struct device *dev, uint8_t options)
{
	const struct adm682x_wdt_config *config = dev->config;
	const struct adm682x_wdt_data *data = dev->data;

	if (options != 0u) {
		return -ENOTSUP;
	}

	if (data->is_enabled) {
		return -EBUSY;
	}

	int err = gpio_pin_configure_dt(&config->wdi, GPIO_OUTPUT_INACTIVE);
	if (err != 0) {
		return err;
	}

	// Toggle once to reset timeout.
	err = gpio_pin_toggle_dt(&config->wdi);
	if (err != 0) {
		return err;
	}

	data->is_enabled = true;
	return 0;
}

static int wdt_adm682x_install_timeout(const struct device *dev, const struct wdt_timeout_cfg *cfg)
{
	int ret = 0;
	const struct adm682x_wdt_config *dev_config = dev->config;
	struct adm682x_wdt_data *dev_data = dev->data;

	if (dev_data->is_enabled) {
		return -EBUSY;
	}

	if ((config->window.min != 0U) || (config->window.max > ADM682X_MAX_WINDOW_MS)) {
		return -EINVAL;
	}

	// Always return channel 0 - timeout is rounded up to 1.12s.
	return 0;
}

static int wdt_adm682x_init(const struct device *dev)
{
	return 0;
}

static DEVICE_API(wdt, adm682x_wdt_api) = {
	.setup = wdt_adm682x_setup,
	.disable = wdt_adm682x_disable,
	.install_timeout = wdt_adm682x_install_timeout,
	.feed = wdt_adm682x_feed,
};

#define ADM682X_WDT_INIT(_num)                                                             \
	static struct adm682x_wdt_data adm682x_wdt_data##_num = {.is_enabled = false};         \
	static const struct adm682x_wdt_config adm682x_wdt_config##_num = {                    \
		.wdi = GPIO_DT_SPEC_GET(DT_INST(_num)),                                            \
	};                                                                                     \
	DEVICE_DT_INST_DEFINE(_num, wdt_adm682x_init, NULL, &adm682x_wdt_data##_num,           \
			      &adm682x_wdt_config##_num, POST_KERNEL,                                  \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &adm682x_wdt_api);

DT_INST_FOREACH_STATUS_OKAY(ADM682X_WDT_INIT)
