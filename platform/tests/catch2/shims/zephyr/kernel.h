#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t k_uptime_get(void);
void k_msleep(int32_t ms);

#ifdef __cplusplus
}
#endif
