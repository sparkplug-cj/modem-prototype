#include "modem_board.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(modem_board, LOG_LEVEL_INF);

#define MODEM_NODE DT_PATH(zephyr_user)

#if !DT_NODE_HAS_PROP(MODEM_NODE, modem_3v8_en_gpios)
#error "Missing /zephyr,user modem-3v8-en-gpios devicetree property"
#endif
#if !DT_NODE_HAS_PROP(MODEM_NODE, modem_pwr_on_n_gpios)
#error "Missing /zephyr,user modem-pwr-on-n-gpios devicetree property"
#endif
#if !DT_NODE_HAS_PROP(MODEM_NODE, modem_nrst_n_gpios)
#error "Missing /zephyr,user modem-nrst-n-gpios devicetree property"
#endif

static const struct gpio_dt_spec rail_en = GPIO_DT_SPEC_GET(MODEM_NODE, modem_3v8_en_gpios);
static const struct gpio_dt_spec pwr_on_n = GPIO_DT_SPEC_GET(MODEM_NODE, modem_pwr_on_n_gpios);
static const struct gpio_dt_spec nrst_n = GPIO_DT_SPEC_GET(MODEM_NODE, modem_nrst_n_gpios);

/*
 * NOTE: Exact modem timing requirements need to be confirmed.
 * These defaults are based on common cellular modem patterns.
 */
static const k_timeout_t T_RAIL_SETTLE = K_MSEC(10);
static const k_timeout_t T_PWR_ON_PULSE = K_MSEC(250);
static const k_timeout_t T_PWR_OFF_PULSE = K_MSEC(1500);
static const k_timeout_t T_POST_ON_DELAY = K_MSEC(100);
static const k_timeout_t T_RESET_PULSE = K_MSEC(200);

int modem_board_init(void)
{
	if (!device_is_ready(rail_en.port)) {
		LOG_ERR("rail_en gpio port not ready");
		return -ENODEV;
	}
	if (!device_is_ready(pwr_on_n.port)) {
		LOG_ERR("pwr_on_n gpio port not ready");
		return -ENODEV;
	}
	if (!device_is_ready(nrst_n.port)) {
		LOG_ERR("nrst_n gpio port not ready");
		return -ENODEV;
	}

	/* Default safe state: rail off, PWR_ON_N deasserted, reset asserted (if already configured). */
	int ret;

	ret = gpio_pin_configure_dt(&rail_en, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("Failed to configure rail_en: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&pwr_on_n, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("Failed to configure pwr_on_n: %d", ret);
		return ret;
	}

	/* nrst_n may be configured by a gpio-hog on this board; just drive it to asserted state. */
	(void)gpio_pin_set_dt(&nrst_n, 1);

	return 0;
}

static int pwr_on_n_pulse(k_timeout_t pulse)
{
	int ret;
	ret = gpio_pin_set_dt(&pwr_on_n, 1); /* assert (active low) */
	if (ret != 0) {
		return ret;
	}
	k_sleep(pulse);
	ret = gpio_pin_set_dt(&pwr_on_n, 0); /* deassert */
	return ret;
}

int modem_board_power_on(void)
{
	int ret;

	/* Assert reset while sequencing power. */
	(void)gpio_pin_set_dt(&nrst_n, 1);

	ret = gpio_pin_set_dt(&rail_en, 1);
	if (ret != 0) {
		return ret;
	}
	k_sleep(T_RAIL_SETTLE);

	ret = pwr_on_n_pulse(T_PWR_ON_PULSE);
	if (ret != 0) {
		return ret;
	}

	k_sleep(T_POST_ON_DELAY);
	/* Release reset */
	ret = gpio_pin_set_dt(&nrst_n, 0);
	return ret;
}

int modem_board_power_off(void)
{
	int ret;

	/* Optionally request modem shutdown via long PWR_ON_N pulse. */
	ret = pwr_on_n_pulse(T_PWR_OFF_PULSE);
	if (ret != 0) {
		return ret;
	}

	/* Assert reset, then remove rail. */
	(void)gpio_pin_set_dt(&nrst_n, 1);
	k_sleep(K_MSEC(20));
	ret = gpio_pin_set_dt(&rail_en, 0);
	return ret;
}

int modem_board_power_cycle(void)
{
	int ret;
	ret = modem_board_power_off();
	if (ret != 0) {
		return ret;
	}
	k_sleep(K_MSEC(500));
	return modem_board_power_on();
}

int modem_board_reset_pulse(void)
{
	int ret;
	ret = gpio_pin_set_dt(&nrst_n, 1);
	if (ret != 0) {
		return ret;
	}
	k_sleep(T_RESET_PULSE);
	ret = gpio_pin_set_dt(&nrst_n, 0);
	return ret;
}

void modem_board_status_print(void)
{
	int rail = gpio_pin_get_dt(&rail_en);
	int pwr = gpio_pin_get_dt(&pwr_on_n);
	int rst = gpio_pin_get_dt(&nrst_n);

	LOG_INF("MODEM_3V8_EN=%d, MODEM_PWR_ON_N=%d, MODEM_nRST=%d", rail, pwr, rst);
}

static int modem_board_sys_init(void)
{
	int ret = modem_board_init();
	if (ret != 0) {
		LOG_ERR("modem_board_init failed: %d", ret);
	}
	return ret;
}

SYS_INIT(modem_board_sys_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
