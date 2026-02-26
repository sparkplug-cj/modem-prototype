#pragma once

#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file modem-board.h
 * @brief Board-specific modem power/reset control primitives.
 *
 * This unit provides **board-level** control over the modem’s rail enable and
 * control pins (e.g. PWR_ON_N and RESET_IN_N). It is intentionally **modem-agnostic**:
 * the sequencing is generic, while the actual GPIO mapping is supplied via devicetree.
 *
 * @note This API does not establish any UART/AT/PPP connectivity. It only manages
 *       the electrical power/reset lines and provides basic status reporting.
 */

/**
 * @brief Initialize modem-board GPIOs and set a safe default state.
 *
 * Preconditions:
 * - Required `/zephyr,user` GPIO properties must exist in the devicetree overlay.
 *
 * Postconditions:
 * - Rail enable and PWR_ON_N GPIOs are configured as outputs (inactive).
 * - Reset line is asserted if available/configured.
 *
 * @retval 0 Success.
 * @retval -ENODEV GPIO controller device not ready.
 * @retval -EIO GPIO configuration failed.
 */
int modem_board_init(void);

/**
 * @brief Power on the modem.
 *
 * High-level behavior:
 * - Assert reset.
 * - Enable rail.
 * - Pulse PWR_ON_N (active-low) for the configured duration.
 * - Wait a short post-on delay.
 * - Deassert reset.
 *
 * @retval 0 Success.
 * @retval -ENODEV GPIO controller device not ready.
 * @retval -EIO GPIO operation failed.
 */
int modem_board_power_on(void);

/**
 * @brief Power off the modem.
 *
 * High-level behavior:
 * - Pulse PWR_ON_N (active-low) for the configured “power-off” duration.
 * - Assert reset.
 * - Disable rail.
 *
 * @retval 0 Success.
 * @retval -ENODEV GPIO controller device not ready.
 * @retval -EIO GPIO operation failed.
 */
int modem_board_power_off(void);

/**
 * @brief Power-cycle the modem.
 *
 * Equivalent to calling modem_board_power_off(), waiting a short delay, then
 * calling modem_board_power_on().
 *
 * @retval 0 Success.
 * @retval -ENODEV GPIO controller device not ready.
 * @retval -EIO GPIO operation failed.
 */
int modem_board_power_cycle(void);

/**
 * @brief Pulse the modem reset line.
 *
 * @retval 0 Success.
 * @retval -ENODEV GPIO controller device not ready.
 * @retval -EIO GPIO operation failed.
 */
int modem_board_reset_pulse(void);

/**
 * @brief Print current control line states.
 *
 * Prints the instantaneous values of the rail enable, PWR_ON_N, and reset GPIOs.
 * Intended for interactive diagnostics via shell/RTT.
 */
void modem_board_status_print(void);

#ifdef __cplusplus
}
#endif
