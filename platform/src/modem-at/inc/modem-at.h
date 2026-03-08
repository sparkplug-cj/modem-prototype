#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int modem_at_send(const char *command, char *response, size_t responseSize);

#ifdef __cplusplus
}
#endif
