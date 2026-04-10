#pragma once

#include <zephyr/net/net_if.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODEM_NET_PPP_APN_MAX_LEN 64
#define MODEM_NET_PPP_ID_MAX_LEN 64
#define MODEM_NET_PPP_PASSWORD_MAX_LEN 64

enum modem_net_ppp_opt {
  MODEM_NET_PPP_OPT_APN = 1,
  MODEM_NET_PPP_OPT_ID,
  MODEM_NET_PPP_OPT_PASSWORD,
  MODEM_NET_PPP_OPT_PROFILE,
};

struct modem_net_ppp_profile {
  char apn[MODEM_NET_PPP_APN_MAX_LEN];
  char id[MODEM_NET_PPP_ID_MAX_LEN];
  char password[MODEM_NET_PPP_PASSWORD_MAX_LEN];
};

struct net_if *modem_net_ppp_iface_get(void);

#ifdef __cplusplus
}
#endif