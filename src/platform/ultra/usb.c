#include "nrf.h"
#include "tusb.h"
#include <stdio.h>
#include <string.h>
extern void tusb_hal_nrf_power_event(uint32_t);
void USBD_IRQHandler(void) {
    tud_int_handler(0);
}

uint8_t tud_network_mac_address[6] = {0x02, 0x43, 0x48, 0x46, 0x55, 0x4e};
static const tusb_desc_device_t device = {.bLength = sizeof(tusb_desc_device_t),
                                          .bDescriptorType = TUSB_DESC_DEVICE,
                                          .bcdUSB = 0x200,
                                          .bDeviceClass = TUSB_CLASS_MISC,
                                          .bDeviceSubClass = MISC_SUBCLASS_COMMON,
                                          .bDeviceProtocol = MISC_PROTOCOL_IAD,
                                          .bMaxPacketSize0 = 64,
                                          .idVendor = 0xcafe,
                                          .idProduct = 0x4060,
                                          .bcdDevice = 0x0100,
                                          .iManufacturer = 1,
                                          .iProduct = 2,
                                          .iSerialNumber = 3,
                                          .bNumConfigurations = 1};
uint8_t const *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&device;
}

static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_ECM_DESC_LEN, 0, 100),
    TUD_CDC_ECM_DESCRIPTOR(0, 4, 5, 0x81, 64, 0x02, 0x82, 64, 1514)};
uint8_t const *tud_descriptor_configuration_cb(uint8_t i) {
    return i ? NULL : configuration;
}

uint16_t const *tud_descriptor_string_cb(uint8_t i, uint16_t lang) {
    (void)lang;
    static uint16_t text[48];
    char serial[24], mac[13];
    snprintf(serial, sizeof(serial), "%08lX%08lX", (unsigned long)NRF_FICR->DEVICEID[0],
             (unsigned long)NRF_FICR->DEVICEID[1]);
    for (unsigned j = 0; j < 6; j++)
        snprintf(mac + j * 2, 3, "%02X", tud_network_mac_address[j]);
    const char *strings[] = {
        "", "ChameleonUltra-Tonie", "ChameleonUltra-Tonie", serial, "Ultra USB", mac};
    if (i == 0) {
        text[0] = (TUSB_DESC_STRING << 8) | 4;
        text[1] = 0x409;
        return text;
    }
    if (i >= sizeof(strings) / sizeof(*strings))
        return NULL;
    unsigned n = strlen(strings[i]);
    if (n > 47)
        n = 47;
    text[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    for (unsigned j = 0; j < n; j++)
        text[j + 1] = (uint8_t)strings[i][j];
    return text;
}

void POWER_CLOCK_IRQHandler(void) {
    if (NRF_POWER->EVENTS_USBDETECTED) {
        NRF_POWER->EVENTS_USBDETECTED = 0;
        tusb_hal_nrf_power_event(0);
    }
    if (NRF_POWER->EVENTS_USBREMOVED) {
        NRF_POWER->EVENTS_USBREMOVED = 0;
        tusb_hal_nrf_power_event(1);
    }
    if (NRF_POWER->EVENTS_USBPWRRDY) {
        NRF_POWER->EVENTS_USBPWRRDY = 0;
        tusb_hal_nrf_power_event(2);
    }
}

void platform_usb_init(void) {
    tud_init(0);
    NRF_POWER->INTENSET = POWER_INTENSET_USBDETECTED_Msk | POWER_INTENSET_USBREMOVED_Msk |
                          POWER_INTENSET_USBPWRRDY_Msk;
    NVIC_SetPriority(POWER_CLOCK_IRQn, 5);
    NVIC_EnableIRQ(POWER_CLOCK_IRQn);
    if (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) {
        tusb_hal_nrf_power_event(0);
        if (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_OUTPUTRDY_Msk)
            tusb_hal_nrf_power_event(2);
    }
}

/* No RNDIS interface is exposed; satisfy TinyUSB's optional dispatch hook. */
void rndis_class_set_handler(uint8_t *data, int size) {
    (void)data;
    (void)size;
}
