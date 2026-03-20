#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct device {
  int unused;
};

extern const struct device g_modem_at_test_device;

bool device_is_ready(const struct device *dev);

#define DT_NODELABEL(name) 0
#define DEVICE_DT_GET(node_id) (&g_modem_at_test_device)

#ifdef __cplusplus
}
#endif
