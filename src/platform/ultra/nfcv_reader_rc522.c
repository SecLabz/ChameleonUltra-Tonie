#include "nrf.h"
#include "nrf_gpio.h"
#include "platform/platform.h"
#include "platform/ultra/board.h"
#include "platform/ultra/internal.h"
#include "tag/nfcv_reader.h"
#include "tag/protocol/nfcv/commands.h"
#include "tag/protocol/nfcv/reader_codec.h"
#include <string.h>

#define CPU_CYCLES_PER_US 64u
#define SPI_TIMEOUT_CYCLES 64000u
#define NFCV_DMA_TIMEOUT_CYCLES 960000u
#define ETU_CYCLES_NUMERATOR 819200u
#define ETU_CYCLES_DENOMINATOR 1356u
#define TX_START_MARGIN_CYCLES 640u
#define FRONTEND_SETTLE_US 1000u
#define READER_SETTLE_MS 10u

enum {
    RC522_REG_COMMAND = 0x01u,
    RC522_REG_COM_IRQ = 0x04u,
    RC522_REG_FIFO_LEVEL = 0x0au,
    RC522_REG_TX_MODE = 0x12u,
    RC522_REG_RX_MODE = 0x13u,
    RC522_REG_TX_CONTROL = 0x14u,
    RC522_REG_TX_AUTO = 0x15u,
    RC522_REG_TX_SEL = 0x16u,
    RC522_REG_RX_SEL = 0x17u,
    RC522_REG_RX_THRESHOLD = 0x18u,
    RC522_REG_DEMOD = 0x19u,
    RC522_REG_RF_CFG = 0x26u,
    RC522_REG_GS_N = 0x27u,
    RC522_REG_CW_GS_CFG = 0x28u,
    RC522_REG_MOD_GS_CFG = 0x29u,
    RC522_REG_TEST_ADC = 0x3bu,
};

enum {
    RC522_SPI_ADDRESS_MASK = 0x7eu,
    RC522_SPI_READ = 0x80u,
    RC522_PCD_IDLE = 0x00u,
    RC522_PCD_RECEIVE = 0x08u,
    RC522_PCD_RESET = 0x0fu,
    RC522_CLEAR_IRQ_FLAGS = 0x7fu,
    RC522_FLUSH_FIFO = 0x80u,
    RC522_FIELD_OFF = 0x80u,
    RC522_FIELD_ON = 0x83u,
    RC522_TX_CONSTANT_CARRIER = 0x30u,
    RC522_CARRIER_LEVEL = 32u,
    RC522_ASK_SHALLOW = 12u,
    SOFTWARE_RX_MODE = 0x14u,
    SOFTWARE_RX_SELECT = 0x84u,
    SOFTWARE_RX_THRESHOLD = 0x84u,
    SOFTWARE_DEMOD = 0x4du,
    SOFTWARE_RF_GAIN = 0x48u,
    SOFTWARE_N_CONDUCTANCE = 0x88u,
};

static uint8_t dma_address = ((RC522_REG_TEST_ADC << 1) & RC522_SPI_ADDRESS_MASK) | RC522_SPI_READ;
static uint8_t iq_samples[NFCV_READER_IQ_SAMPLES + 1u];
static bool spi_ok;

#ifdef NFCV_SCAN_DIAGNOSTICS
static bool extended_auth, verify_auth;
static nfcv_scan_trace_t trace[32];
static size_t trace_count;

void nfcv_reader_trace_configure(bool use_extended_auth, bool check_auth) {
    extended_auth = use_extended_auth;
    verify_auth = check_auth;
    trace_count = 0;
}

bool nfcv_reader_trace_verify_auth(void) {
    return verify_auth;
}

const nfcv_scan_trace_t *nfcv_reader_trace(size_t *count) {
    *count = trace_count;
    return trace;
}
#endif

static void delay_us(uint32_t us) {
    uint32_t start = DWT->CYCCNT;
    while (DWT->CYCCNT - start < us * CPU_CYCLES_PER_US) {
    }
}

static void delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        platform_watchdog_feed();
        delay_us(1000u);
    }
}

static uint8_t spi_transfer(uint8_t value) {
    if (!spi_ok)
        return 0;
    NRF_SPI0->EVENTS_READY = 0;
    NRF_SPI0->TXD = value;
    uint32_t start = DWT->CYCCNT;
    while (!NRF_SPI0->EVENTS_READY) {
        if (DWT->CYCCNT - start > SPI_TIMEOUT_CYCLES) {
            spi_ok = false;
            return 0;
        }
    }
    return NRF_SPI0->RXD;
}

static void write_register(uint8_t reg, uint8_t value) {
    nrf_gpio_pin_clear(HF_SPI_SELECT);
    spi_transfer((reg << 1) & RC522_SPI_ADDRESS_MASK);
    spi_transfer(value);
    nrf_gpio_pin_set(HF_SPI_SELECT);
}

static void symbol(uint32_t start, unsigned etu, uint8_t reg, uint8_t value) {
    uint32_t target = start + (etu * ETU_CYCLES_NUMERATOR) / ETU_CYCLES_DENOMINATOR;
    while ((int32_t)(DWT->CYCCNT - target) < 0) {
    }
    write_register(reg, value);
}

static void nfcv_tx(const uint8_t *levels, size_t ticks) {
    uint8_t reg = RC522_REG_CW_GS_CFG, high = RC522_CARRIER_LEVEL, low = 0;
    write_register(RC522_REG_TX_SEL, RC522_TX_CONSTANT_CARRIER);
    write_register(reg, high);
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    uint32_t start = DWT->CYCCNT + TX_START_MARGIN_CYCLES;
    uint8_t previous = 0;
    for (unsigned tick = 0; tick < ticks; tick++) {
        if (tick == 0 || levels[tick] != previous) {
            symbol(start, tick, reg, levels[tick] ? low : high);
            previous = levels[tick];
        }
    }
    symbol(start, (unsigned)ticks, reg, high);
    __set_PRIMASK(mask);
}

static void sample_iq(size_t count) {
    if (!spi_ok)
        return;
    NRF_SPI0->ENABLE = 0;
    NRF_SPIM0->PSEL.SCK = HF_SPI_SCK;
    NRF_SPIM0->PSEL.MOSI = HF_SPI_MOSI;
    NRF_SPIM0->PSEL.MISO = HF_SPI_MISO;
    NRF_SPIM0->CONFIG = 0;
    NRF_SPIM0->FREQUENCY = SPIM_FREQUENCY_FREQUENCY_M8;
    NRF_SPIM0->TXD.PTR = (uint32_t)&dma_address;
    NRF_SPIM0->TXD.MAXCNT = 1;
    NRF_SPIM0->ORC = dma_address;
    NRF_SPIM0->RXD.PTR = (uint32_t)iq_samples;
    /* Discard the RX byte clocked while sending the register address. */
    NRF_SPIM0->RXD.MAXCNT = count + 1u;
    NRF_SPIM0->EVENTS_END = 0;
    NRF_SPIM0->ENABLE = SPIM_ENABLE_ENABLE_Enabled;
    nrf_gpio_pin_clear(HF_SPI_SELECT);
    uint32_t dma_started = DWT->CYCCNT;
    NRF_SPIM0->EVENTS_STOPPED = 0;
    NRF_SPIM0->TASKS_START = 1;
    const uint32_t timeout = NFCV_DMA_TIMEOUT_CYCLES +
                             (count - NFCV_READER_DEFAULT_IQ_SAMPLES) * CPU_CYCLES_PER_US;
    while (!NRF_SPIM0->EVENTS_END) {
        if (DWT->CYCCNT - dma_started > timeout) {
            spi_ok = false;
            break;
        }
    }
    NRF_SPIM0->TASKS_STOP = 1;
    uint32_t start = DWT->CYCCNT;
    while (!NRF_SPIM0->EVENTS_STOPPED && DWT->CYCCNT - start < SPI_TIMEOUT_CYCLES) {
    }
    if (!NRF_SPIM0->EVENTS_STOPPED)
        NVIC_SystemReset();
    nrf_gpio_pin_set(HF_SPI_SELECT);
    NRF_SPIM0->ENABLE = 0;
    NRF_SPI0->ENABLE = SPI_ENABLE_ENABLE_Enabled;
}

void nfcv_reader_phy_open(void) {
    ultra_start_hf_clock();
    nrf_gpio_pin_set(READER_POWER);
    nrf_gpio_pin_clear(HF_ANT_SEL);
    nrf_gpio_pin_clear(FIELD_LED);
    delay_ms(READER_SETTLE_MS);

    NRF_SPIM0->ENABLE = 0;
    NRF_SPI0->ENABLE = 0;
    NRF_SPI0->PSELSCK = HF_SPI_SCK;
    NRF_SPI0->PSELMOSI = HF_SPI_MOSI;
    NRF_SPI0->PSELMISO = HF_SPI_MISO;
    nrf_gpio_cfg_output(HF_SPI_SELECT);
    nrf_gpio_pin_set(HF_SPI_SELECT);
    nrf_gpio_cfg_output(HF_SPI_SCK);
    nrf_gpio_pin_clear(HF_SPI_SCK);
    nrf_gpio_cfg_output(HF_SPI_MOSI);
    nrf_gpio_cfg_input(HF_SPI_MISO, NRF_GPIO_PIN_NOPULL);
    NRF_SPI0->FREQUENCY = SPI_FREQUENCY_FREQUENCY_M8;
    NRF_SPI0->CONFIG = 0;
    NRF_SPI0->ENABLE = SPI_ENABLE_ENABLE_Enabled;
    (void)NRF_SPI0->RXD;
    NRF_SPI0->EVENTS_READY = 0;
    spi_ok = true;

    write_register(RC522_REG_COMMAND, RC522_PCD_RESET);
    delay_ms(READER_SETTLE_MS);
    write_register(RC522_REG_TX_MODE, 0);
    write_register(RC522_REG_RX_MODE, SOFTWARE_RX_MODE);
    write_register(RC522_REG_TX_AUTO, 0);
    write_register(RC522_REG_TX_SEL, RC522_TX_CONSTANT_CARRIER);
    write_register(RC522_REG_RX_SEL, SOFTWARE_RX_SELECT);
    write_register(RC522_REG_RX_THRESHOLD, SOFTWARE_RX_THRESHOLD);
    write_register(RC522_REG_DEMOD, SOFTWARE_DEMOD);
    write_register(RC522_REG_RF_CFG, SOFTWARE_RF_GAIN);
    write_register(RC522_REG_GS_N, SOFTWARE_N_CONDUCTANCE);
    write_register(RC522_REG_CW_GS_CFG, RC522_CARRIER_LEVEL);
    write_register(RC522_REG_MOD_GS_CFG, RC522_ASK_SHALLOW);
    write_register(RC522_REG_TX_CONTROL, RC522_FIELD_ON);
    delay_ms(READER_SETTLE_MS);
}

void nfcv_reader_phy_close(void) {
    if (spi_ok) {
        write_register(RC522_REG_COMMAND, RC522_PCD_IDLE);
        write_register(RC522_REG_TX_CONTROL, RC522_FIELD_OFF);
    }
    nrf_gpio_pin_set(HF_SPI_SELECT);
    NRF_SPIM0->ENABLE = 0;
    NRF_SPI0->ENABLE = 0;
    nrf_gpio_pin_clear(READER_POWER);
    nrf_gpio_pin_set(HF_ANT_SEL);
    nrf_gpio_pin_set(FIELD_LED);
    delay_ms(READER_SETTLE_MS);
}

size_t nfcv_reader_phy_exchange(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_cap) {
    platform_watchdog_feed();
    if (!spi_ok)
        return 0;
    uint8_t levels[NFCV_READER_MAX_TICKS];
    size_t ticks = nfcv_reader_encode(tx, tx_len, levels, sizeof(levels));
    if (!ticks)
        return 0;
    write_register(RC522_REG_COMMAND, RC522_PCD_IDLE);
    write_register(RC522_REG_COM_IRQ, RC522_CLEAR_IRQ_FLAGS);
    write_register(RC522_REG_FIFO_LEVEL, RC522_FLUSH_FIFO);
    write_register(RC522_REG_COMMAND, RC522_PCD_RECEIVE);
    delay_us(FRONTEND_SETTLE_US);
    size_t count = NFCV_READER_DEFAULT_IQ_SAMPLES;
#ifdef NFCV_SCAN_DIAGNOSTICS
    if (extended_auth && tx[1] == ISO15693_CMD_NXP_SET_PASSWORD)
        count = NFCV_READER_IQ_SAMPLES;
#endif
    memset(iq_samples, 0, count + 1u);
    nfcv_tx(levels, ticks);
#ifdef NFCV_SCAN_DIAGNOSTICS
    const uint32_t started = DWT->CYCCNT;
#endif
    sample_iq(count);
#ifdef NFCV_SCAN_DIAGNOSTICS
    const uint32_t captured = DWT->CYCCNT - started;
#endif
    unsigned offset = 0;
    size_t got = spi_ok ? nfcv_reader_decode_iq(iq_samples + 1u, count, rx, rx_cap, &offset) : 0u;
#ifdef NFCV_SCAN_DIAGNOSTICS
    if (trace_count < sizeof(trace) / sizeof(trace[0]))
        trace[trace_count++] = (nfcv_scan_trace_t){
            tx[1], got, got >= 4u && (rx[0] & 1u) ? rx[1] : 0u, offset,
            captured / CPU_CYCLES_PER_US};
#endif
    return got;
}
