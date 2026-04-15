#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(control_app, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF("FW_MARKER: phase3-post-help-verify-20260409");
    LOG_INF("FW_MARKER: expect shell command 'modem post'");

    while (1) {
        k_sleep(K_FOREVER);
    }
}
