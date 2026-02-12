/*
 * Copyright (c) 2024, Ambiq Micro Inc. <www.ambiq.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT mspi_is66wvo8m8
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mspi.h>

LOG_MODULE_REGISTER(memc_mspi_is66wvo8m8, CONFIG_MEMC_LOG_LEVEL);

// Supported commands
#define CMD_READ_CONT_BURST     0xA000
#define CMD_READ_WRAP_BURST     0x8000
#define CMD_WRITE_CONT_BURST    0x2000
#define CMD_WRITE_WRAP_BURST    0x0000
#define CMD_READ_ID             0xC000
#define CMD_READ_CONFIG         0xE000
#define CMD_WRITE_CONFIG        0x4000
#define CMD_READ_PREABLE        0xF000

// Registers addresses
#define REGISTER_ADDRESS_ID     0x00000000
#define REGISTER_ADDRESS_CONFIG 0x00040000

// Register masks
#define REGISTER_ID_VOLTAGE_MASK        0xD000
#define REGISTER_ID_VOLTAGE_1V8         0x0000
#define REGISTER_ID_VOLTAGE_3V0         0x2000
#define REGISTER_ID_ROWBITS_MASK        0x1f00
#define REGISTER_ID_ROWBITS_SHIFT       8
#define REGISTER_ID_COLBITS_MASK        0x00F0
#define REGISTER_ID_COLBITS_SHIFT       4
#define REGISTER_ID_MANUFACTURER_MASK   0x000F
#define REGISTER_ID_MANUFACTURER_VALUE  0x3

#define REGISTER_CONFIG_DPD_DISABLE         0x8000
#define REGISTER_CONFIG_ODS_MASK            0x7000
#define REGISTER_CONFIG_ODS_SHIFT           12
#define REGISTER_CONFIG_PAR_MASK            0x0e00
#define REGISTER_CONFIG_PAR_SHIFT           9
#define REGISTER_CONFIG_DQSM_OCLK           0x0100
#define REGISTER_CONFIG_LATENCY_MASK        0x00F0
#define REGISTER_CONFIG_LATENCY_SHIFT       4
#define REGISTER_CONFIG_INITLATENCY_FIXED   0x0008
#define REGISTER_CONFIG_BURST_HYBRID        0x0004
#define REGISTER_CONFIG_BURST_MASK          0x0003
#define REGISTER_CONFIG_BURST_SHIFT         0

#define DEFAULT_CONFIG 0xf042

// Duration in microseconds to keep reset line asserted.
#define RESET_DELAY_US 200

struct memc_mspi_is66wvo8m8_config {
	uint32_t                       port;
	uint32_t                       mem_size;

	const struct device            *bus;
    const struct gpio_dt_spec      reset;
	struct mspi_dev_id             dev_id;
	struct mspi_dev_cfg            dev_cfg;
	struct mspi_xip_cfg            xip_cfg;
	struct mspi_scramble_cfg       scramble_cfg;

	bool                           sw_multi_periph;
	uint16_t                       target_cfg;
	uint32_t                       setup_tx_dummy;
	uint32_t                       setup_rx_dummy;
};

struct memc_mspi_is66wvo8m8_data {
	struct mspi_dev_cfg            dev_cfg;
	struct mspi_xip_cfg            xip_cfg;
	struct mspi_scramble_cfg       scramble_cfg;
	struct mspi_xfer               trans;
	struct mspi_xfer_packet        packet;

	struct k_sem                   lock;
};

static int memc_mspi_is66wvo8m8_command_write(const struct device *psram, uint16_t cmd, uint32_t addr,
						uint8_t *wdata, uint32_t length)
{
	const struct memc_mspi_is66wvo8m8_config *cfg = psram->config;
	struct memc_mspi_is66wvo8m8_data *data = psram->data;
	int ret;
	uint8_t buffer[16];

	data->packet.dir              = MSPI_TX;
	data->packet.cmd              = cmd;
	data->packet.address          = addr;
	data->packet.data_buf         = buffer;
	data->packet.num_bytes        = length;

	data->trans.async             = false;
	data->trans.xfer_mode         = MSPI_PIO;
	data->trans.tx_dummy          = data->dev_cfg.tx_dummy;
	data->trans.rx_dummy          = data->dev_cfg.rx_dummy;
	data->trans.cmd_length        = 2;
	data->trans.addr_length       = 4;
	data->trans.hold_ce           = false;
	data->trans.packets           = &data->packet;
	data->trans.num_packet        = 1;
	data->trans.timeout           = 10;

	if (wdata != NULL) {
		memcpy(buffer, wdata, length);
	}

	ret = mspi_transceive(cfg->bus, &cfg->dev_id, (const struct mspi_xfer *)&data->trans);
	if (ret) {
		LOG_ERR("MSPI write transaction failed with code: %d/%u", ret, __LINE__);
		return -EIO;
	}
	return ret;
}

static int memc_mspi_is66wvo8m8_command_read(const struct device *psram, uint16_t cmd, uint32_t addr,
					   uint8_t *rdata, uint32_t length)
{
	const struct memc_mspi_is66wvo8m8_config *cfg = psram->config;
	struct memc_mspi_is66wvo8m8_data *data = psram->data;

	int ret;
	uint8_t buffer[16];

	data->packet.dir              = MSPI_RX;
	data->packet.cmd              = cmd;
	data->packet.address          = addr;
	data->packet.data_buf         = buffer;
	data->packet.num_bytes        = length;

	data->trans.async             = false;
	data->trans.xfer_mode         = MSPI_PIO;
	data->trans.tx_dummy          = data->dev_cfg.tx_dummy;
	data->trans.rx_dummy          = data->dev_cfg.rx_dummy;
	data->trans.cmd_length        = 2;
	data->trans.addr_length       = 4;
	data->trans.hold_ce           = false;
	data->trans.packets           = &data->packet;
	data->trans.num_packet        = 1;
	data->trans.timeout           = 10;

	ret = mspi_transceive(cfg->bus, &cfg->dev_id, (const struct mspi_xfer *)&data->trans);
	if (ret) {
		LOG_ERR("MSPI read transaction failed with code: %d/%u", ret, __LINE__);
		return -EIO;
	}
	memcpy(rdata, buffer, length);
	return ret;
}

#if CONFIG_PM_DEVICE
static void acquire(const struct device *psram)
{
	const struct memc_mspi_is66wvo8m8_config *cfg = psram->config;
	struct memc_mspi_is66wvo8m8_data *data = psram->data;

	k_sem_take(&data->lock, K_FOREVER);

	if (cfg->sw_multi_periph) {
		while (mspi_dev_config(cfg->bus, &cfg->dev_id,
					   MSPI_DEVICE_CONFIG_ALL, &data->dev_cfg)) {
			;
		}
	} else {
		while (mspi_dev_config(cfg->bus, &cfg->dev_id,
					   MSPI_DEVICE_CONFIG_NONE, NULL)) {
			;
		}
	}
}
#endif /* CONFIG_PM_DEVICE */

static void release(const struct device *psram)
{
	const struct memc_mspi_is66wvo8m8_config *cfg = psram->config;
	struct memc_mspi_is66wvo8m8_data *data = psram->data;

	while (mspi_get_channel_status(cfg->bus, cfg->port)) {
		;
	}

	k_sem_give(&data->lock);
}

static int memc_mspi_is66wvo8m8_reset(const struct device *psram)
{
	LOG_DBG("Resetting is66wvo8m8/%u", __LINE__);

	int ret = 0;
	const struct memc_mspi_is66wvo8m8_config *cfg = psram->config;
	if (cfg->reset.port == NULL) {
		LOG_ERR("No hardware reset pin specified.");
		return -ENOTSUP;
	}

	// First, configure the reset line as output.
	ret = gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		return ret;
	}
	k_usleep(RESET_DELAY_US);

	ret = gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_ACTIVE);
	if (ret != 0) {
		return ret;
	}
	k_usleep(RESET_DELAY_US);

	ret = gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		return ret;
	}
	k_usleep(RESET_DELAY_US);

	return ret;
}

static int memc_mspi_is66wvo8m8_check_vendor_id(const struct device *psram)
{
	uint16_t id_register = 0;
	int ret;

	ret = memc_mspi_is66wvo8m8_command_read(psram, CMD_READ_ID, REGISTER_ADDRESS_ID,
			(uint8_t *)&id_register, sizeof(id_register));

	if (ret != 0) {
		LOG_ERR("Failed to read vendor ID: %d", ret);
		return ret;
	}

	id_register = sys_be16_to_cpu(id_register);
	LOG_DBG("Read ID buff: %x", id_register);
	LOG_DBG("Vendor id: 0x%0x", id_register & REGISTER_ID_MANUFACTURER_MASK);
	LOG_DBG("Device Voltage: %dmV", (id_register & REGISTER_ID_VOLTAGE_3V0) ? 3000 : 1800);
	LOG_DBG("Rows: %d", ((id_register & REGISTER_ID_ROWBITS_MASK) >> REGISTER_ID_ROWBITS_SHIFT) + 1);
	LOG_DBG("Cols: %d", ((id_register & REGISTER_ID_COLBITS_MASK) >> REGISTER_ID_COLBITS_SHIFT) + 1);

	if ((id_register & REGISTER_ID_MANUFACTURER_MASK) != REGISTER_ID_MANUFACTURER_VALUE) {
		LOG_WRN("Vendor ID does not match expected value of 0x%0x/%u", REGISTER_ID_MANUFACTURER_VALUE,
			__LINE__);
		return -EINVAL;
	}

	return ret;
}

static int memc_mspi_is66wvo8m8_read_config(const struct device *psram, uint16_t *cfg)
{
	uint16_t cfg_register = 0;
	int ret;

	ret = memc_mspi_is66wvo8m8_command_read(psram, CMD_READ_CONFIG, REGISTER_ADDRESS_CONFIG,
			(uint8_t *)&cfg_register, sizeof(cfg_register));

	if (ret != 0) {
		LOG_ERR("Failed to read vendor ID: %d", ret);
		return ret;
	}

	cfg_register = sys_be16_to_cpu(cfg_register);
	LOG_DBG("Read CFG buff: %x", cfg_register);

    if (ret == 0) {
        *cfg = cfg_register;
    }
	return ret;
}

static int memc_mspi_is66wvo8m8_write_config(const struct device *psram)
{
    uint16_t cfg_reg = 0;
    int ret = 0;

	struct memc_mspi_is66wvo8m8_data *data = psram->data;
	const struct memc_mspi_is66wvo8m8_config *cfg = psram->config;

    ret = memc_mspi_is66wvo8m8_read_config(psram, &cfg_reg);
	if (ret != 0) {
		LOG_ERR("Could not read configuration register: %d", ret);
		return -EIO;
	}

	if (cfg_reg != DEFAULT_CONFIG) {
		LOG_WRN("Expected default config 0x%04x but got 0x%04x", DEFAULT_CONFIG, cfg_reg);
	}

	uint16_t target_config = sys_cpu_to_be16(cfg->target_cfg);

	ret = memc_mspi_is66wvo8m8_command_write(psram, CMD_WRITE_CONFIG, REGISTER_ADDRESS_CONFIG,
			(uint8_t *)&target_config, sizeof(target_config));
	if (ret != 0) {
		LOG_ERR("Could not write configuration register: %d", ret);
		return -EIO;
	}

	// Configure new dummy cycles.
	data->dev_cfg.tx_dummy = cfg->dev_cfg.tx_dummy;
	data->dev_cfg.rx_dummy = cfg->dev_cfg.rx_dummy;

    ret = memc_mspi_is66wvo8m8_read_config(psram, &cfg_reg);
	if (ret != 0) {
		LOG_ERR("Could not read configuration register: %d", ret);
		return -EIO;
	}

	if (cfg_reg != sys_be16_to_cpu(target_config)) {
		LOG_ERR("Expected config %04x but got %04x", target_config, cfg_reg);
		return -EIO;
	}

	return 0;
}

#if CONFIG_PM_DEVICE
static int memc_mspi_is66wvo8m8_deep_sleep_enter(const struct device *psram)
{
	int ret;
	struct memc_mspi_is66wvo8m8_data *data = psram->data;

	LOG_DBG("Putting is66wvo8m8 to deep sleep/%u", __LINE__);
	uint16_t cfg_reg = 0;
    ret = memc_mspi_is66wvo8m8_read_config(psram, &cfg_reg);
	if (ret != 0) {
		LOG_ERR("Could not read configuration register: %d", ret);
		return -EIO;
	}

	uint16_t target_config = sys_cpu_to_be16(cfg->target_cfg & ~REGISTER_CONFIG_DPD_DISABLE);

	ret = memc_mspi_is66wvo8m8_command_write(psram, CMD_WRITE_CONFIG, REGISTER_ADDRESS_CONFIG,
			(uint8_t *)&target_config, sizeof(target_config));
	if (ret != 0) {
		LOG_ERR("Could not write configuration register: %d", ret);
		return -EIO;
	}

	/** Minimum half sleep duration tHS time */
	k_busy_wait(4);
    ret = memc_mspi_is66wvo8m8_read_config(psram, &cfg_reg);
	if (ret != 0) {
		LOG_ERR("Could not read configuration register: %d", ret);
		return -EIO;
	}

	if ((cfg_reg & REGISTER_CONFIG_DPD_DISABLE) != 0) {
		LOG_ERR("Failed to activate deep sleep/%u", __LINE__);
		return -EIO;
	}

	return ret;
}

static int memc_mspi_is66wvo8m8_deep_sleep_exit(const struct device *psram)
{
	int ret;
	struct memc_mspi_is66wvo8m8_data *data = psram->data;

	LOG_DBG("Waking up is66wvo8m8 from deep sleep/%u", __LINE__);
	uint16_t cfg_reg = 0;
    ret = memc_mspi_is66wvo8m8_read_config(psram, &cfg_reg);
	if (ret != 0) {
		LOG_ERR("Could not read configuration register: %d", ret);
		return -EIO;
	}

	uint16_t target_config = sys_cpu_to_be16(cfg->target_cfg | REGISTER_CONFIG_DPD_DISABLE);

	ret = memc_mspi_is66wvo8m8_command_write(psram, CMD_WRITE_CONFIG, REGISTER_ADDRESS_CONFIG,
			(uint8_t *)&target_config, sizeof(target_config));
	if (ret != 0) {
		LOG_ERR("Could not write configuration register: %d", ret);
		return -EIO;
	}

	/** Minimum half sleep duration tHS time */
	k_busy_wait(4);
    ret = memc_mspi_is66wvo8m8_read_config(psram, &cfg_reg);
	if (ret != 0) {
		LOG_ERR("Could not read configuration register: %d", ret);
		return -EIO;
	}

	if ((cfg_reg & REGISTER_CONFIG_DPD_DISABLE) == 0) {
		LOG_ERR("Failed to wake up from deep sleep/%u", __LINE__);
		return -EIO;
	}

	return ret;
}

static int memc_mspi_is66wvo8m8_pm_action(const struct device *psram, enum pm_device_action action)
{
    // TODO: Adapt
	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		acquire(psram);
		/* memc_mspi_is66wvo8m8_deep_sleep_exit(psram); */
		release(psram);
		break;

	case PM_DEVICE_ACTION_SUSPEND:
		acquire(psram);
		/* memc_mspi_is66wvo8m8_deep_sleep_enter(psram); */
		release(psram);
		break;

	default:
		return -ENOTSUP;
	}

	return 0;
}
#endif /** IS_ENABLED(CONFIG_PM_DEVICE) */

static int memc_mspi_is66wvo8m8_init(const struct device *psram)
{
	const struct memc_mspi_is66wvo8m8_config *cfg = psram->config;
	struct memc_mspi_is66wvo8m8_data *data = psram->data;

	if (!device_is_ready(cfg->bus)) {
		LOG_ERR("Controller device not ready/%u", __LINE__);
		return -ENODEV;
	}

	switch (cfg->dev_cfg.io_mode) {
	case MSPI_IO_MODE_OCTAL:
		break;
	default:
		LOG_ERR("Bus mode %d not supported/%u", cfg->dev_cfg.io_mode, __LINE__);
		return -EIO;
	}

	if (memc_mspi_is66wvo8m8_reset(psram)) {
		LOG_ERR("Could not reset PSRAM/%u", __LINE__);
		return -EIO;
	}

	if (mspi_dev_config(cfg->bus, &cfg->dev_id, MSPI_DEVICE_CONFIG_ALL, &cfg->dev_cfg)) {
		LOG_ERR("Failed to config mspi controller/%u", __LINE__);
		return -EIO;
	}

	// Apply config but apply setup dummy cycles.
	// Will be set to the correct value when config is written.
	data->dev_cfg = cfg->dev_cfg;
	data->dev_cfg.tx_dummy = cfg->setup_tx_dummy;
	data->dev_cfg.rx_dummy = cfg->setup_rx_dummy;

	if (memc_mspi_is66wvo8m8_check_vendor_id(psram)) {
		LOG_ERR("Could not read vendor id/%u", __LINE__);
		return -EIO;
	}

	if (memc_mspi_is66wvo8m8_write_config(psram)) {
		LOG_ERR("Could not write configuration register/%u", __LINE__);
		return -EIO;
	}

	if (mspi_dev_config(cfg->bus, &cfg->dev_id, MSPI_DEVICE_CONFIG_ALL, &cfg->dev_cfg)) {
		LOG_ERR("Failed to config mspi controller/%u", __LINE__);
		return -EIO;
	}

#if CONFIG_MSPI_XIP
	if (cfg->xip_cfg.enable) {
		if (mspi_xip_config(cfg->bus, &cfg->dev_id, &cfg->xip_cfg)) {
			LOG_ERR("Failed to enable XIP/%u", __LINE__);
			return -EIO;
		}
		data->xip_cfg = cfg->xip_cfg;
	}
#endif /* CONFIG_MSPI_XIP */

#if CONFIG_MSPI_SCRAMBLE
	if (cfg->scramble_cfg.enable) {
		if (mspi_scramble_config(cfg->bus, &cfg->dev_id, &cfg->scramble_cfg)) {
			LOG_ERR("Failed to enable scrambling/%u", __LINE__);
			return -EIO;
		}
		data->scramble_cfg = cfg->scramble_cfg;
	}
#endif /* MSPI_SCRAMBLE */

	release(psram);
	return 0;
}

#define CFG_ODS(n) \
	(DT_ENUM_IDX(DT_DRV_INST(n), output_driver_strength) << REGISTER_CONFIG_ODS_SHIFT)

#define CFG_PAR(n) \
	(DT_ENUM_IDX(DT_DRV_INST(n), partial_array_refresh) << REGISTER_CONFIG_PAR_SHIFT)

#define CFG_LAT(n) \
	(DT_ENUM_IDX(DT_DRV_INST(n), latency_counter) << REGISTER_CONFIG_LATENCY_SHIFT)

#define CFG_BURST(n) \
	(DT_ENUM_IDX(DT_DRV_INST(n), wrapped_burst_sequence) << REGISTER_CONFIG_BURST_SHIFT)

#define CFG_HYBRID(n) \
	(DT_INST_PROP(n, hybrid_mode) * REGISTER_CONFIG_BURST_HYBRID)

#define CFG_INIT_LAT(n) \
	(DT_INST_PROP(n, fixed_latency) * REGISTER_CONFIG_INITLATENCY_FIXED)

#define CFG_DQSM(n) \
	(DT_INST_PROP(n, dqsm_read_pre_cycle) * REGISTER_CONFIG_DQSM_OCLK)

#define CFG_REGISTER(n) \
	(CFG_ODS(n) | CFG_PAR(n) | CFG_LAT(n) | CFG_BURST(n) | CFG_HYBRID(n) | \
	 CFG_INIT_LAT(n) | CFG_DQSM(n) | REGISTER_CONFIG_DPD_DISABLE)

#define MEMC_MSPI_IS66WVO8M8(n)                                                           \
	static const struct memc_mspi_is66wvo8m8_config                                       \
	memc_mspi_is66wvo8m8_config_##n = {                                                   \
		.mem_size           = DT_INST_PROP(n, size) / 8,                                  \
		.bus                = DEVICE_DT_GET(DT_INST_BUS(n)),                              \
		.reset              = GPIO_DT_SPEC_GET_OR(DT_DRV_INST(n), reset_gpios, {0}),      \
		.dev_id             = MSPI_DEVICE_ID_DT_INST(n),                                  \
		.dev_cfg            = MSPI_DEVICE_CONFIG_DT_INST(n),                              \
		.xip_cfg            = MSPI_XIP_CONFIG_DT_INST(n),                                 \
		.scramble_cfg       = MSPI_SCRAMBLE_CONFIG_DT_INST(n),                            \
		.sw_multi_periph    = DT_PROP(DT_INST_BUS(n), software_multiperipheral),          \
		.target_cfg         = CFG_REGISTER(n),                                            \
		.setup_tx_dummy     = DT_INST_PROP(n, setup_tx_dummy),                            \
		.setup_rx_dummy     = DT_INST_PROP(n, setup_rx_dummy),                            \
	};                                                                                    \
	static struct memc_mspi_is66wvo8m8_data                                               \
		memc_mspi_is66wvo8m8_data_##n = {                                                 \
		.lock = Z_SEM_INITIALIZER(memc_mspi_is66wvo8m8_data_##n.lock, 0, 1),              \
	};                                                                                    \
	PM_DEVICE_DT_INST_DEFINE(n, memc_mspi_is66wvo8m8_pm_action);                          \
	DEVICE_DT_INST_DEFINE(n,                                                              \
				  memc_mspi_is66wvo8m8_init,                                              \
				  PM_DEVICE_DT_INST_GET(n),                                               \
				  &memc_mspi_is66wvo8m8_data_##n,                                         \
				  &memc_mspi_is66wvo8m8_config_##n,                                       \
				  POST_KERNEL,                                                            \
				  CONFIG_MEMC_INIT_PRIORITY,                                              \
				  NULL);

DT_INST_FOREACH_STATUS_OKAY(MEMC_MSPI_IS66WVO8M8)

#undef DT_DRV_COMPAT
