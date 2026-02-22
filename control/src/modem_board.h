#pragma once

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Board-specific modem control primitives.
 *
 * These functions are modem-agnostic (power rail, PWR_ON_N, reset), but
 * the underlying GPIO mapping is board-specific and provided via devicetree.
 */

int modem_board_init(void);

int modem_board_power_on(void);
int modem_board_power_off(void);
int modem_board_power_cycle(void);

int modem_board_reset_pulse(void);

void modem_board_status_print(void);

#ifdef __cplusplus
}
#endif
