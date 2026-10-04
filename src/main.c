#include "tag/nfcv_emulation.h"
#include "platform/platform.h"
#include "core/app.h"
#include "net/network.h"
#include "tusb.h"

int main(void) {
    platform_init();
    app_init(platform_flash_io());
    network_init();
    for (;;) {
        platform_poll();
        nfcv_emulation_poll();
        tud_task();
        network_poll();
        platform_idle(app_state.emulating);
    }
}
