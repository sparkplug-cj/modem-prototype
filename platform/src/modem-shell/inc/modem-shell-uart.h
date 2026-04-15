#ifndef MODEM_SHELL_UART_H_
#define MODEM_SHELL_UART_H_

#ifdef __cplusplus
extern "C" {
#endif

enum modem_uart_owner {
	MODEM_UART_OWNER_NONE = 0,
	MODEM_UART_OWNER_AT,
	MODEM_UART_OWNER_PPP,
};

int modem_uart_owner_acquire(enum modem_uart_owner owner);
void modem_uart_owner_release(enum modem_uart_owner owner);
enum modem_uart_owner modem_uart_owner_get(void);

#ifdef __cplusplus
}
#endif

#endif
