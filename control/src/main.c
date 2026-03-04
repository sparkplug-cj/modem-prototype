#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/usb/usb_dc.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(control_app, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF("control app boot");

    while (1) {
        k_sleep(K_SECONDS(1));
        LOG_DBG("tick");
    }

    return 0;
}
