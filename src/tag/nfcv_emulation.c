#include "tag/nfcv_emulation.h"
#include "tag/phy/nfcv/phy.h"
#include "tag/nfcv_session.h"

void nfcv_emulation_stop(void) {
    nfcv_phy_stop();
}

bool nfcv_emulation_start_from_image(const tag_image_t *image) {
    nfcv_emulation_stop();
    return nfcv_session_configure_from_image(image) && nfcv_phy_start();
}

bool nfcv_emulation_field_present(void) {
    return nfcv_phy_field_present();
}

void nfcv_emulation_poll(void) {
    nfcv_phy_poll();
}
