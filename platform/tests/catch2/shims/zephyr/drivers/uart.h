#pragma once

#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

int uart_poll_in(const struct device *dev, unsigned char *ch);
void uart_poll_out(const struct device *dev, unsigned char ch);

#ifdef __cplusplus
}
#endif
