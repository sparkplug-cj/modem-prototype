#pragma once

#include <stddef.h>

struct shell;

#ifdef __cplusplus
extern "C" {
#endif

int cmd_modem_ppp_connect(const struct shell *sh, size_t argc, char **argv);
int cmd_modem_ppp_disconnect(const struct shell *sh, size_t argc, char **argv);
int cmd_modem_ppp_status(const struct shell *sh, size_t argc, char **argv);

#ifdef __cplusplus
}
#endif
