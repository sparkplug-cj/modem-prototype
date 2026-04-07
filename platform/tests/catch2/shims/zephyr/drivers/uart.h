#pragma once

#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

int uart_poll_in(const struct device *dev, unsigned char *ch);
void uart_poll_out(const struct device *dev, unsigned char ch);

typedef void (*uart_irq_callback_user_data_t)(const struct device *dev, void *user_data);

void uart_irq_update(const struct device *dev);
int uart_irq_rx_ready(const struct device *dev);
int uart_fifo_read(const struct device *dev, uint8_t *buf, const int size);
void uart_irq_callback_user_data_set(const struct device *dev,
                                     uart_irq_callback_user_data_t callback,
                                     void *user_data);
void uart_irq_rx_enable(const struct device *dev);
void uart_irq_rx_disable(const struct device *dev);

#ifdef __cplusplus
}
#endif
