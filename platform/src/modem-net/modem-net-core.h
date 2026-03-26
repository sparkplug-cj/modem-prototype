#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct modem_net_status {
	bool modemPowered;
	bool sessionOpen;
	bool connected;
	bool dnsReady;
	int uartOwner;
	int lastError;
	const char *lastErrorText;
	const char *apn;
	const char *ipv4;
};


struct modem_net_profile {
    const char *apn;
    const char *id;
    const char *password;
};

struct modem_net_ops {
	int (*owner_get)(void);
	int (*ensure_powered)(void *ctx);
	int (*configure_context)(void *ctx, const struct modem_net_profile *prof);
	int (*open_uart_session)(void);
	int (*dial_ppp)(void *ctx);
	int (*attach_ppp)(void);
	int (*wait_for_network)(void *ctx);
	void (*close_uart_session)(void);
	int (*escape_and_hangup)(void);
	int (*get_status)(struct modem_net_status *out);
	void (*set_apn)(const char *apn);
	void (*set_error)(int error, const char *message);
	void (*clear_error)(void);
	void (*print)(void *ctx, const char *fmt, ...);
	void (*error)(void *ctx, const char *fmt, ...);
	void *ctx;
};

int modem_net_cmd_connect_core(const struct modem_net_ops *ops, size_t argc, char **argv);
int modem_net_cmd_disconnect_core(const struct modem_net_ops *ops, size_t argc, char **argv);
int modem_net_cmd_status_core(const struct modem_net_ops *ops, size_t argc, char **argv);

#ifdef __cplusplus
}
#endif
