/**
 ******************************************************************************
 * @file    smart_io_ring.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   Lock-free single-producer single-consumer ring of fixed-size
 *          entries. push is inlined into the producer (ISR in IRAM); the
 *          buffer must be in internal RAM. Callers pass sizeof(entry) so the
 *          copy and index scaling are compile-time constants.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_RING_H_
#define SMART_IO_RING_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

/* head is written by the producer only, tail by the consumer only, both with
 * atomic acquire/release. */
typedef struct smart_io_ring_s
{
    uint8_t* buffer;
    uint32_t mask; /* depth - 1, depth a power of two */
    uint32_t head;
    uint32_t tail;
    uint32_t dropped; /* producer side, read and reset by the consumer */
} smart_io_ring_t;

/* Exposed API ------------------------------------------------------------- */

/* FORCE_INLINE_ATTR: an out-of-line copy would sit in flash, unusable from an
 * IRAM ISR during a flash write. */
FORCE_INLINE_ATTR void smart_io_ring_init(smart_io_ring_t* ring, uint8_t* buffer, uint32_t depth)
{
    ring->buffer  = buffer;
    ring->mask    = depth - 1u;
    ring->head    = 0u;
    ring->tail    = 0u;
    ring->dropped = 0u;
}

FORCE_INLINE_ATTR uint32_t smart_io_ring_count(const smart_io_ring_t* ring)
{
    const uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    const uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);

    return head - tail;
}

/* Producer only. False and dropped++ when full. */
FORCE_INLINE_ATTR bool smart_io_ring_push(
    smart_io_ring_t* ring,
    const void* entry,
    uint32_t entry_size)
{
    bool pushed         = false;
    const uint32_t head = ring->head;
    const uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);

    if ((head - tail) <= ring->mask)
    {
        memcpy(&ring->buffer[(head & ring->mask) * entry_size], entry, entry_size);
        __atomic_store_n(&ring->head, head + 1u, __ATOMIC_RELEASE);
        pushed = true;
    }
    else
    {
        __atomic_fetch_add(&ring->dropped, 1u, __ATOMIC_RELAXED);
    }

    return pushed;
}

/* Consumer only. */
FORCE_INLINE_ATTR bool smart_io_ring_pop(smart_io_ring_t* ring, void* entry, uint32_t entry_size)
{
    bool popped         = false;
    const uint32_t tail = ring->tail;
    const uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);

    if (head != tail)
    {
        memcpy(entry, &ring->buffer[(tail & ring->mask) * entry_size], entry_size);
        __atomic_store_n(&ring->tail, tail + 1u, __ATOMIC_RELEASE);
        popped = true;
    }

    return popped;
}

/* Consumer only. Copies the oldest entry without removing it. */
FORCE_INLINE_ATTR bool smart_io_ring_peek(
    const smart_io_ring_t* ring,
    void* entry,
    uint32_t entry_size)
{
    bool found          = false;
    const uint32_t tail = ring->tail;
    const uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);

    if (head != tail)
    {
        memcpy(entry, &ring->buffer[(tail & ring->mask) * entry_size], entry_size);
        found = true;
    }

    return found;
}

/* Consumer only, after a successful peek. */
FORCE_INLINE_ATTR void smart_io_ring_drop(smart_io_ring_t* ring)
{
    __atomic_store_n(&ring->tail, ring->tail + 1u, __ATOMIC_RELEASE);
}

/* Consumer only. */
FORCE_INLINE_ATTR uint32_t smart_io_ring_take_dropped(smart_io_ring_t* ring)
{
    return __atomic_exchange_n(&ring->dropped, 0u, __ATOMIC_RELAXED);
}

#endif /* SMART_IO_RING_H_ */

/**** END OF FILE ****/
