#ifndef MODEM_RC7620_H_
#define MODEM_RC7620_H_

#include <zephyr/device.h>
#include <zephyr/net/net_if.h>

#ifdef __cplusplus
extern "C" {
#endif

struct net_if *modem_rc7620_ppp_iface_get(const struct device *dev);
int modem_rc7620_set_profile(const struct device *dev, const char *apn,
			     const char *id, const char *password);

#ifdef __cplusplus
}
#endif

#endif
