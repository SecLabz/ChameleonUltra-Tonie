#include "tusb.h"
#include "device/dcd.h"
#include <stdatomic.h>
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
    STRING_HOST_MAC
};
enum { USB_PROFILE_ECM, USB_PROFILE_PENDING, USB_PROFILE_RNDIS };
static atomic_int usb_profile;
enum { ENDPOINT_NETWORK_NOTIFY = 0x81, ENDPOINT_NETWORK_OUT = 0x02, ENDPOINT_NETWORK_IN = 0x82 };
static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0201,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64,
    .idVendor = 0xcafe,
    /* Alternate enumeration layout: avoid cached driver bindings. */
    .idProduct = 0x4062,
    .bcdDevice = 0x0100,
    .iManufacturer = STRING_MANUFACTURER,
    .iProduct = STRING_PRODUCT,
    .iSerialNumber = STRING_SERIAL,
    .bNumConfigurations = 1};
uint8_t const *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&device;
}

/* ECM is the default. Windows 10/11 requests RNDIS using MS OS 2.0 alternate
 * enumeration, before selecting configuration 1. */
static const uint8_t rndis_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_RNDIS_DESC_LEN, 0,
                          100),
    TUD_RNDIS_DESCRIPTOR(0, STRING_NETWORK_INTERFACE, ENDPOINT_NETWORK_NOTIFY, 8,
                         ENDPOINT_NETWORK_OUT, ENDPOINT_NETWORK_IN, CFG_TUD_NET_ENDPOINT_SIZE)};
static const uint8_t ecm_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_ECM_DESC_LEN, 0,
                          100),
    TUD_CDC_ECM_DESCRIPTOR(0, STRING_NETWORK_INTERFACE, STRING_HOST_MAC, ENDPOINT_NETWORK_NOTIFY,
                           64, ENDPOINT_NETWORK_OUT, ENDPOINT_NETWORK_IN, CFG_TUD_NET_ENDPOINT_SIZE,
                           CFG_TUD_NET_MTU)};
uint8_t const *tud_descriptor_configuration_cb(uint8_t i) {
    if (i)
        return NULL;
    return usb_profile == USB_PROFILE_RNDIS ? rndis_configuration : ecm_configuration;
}

enum { MS_OS_VENDOR_CODE = 0x20, MS_OS_DESCRIPTOR_INDEX = 7, MS_OS_ALT_ENUM_INDEX = 8 };
/* MS OS 2.0 set: one RNDIS function in the default configuration. */
/* clang-format off */
static const uint8_t ms_os_descriptor[] = {
    U16_TO_U8S_LE(10), U16_TO_U8S_LE(0), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(38),
    U16_TO_U8S_LE(8), U16_TO_U8S_LE(2), 0, 0, U16_TO_U8S_LE(28),
    U16_TO_U8S_LE(20), U16_TO_U8S_LE(3),
    'R', 'N', 'D', 'I', 'S', 0, 0, 0,
    '5', '1', '6', '2', '0', '0', '1', 0};
static const uint8_t bos[] = {
    TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN, 1),
    TUD_BOS_PLATFORM_DESCRIPTOR(TUD_BOS_MS_OS_20_UUID, U32_TO_U8S_LE(0x06030000),
        U16_TO_U8S_LE(sizeof(ms_os_descriptor)), MS_OS_VENDOR_CODE, 1)};
/* clang-format on */
_Static_assert(sizeof(ms_os_descriptor) == 38, "MS OS 2.0 descriptor set size");
uint8_t const *tud_descriptor_bos_cb(void) {
    return bos;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                const tusb_control_request_t *request) {
    if (request->bRequest != MS_OS_VENDOR_CODE)
        return false;
    if (request->bmRequestType == 0xc0 && request->wIndex == MS_OS_DESCRIPTOR_INDEX &&
        !request->wValue) {
        return stage != CONTROL_STAGE_SETUP ||
            tud_control_xfer(rhport, request, (void *)ms_os_descriptor, sizeof(ms_os_descriptor));
    }
    if (request->bmRequestType != 0x40 || request->wIndex != MS_OS_ALT_ENUM_INDEX ||
        request->wValue != 0x0100 || request->wLength || tud_mounted())
        return false;
    int expected = USB_PROFILE_PENDING;
    if (stage == CONTROL_STAGE_ACK) {
        return atomic_compare_exchange_strong(&usb_profile, &expected, USB_PROFILE_RNDIS);
    }
    if (stage != CONTROL_STAGE_SETUP)
        return false;
    expected = usb_profile;
    if (expected == USB_PROFILE_RNDIS)
        return false;
    if (!atomic_compare_exchange_strong(&usb_profile, &expected, USB_PROFILE_PENDING))
        return false;
    if (tud_control_status(rhport, request))
        return true;
    expected = USB_PROFILE_PENDING;
    atomic_compare_exchange_strong(&usb_profile, &expected, USB_PROFILE_ECM);
    return false;
}

void tud_event_hook_cb(uint8_t rhport, uint32_t event, bool in_isr) {
    (void)rhport;
    (void)in_isr;
    if (event != DCD_EVENT_BUS_RESET && event != DCD_EVENT_UNPLUGGED)
        return;
    usb_profile = USB_PROFILE_ECM;
}

uint16_t const *tud_descriptor_string_cb(uint8_t i, uint16_t lang) {
    (void)lang;
    static uint16_t text[48];
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
