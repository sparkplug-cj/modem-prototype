#pragma once

#include <errno.h>
#include <stdbool.h>

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
 * @brief Snapshot of modem-board control line states.
 */
struct modem_board_status {
	/* Logical values (honor GPIO_ACTIVE_LOW) */
	int rail_en;   /**< MODEM_3V8_EN logical value (0/1), or negative errno */
	int pwr_on_n;  /**< MODEM_PWR_ON_N logical value (0/1), or negative errno */
	int rst_n;     /**< MODEM_RST_N logical value (0/1), or negative errno */
	int vgpio_mv;  /**< VGPIO sense voltage in mV, or negative errno if ADC read failed */
	bool modem_state_on; /**< Derived boolean modem state based on VGPIO threshold */
};

/**
 * @brief Read current control line states.
 *
 * This is a pure data function; formatting/output is the caller's responsibility.
 *
 * @param[out] out Filled with instantaneous GPIO values.
 *
 * @retval 0 Success.
 * @retval -EINVAL out is NULL.
 */
int modem_board_get_status(struct modem_board_status *out);

#ifdef __cplusplus
}
#endif
