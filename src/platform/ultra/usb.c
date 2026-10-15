#include "tusb.h"
#ifndef PLATFORM_HOST
#include "nrf.h"
#endif
#include <stdio.h>
#include <string.h>
#ifndef PLATFORM_HOST
extern void tusb_hal_nrf_power_event(uint32_t);
void USBD_IRQHandler(void) {
    tud_int_handler(0);
}
#endif

uint8_t tud_network_mac_address[6] = {0x02, 0x43, 0x48, 0x46, 0x55, 0x4e};
enum {
    STRING_MANUFACTURER = 1,
    STRING_PRODUCT,
    STRING_SERIAL,
    STRING_NETWORK_INTERFACE,
    STRING_HOST_MAC,
    STRING_MS_OS = 0xee
};
enum { CONFIGURATION_RNDIS = 1, CONFIGURATION_ECM, CONFIGURATION_COUNT = 2 };
enum { ENDPOINT_NETWORK_NOTIFY = 0x81, ENDPOINT_NETWORK_OUT = 0x02, ENDPOINT_NETWORK_IN = 0x82 };
static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64,
    .idVendor = 0xcafe,
    /* New USB layout: avoid cached ECM-only driver bindings. */
    .idProduct = 0x4061,
    .bcdDevice = 0x0100,
    .iManufacturer = STRING_MANUFACTURER,
    .iProduct = STRING_PRODUCT,
    .iSerialNumber = STRING_SERIAL,
    .bNumConfigurations = CONFIGURATION_COUNT};
uint8_t const *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&device;
}

/* Windows uses the first configuration; ECM hosts can select the second. */
static const uint8_t rndis_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(CONFIGURATION_RNDIS, 2, 0, TUD_CONFIG_DESC_LEN + TUD_RNDIS_DESC_LEN, 0,
                          100),
    TUD_RNDIS_DESCRIPTOR(0, STRING_NETWORK_INTERFACE, ENDPOINT_NETWORK_NOTIFY, 8,
                         ENDPOINT_NETWORK_OUT, ENDPOINT_NETWORK_IN, CFG_TUD_NET_ENDPOINT_SIZE)};
static const uint8_t ecm_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(CONFIGURATION_ECM, 2, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_ECM_DESC_LEN, 0,
                          100),
    TUD_CDC_ECM_DESCRIPTOR(0, STRING_NETWORK_INTERFACE, STRING_HOST_MAC, ENDPOINT_NETWORK_NOTIFY,
                           64, ENDPOINT_NETWORK_OUT, ENDPOINT_NETWORK_IN, CFG_TUD_NET_ENDPOINT_SIZE,
                           CFG_TUD_NET_MTU)};
uint8_t const *tud_descriptor_configuration_cb(uint8_t i) {
    static const uint8_t *const configurations[] = {rndis_configuration, ecm_configuration};
    return i < CONFIGURATION_COUNT ? configurations[i] : NULL;
}

enum { MS_OS_VENDOR_CODE = 0x20, MS_OS_COMPATIBLE_ID_INDEX = 4 };
/* Microsoft OS 1.0 descriptors associate the built-in RNDIS driver without an INF. */
/* clang-format off */
static const uint16_t ms_os_string[] = {
    (TUSB_DESC_STRING << 8) | 18, 'M', 'S', 'F', 'T', '1', '0', '0', MS_OS_VENDOR_CODE};
static const uint8_t ms_compatible_id[] = {
    U32_TO_U8S_LE(40),                      /* Total descriptor length. */
    U16_TO_U8S_LE(0x0100),                  /* Microsoft OS 1.0. */
    U16_TO_U8S_LE(MS_OS_COMPATIBLE_ID_INDEX),
    1, 0, 0, 0, 0, 0, 0, 0,                /* One function; reserved bytes. */
    0, 1,                                  /* Control interface 0; reserved byte. */
    'R', 'N', 'D', 'I', 'S', 0, 0, 0,       /* Compatible ID. */
    '5', '1', '6', '2', '0', '0', '1', 0,   /* RNDIS 6 subcompatible ID. */
    0, 0, 0, 0, 0, 0};                     /* Reserved bytes. */
/* clang-format on */
_Static_assert(sizeof(ms_compatible_id) == 40, "Microsoft compatible ID descriptor size");

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                const tusb_control_request_t *request) {
    if (request->bmRequestType != 0xc0 || request->bRequest != MS_OS_VENDOR_CODE ||
        request->wIndex != MS_OS_COMPATIBLE_ID_INDEX || request->wValue != 0)
        return false;
    return stage != CONTROL_STAGE_SETUP ||
           tud_control_xfer(rhport, request, (void *)ms_compatible_id, sizeof(ms_compatible_id));
}

uint16_t const *tud_descriptor_string_cb(uint8_t i, uint16_t lang) {
    (void)lang;
    static uint16_t text[48];
    if (i == STRING_MS_OS)
        return ms_os_string;
    char serial[24], mac[13];
#ifdef PLATFORM_HOST
    snprintf(serial, sizeof(serial), "%s", "0123456789ABCDEF");
#else
    snprintf(serial, sizeof(serial), "%08lX%08lX", (unsigned long)NRF_FICR->DEVICEID[0],
             (unsigned long)NRF_FICR->DEVICEID[1]);
#endif
    for (unsigned j = 0; j < 6; j++)
        snprintf(mac + j * 2, 3, "%02X", tud_network_mac_address[j]);
    const char *strings[] = {[0] = "",
                             [STRING_MANUFACTURER] = "ChameleonUltra-Tonie",
                             [STRING_PRODUCT] = "ChameleonUltra-Tonie",
                             [STRING_SERIAL] = serial,
                             [STRING_NETWORK_INTERFACE] = "Ultra USB",
                             [STRING_HOST_MAC] = mac};
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

#ifndef PLATFORM_HOST
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

#endif
