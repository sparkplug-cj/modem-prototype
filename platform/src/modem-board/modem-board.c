#include <modem-board.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

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
static const struct gpio_dt_spec rst_n = GPIO_DT_SPEC_GET(MODEM_NODE, modem_nrst_n_gpios);

/*
 * NOTE: Exact modem timing requirements need to be confirmed.
 * These defaults are based on common cellular modem patterns.
 */
static const int T_RAIL_SETTLE_MS = 10;
static const int T_PWR_ON_PULSE_MS = 250;
static const int T_PWR_OFF_PULSE_MS = 1500;
static const int T_POST_ON_DELAY_MS = 100;
static const int T_RESET_PULSE_MS = 200;

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
	if (!device_is_ready(rst_n.port)) {
		LOG_ERR("rst_n gpio port not ready");
		return -ENODEV;
	}

	/* Default safe state: rail off, PWR_ON_N deasserted, reset deasserted. */
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

	/* Configure reset as output and start deasserted (active-low). */
	ret = gpio_pin_configure_dt(&rst_n, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("Failed to configure rst_n: %d", ret);
		return ret;
	}

	return 0;
}

static int pwr_on_n_pulse(int pulse_ms)
{
	int ret;
	ret = gpio_pin_set_dt(&pwr_on_n, 1); /* assert (active low) */
	if (ret != 0) {
		return ret;
	}
	k_sleep(K_MSEC(pulse_ms));
	ret = gpio_pin_set_dt(&pwr_on_n, 0); /* deassert */
	return ret;
}

int modem_board_power_on(void)
{
	int ret;

	/* Avoid using RESET_IN_N for normal bring-up; prefer power-cycle via rail + PWR_ON_N. */

	ret = gpio_pin_set_dt(&rail_en, 1);
	if (ret != 0) {
		return ret;
	}
	k_sleep(K_MSEC(T_RAIL_SETTLE_MS));

	ret = pwr_on_n_pulse(T_PWR_ON_PULSE_MS);
	if (ret != 0) {
		return ret;
	}

	k_sleep(K_MSEC(T_POST_ON_DELAY_MS));
	return 0;
}

int modem_board_power_off(void)
{
	int ret;

	/* Optionally request modem shutdown via long PWR_ON_N pulse. */
	ret = pwr_on_n_pulse(T_PWR_OFF_PULSE_MS);
	if (ret != 0) {
		return ret;
	}

	/* Remove rail. */
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
	ret = gpio_pin_set_dt(&rst_n, 1);
	if (ret != 0) {
		return ret;
	}
	k_sleep(K_MSEC(T_RESET_PULSE_MS));
	ret = gpio_pin_set_dt(&rst_n, 0);
	return ret;
}

int modem_board_get_status(struct modem_board_status *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	out->rail_en = gpio_pin_get_dt(&rail_en);
	out->pwr_on_n = gpio_pin_get_dt(&pwr_on_n);
	out->rst_n = gpio_pin_get_dt(&rst_n);
	return 0;
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
