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
#if !DT_NODE_HAS_PROP(MODEM_NODE, modem_rst_n_gpios)
#error "Missing /zephyr,user modem-rst-n-gpios devicetree property"
#endif

static const struct gpio_dt_spec rail_en = GPIO_DT_SPEC_GET(MODEM_NODE, modem_3v8_en_gpios);
static const struct gpio_dt_spec pwr_on_n = GPIO_DT_SPEC_GET(MODEM_NODE, modem_pwr_on_n_gpios);
static const struct gpio_dt_spec rst_n = GPIO_DT_SPEC_GET(MODEM_NODE, modem_rst_n_gpios);

/*
 * NOTE: Exact modem timing requirements need to be confirmed.
 * These defaults are based on common cellular modem patterns.
 */
static const int T_RAIL_SETTLE_MS = 10;
static const int T_PWR_ON_PULSE_MS = 250;
static const int T_PWR_OFF_PULSE_MS = 1500;
static const int T_POST_ON_DELAY_MS = 100;
static const int T_RESET_PULSE_MS = 200;

static int ensure_ready(void)
{
	if (!device_is_ready(rail_en.port) || !device_is_ready(pwr_on_n.port) || !device_is_ready(rst_n.port)) {
		return -ENODEV;
	}
	return 0;
}



static int gpio_dt_set_active(const struct gpio_dt_spec *spec, bool active)
{
	/* gpio_pin_set* APIs operate on the raw electrical level.
	 * Apply GPIO_ACTIVE_LOW here so callers can work in active/inactive (assert/deassert) terms.
	 */
	int raw = active ? 1 : 0;
	if ((spec->dt_flags & GPIO_ACTIVE_LOW) != 0) {
		raw = !raw;
	}
	return gpio_pin_set(spec->port, spec->pin, raw);
}

static int pwr_on_n_pulse(int pulse_ms)
{
	int ret;
	ret = gpio_dt_set_active(&pwr_on_n, true); /* assert (active low) */
	if (ret != 0) {
		return ret;
	}
	k_sleep(K_MSEC(pulse_ms));
	ret = gpio_dt_set_active(&pwr_on_n, false); /* deassert */
	return ret;
}

int modem_board_power_on(void)
{
	int ret = ensure_ready();
	if (ret != 0) {
		return ret;
	}

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
	int ret = ensure_ready();
	if (ret != 0) {
		return ret;
	}

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
	int ret = ensure_ready();
	if (ret != 0) {
		return ret;
	}
	ret = gpio_dt_set_active(&rst_n, true);
	if (ret != 0) {
		return ret;
	}
	k_sleep(K_MSEC(T_RESET_PULSE_MS));
	ret = gpio_dt_set_active(&rst_n, false);
	return ret;
}

int modem_board_get_status(struct modem_board_status *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	int ret = ensure_ready();
	if (ret != 0) {
		return ret;
	}

	out->rail_en = gpio_pin_get_dt(&rail_en);
	out->pwr_on_n = gpio_pin_get_dt(&pwr_on_n);
	out->rst_n = gpio_pin_get_dt(&rst_n);


	return 0;
}

