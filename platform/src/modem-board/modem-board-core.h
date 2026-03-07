#pragma once

#include <stdbool.h>

#include <modem-board.h>

#ifdef __cplusplus
extern "C" {
#endif

struct modem_board_ops {
	int (*ensure_ready)(void *ctx);
	int (*set_rail_en)(void *ctx, int value);
	int (*set_pwr_on_asserted)(void *ctx, bool asserted);
	int (*set_rst_asserted)(void *ctx, bool asserted);
	int (*get_rail_en)(void *ctx);
	int (*get_pwr_on_n)(void *ctx);
	int (*get_rst_n)(void *ctx);
	int (*get_vgpio_mv)(void *ctx);
	void (*sleep_ms)(void *ctx, int duration_ms);
	void *ctx;
};

int modem_board_power_on_core(const struct modem_board_ops *ops);
int modem_board_power_off_core(const struct modem_board_ops *ops);
int modem_board_power_cycle_core(const struct modem_board_ops *ops);
int modem_board_reset_pulse_core(const struct modem_board_ops *ops);
int modem_board_get_status_core(const struct modem_board_ops *ops, struct modem_board_status *out);

#ifdef __cplusplus
}
#endif
