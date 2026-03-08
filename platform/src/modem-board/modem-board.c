#include "modem-board.h"

#include "modem-board-core.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
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
#if !DT_NODE_HAS_PROP(MODEM_NODE, io_channels)
#error "Missing /zephyr,user io-channels devicetree property"
#endif

static const struct gpio_dt_spec rail_en = GPIO_DT_SPEC_GET(MODEM_NODE, modem_3v8_en_gpios);
static const struct gpio_dt_spec pwr_on_n = GPIO_DT_SPEC_GET(MODEM_NODE, modem_pwr_on_n_gpios);
static const struct gpio_dt_spec rst_n = GPIO_DT_SPEC_GET(MODEM_NODE, modem_rst_n_gpios);
static const struct adc_dt_spec vgpio = ADC_DT_SPEC_GET_BY_NAME(MODEM_NODE, vgpio);

static int ensure_ready(void *ctx)
{
	ARG_UNUSED(ctx);
	if (!device_is_ready(rail_en.port) || !device_is_ready(pwr_on_n.port) || !device_is_ready(rst_n.port)
		|| !adc_is_ready_dt(&vgpio)) {
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

static int set_rail_en(void *ctx, int value)
{
	ARG_UNUSED(ctx);
	return gpio_pin_set_dt(&rail_en, value);
}

static int set_pwr_on_asserted(void *ctx, bool asserted)
{
	ARG_UNUSED(ctx);
	return gpio_dt_set_active(&pwr_on_n, asserted);
}

static int set_rst_asserted(void *ctx, bool asserted)
{
	ARG_UNUSED(ctx);
	return gpio_dt_set_active(&rst_n, asserted);
}

static int get_rail_en(void *ctx)
{
	ARG_UNUSED(ctx);
	return gpio_pin_get_dt(&rail_en);
}

static int get_pwr_on_n(void *ctx)
{
	ARG_UNUSED(ctx);
	return gpio_pin_get_dt(&pwr_on_n);
}

static int get_rst_n(void *ctx)
{
	ARG_UNUSED(ctx);
	return gpio_pin_get_dt(&rst_n);
}

static int get_vgpio_mv(void *ctx)
{
	ARG_UNUSED(ctx);

	static bool vgpioConfigured;
	int ret;
	int16_t sampleBuffer;
	struct adc_sequence sequence = {
		.buffer = &sampleBuffer,
		.buffer_size = sizeof(sampleBuffer),
	};
	int32_t mv;

	if (!vgpioConfigured) {
		ret = adc_channel_setup_dt(&vgpio);
		if (ret != 0) {
			return ret;
		}
		vgpioConfigured = true;
	}

	ret = adc_sequence_init_dt(&vgpio, &sequence);
	if (ret != 0) {
		return ret;
	}

	ret = adc_read_dt(&vgpio, &sequence);
	if (ret != 0) {
		return ret;
	}

	mv = ((int32_t)sampleBuffer * 3300) / BIT(12);
	return (int)mv;
}

static void sleep_ms(void *ctx, int duration_ms)
{
	ARG_UNUSED(ctx);
	k_sleep(K_MSEC(duration_ms));
}

static const struct modem_board_ops boardOps = {
	.ensure_ready = ensure_ready,
	.set_rail_en = set_rail_en,
	.set_pwr_on_asserted = set_pwr_on_asserted,
	.set_rst_asserted = set_rst_asserted,
	.get_rail_en = get_rail_en,
	.get_pwr_on_n = get_pwr_on_n,
	.get_rst_n = get_rst_n,
	.get_vgpio_mv = get_vgpio_mv,
	.sleep_ms = sleep_ms,
	.ctx = NULL,
};

int modem_board_power_on(void)
{
	return modem_board_power_on_core(&boardOps);
}

int modem_board_power_off(void)
{
	return modem_board_power_off_core(&boardOps);
}

int modem_board_power_cycle(void)
{
	return modem_board_power_cycle_core(&boardOps);
}

int modem_board_reset_pulse(void)
{
	return modem_board_reset_pulse_core(&boardOps);
}

int modem_board_get_status(struct modem_board_status *out)
{
	return modem_board_get_status_core(&boardOps, out);
}

