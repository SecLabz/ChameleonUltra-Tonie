#pragma once
#include "nrf.h"
#include <assert.h>
#include <stdint.h>
#include "soc/nrfx_irqs.h"
#define NRFX_ASSERT(x) assert(x)
#define NRFX_STATIC_ASSERT(x) _Static_assert(x, #x)
#define NRFX_IRQ_PRIORITY_SET(i, p) NVIC_SetPriority(i, p)
#define NRFX_IRQ_ENABLE(i) NVIC_EnableIRQ(i)
#define NRFX_IRQ_DISABLE(i) NVIC_DisableIRQ(i)
#define NRFX_IRQ_PENDING_CLEAR(i) NVIC_ClearPendingIRQ(i)
#define NRFX_IRQ_PENDING_SET(i) NVIC_SetPendingIRQ(i)
#define NRFX_IRQ_IS_ENABLED(i) NVIC_GetEnableIRQ(i)
#define NRFX_IRQ_IS_PENDING(i) NVIC_GetPendingIRQ(i)
#define NRFX_CRITICAL_SECTION_ENTER()                                                              \
    {                                                                                              \
        uint32_t platform_irq_state = __get_PRIMASK();                                             \
        __disable_irq();
#define NRFX_CRITICAL_SECTION_EXIT()                                                               \
    __set_PRIMASK(platform_irq_state);                                                             \
    }
#define NRFX_DELAY_DWT_BASED 1
#include "soc/nrfx_coredep.h"
#define NRFX_DELAY_US(us) nrfx_coredep_delay_us(us)
#define nrfx_atomic_t volatile uint32_t
#define NRFX_ATOMIC_FETCH_STORE(p, v) __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_OR(p, v) __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_AND(p, v) __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_XOR(p, v) __atomic_fetch_xor(p, v, __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_ADD(p, v) __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST)
#define NRFX_ATOMIC_FETCH_SUB(p, v) __atomic_fetch_sub(p, v, __ATOMIC_SEQ_CST)
#define NRFX_CUSTOM_ERROR_CODES 0
#define NRFX_PPI_CHANNELS_USED 0
#define NRFX_PPI_GROUPS_USED 0
