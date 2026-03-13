#include "modem-board-core.h"

#include <errno.h>
#include <stddef.h>

static const int T_RAIL_SETTLE_MS = 10;
static const int T_PWR_ON_PULSE_MS = 250;
static const int T_PWR_OFF_PULSE_MS = 1500;
static const int T_POWER_CYCLE_DELAY_MS = 500;
static const int T_POST_ON_DELAY_MS = 100;
static const int T_RESET_PULSE_MS = 200;
static const int MODEM_VGPIO_ON_THRESHOLD_MV = 900;

static int pwr_on_pulse(const struct modem_board_ops *ops, int pulse_ms)
{
	int ret = ops->set_pwr_on_asserted(ops->ctx, true);
	if (ret != 0) {
		return ret;
	}

	ops->sleep_ms(ops->ctx, pulse_ms);
	return ops->set_pwr_on_asserted(ops->ctx, false);
}

int modem_board_power_on_core(const struct modem_board_ops *ops)
{
	int ret = ops->ensure_ready(ops->ctx);
	if (ret != 0) {
		return ret;
	}

	ret = ops->set_rail_en(ops->ctx, 1);
	if (ret != 0) {
		return ret;
	}
	ops->sleep_ms(ops->ctx, T_RAIL_SETTLE_MS);

	ret = pwr_on_pulse(ops, T_PWR_ON_PULSE_MS);
	if (ret != 0) {
		return ret;
	}

	ops->sleep_ms(ops->ctx, T_POST_ON_DELAY_MS);
	return 0;
}

int modem_board_power_off_core(const struct modem_board_ops *ops)
{
	int ret = ops->ensure_ready(ops->ctx);
	if (ret != 0) {
		return ret;
	}

	ret = pwr_on_pulse(ops, T_PWR_OFF_PULSE_MS);
	if (ret != 0) {
		return ret;
	}

	return ops->set_rail_en(ops->ctx, 0);
}

int modem_board_power_cycle_core(const struct modem_board_ops *ops)
{
	int ret = modem_board_power_off_core(ops);
	if (ret != 0) {
		return ret;
	}

	ops->sleep_ms(ops->ctx, T_POWER_CYCLE_DELAY_MS);
	return modem_board_power_on_core(ops);
}

int modem_board_reset_pulse_core(const struct modem_board_ops *ops)
{
	int ret = ops->ensure_ready(ops->ctx);
	if (ret != 0) {
		return ret;
	}

	ret = ops->set_rst_asserted(ops->ctx, true);
	if (ret != 0) {
		return ret;
	}

	ops->sleep_ms(ops->ctx, T_RESET_PULSE_MS);
	return ops->set_rst_asserted(ops->ctx, false);
}

int modem_board_get_status_core(const struct modem_board_ops *ops, struct modem_board_status *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	int ret = ops->ensure_ready(ops->ctx);
	if (ret != 0) {
		return ret;
	}

	out->rail_en = ops->get_rail_en(ops->ctx);
	out->pwr_on = ops->get_pwr_on(ops->ctx);
	out->rst = ops->get_rst(ops->ctx);
	out->vgpio_mv = ops->get_vgpio_mv(ops->ctx);
	out->modem_state_on = (out->vgpio_mv >= MODEM_VGPIO_ON_THRESHOLD_MV);
	return 0;
}
