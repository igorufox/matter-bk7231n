/* The ARMINO API of matter_pal.h (bk_* key-value store, Wi-Fi status and events, OTA, entropy) is implemented by
 * ESPurna: code/espurna/arch/beken/matter_beken.cpp. The sizing stubs are in bk7231n_port.c.stubs */
#include "matter_pal.h"

#include <FreeRTOS.h>

#include <stdbool.h>
#include <stdint.h>

/* __atomic_* of the library (connectedhomeip: std::atomic, __atomic_compare_exchange_1/4, fetch_add/sub/and/or_4).
 * The ARM968E-S (ARMv5) has no atomic instructions and libatomic is not linked: as libatomic for a single core MCU,
 * every operation with the interrupts masked. They were ESPurna's (compat_beken.cpp) while only this library needed
 * them; the port is where they belong */
#define BK_ATOMIC(SIZE, TYPE)                                                                                             \
    TYPE __atomic_exchange_##SIZE(volatile void * ptr, TYPE value, int order)                                            \
    {                                                                                                                     \
        (void) order;                                                                                                     \
        const int level = port_disable_interrupts_flag();                                                               \
        volatile TYPE * target = (volatile TYPE *) ptr;                                                                  \
        const TYPE out = *target;                                                                                         \
        *target = value;                                                                                                  \
        port_enable_interrupts_flag(level);                                                                               \
        return out;                                                                                                       \
    }                                                                                                                     \
    bool __atomic_compare_exchange_##SIZE(volatile void * ptr, void * expected, TYPE desired, bool weak, int success,   \
                                          int failure)                                                                    \
    {                                                                                                                     \
        (void) weak;                                                                                                      \
        (void) success;                                                                                                   \
        (void) failure;                                                                                                   \
        const int level = port_disable_interrupts_flag();                                                               \
        volatile TYPE * target = (volatile TYPE *) ptr;                                                                  \
        TYPE * current = (TYPE *) expected;                                                                               \
        bool out = (*target == *current);                                                                                 \
        if (out)                                                                                                          \
            *target = desired;                                                                                            \
        else                                                                                                              \
            *current = *target;                                                                                           \
        port_enable_interrupts_flag(level);                                                                               \
        return out;                                                                                                       \
    }                                                                                                                     \
    BK_ATOMIC_FETCH(SIZE, TYPE, add, +)                                                                                   \
    BK_ATOMIC_FETCH(SIZE, TYPE, sub, -)                                                                                   \
    BK_ATOMIC_FETCH(SIZE, TYPE, and, &)                                                                                   \
    BK_ATOMIC_FETCH(SIZE, TYPE, or, |)                                                                                    \
    BK_ATOMIC_FETCH(SIZE, TYPE, xor, ^)

#define BK_ATOMIC_FETCH(SIZE, TYPE, NAME, OP)                                                                             \
    TYPE __atomic_fetch_##NAME##_##SIZE(volatile void * ptr, TYPE value, int order)                                      \
    {                                                                                                                     \
        (void) order;                                                                                                     \
        const int level = port_disable_interrupts_flag();                                                               \
        volatile TYPE * target = (volatile TYPE *) ptr;                                                                  \
        const TYPE out = *target;                                                                                         \
        *target = (TYPE) (out OP value);                                                                                  \
        port_enable_interrupts_flag(level);                                                                               \
        return out;                                                                                                       \
    }

BK_ATOMIC(1, uint8_t)
BK_ATOMIC(2, uint16_t)
BK_ATOMIC(4, unsigned int)
