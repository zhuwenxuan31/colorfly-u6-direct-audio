#include "pcm_session.h"

#include <string.h>

_Static_assert(sizeof(struct pcm_ring) == 8, "Shared PCM header must be ABI-independent");
_Static_assert(__atomic_always_lock_free(4, 0), "Shared cursors require lock-free atomics");

size_t pcm_ring_size(uint32_t capacity, uint32_t frame_bytes) {
    if (capacity == 0 || capacity > 32768 || (capacity & (capacity - 1)) ||
        (frame_bytes != 4 && frame_bytes != 6 && frame_bytes != 8))
        return 0;
    return sizeof(struct pcm_ring) + (size_t)capacity * frame_bytes;
}

int pcm_ring_init(struct pcm_ring *ring, size_t bytes, uint32_t capacity, uint32_t frame_bytes) {
    size_t required = pcm_ring_size(capacity, frame_bytes);
    if (!ring || required == 0 || bytes != required) return PCM_INVALID;
    memset(ring, 0, bytes);
    return PCM_OK;
}

static uint32_t contiguous(uint32_t cursor, uint32_t capacity, uint32_t count) {
    uint32_t until_wrap = capacity - (cursor & (capacity - 1));
    return count < until_wrap ? count : until_wrap;
}

int pcm_ring_write(struct pcm_ring *ring, uint32_t capacity, uint32_t frame_bytes,
                   const void *input, size_t frames, size_t *written) {
    if (!written) return PCM_INVALID;
    *written = 0;
    if (!ring || !input || !frames || !pcm_ring_size(capacity, frame_bytes)) return PCM_INVALID;

    uint32_t produced = __atomic_load_n(&ring->produced, __ATOMIC_RELAXED);
    uint32_t consumed = __atomic_load_n(&ring->consumed, __ATOMIC_ACQUIRE);
    uint32_t queued = produced - consumed;
    if (queued > capacity) return PCM_INVALID;

    uint32_t available = capacity - queued;
    if (available == 0) return PCM_WOULD_BLOCK;
    uint32_t count = frames < available ? (uint32_t)frames : available;
    uint32_t first = contiguous(produced, capacity, count);
    uint8_t *payload = (uint8_t *)(ring + 1);
    memcpy(payload + (size_t)(produced & (capacity - 1)) * frame_bytes, input,
           (size_t)first * frame_bytes);
    if (first < count) {
        memcpy(payload, (const uint8_t *)input + (size_t)first * frame_bytes,
               (size_t)(count - first) * frame_bytes);
    }
    __atomic_store_n(&ring->produced, produced + count, __ATOMIC_RELEASE);
    *written = count;
    return PCM_OK;
}

int pcm_session_init(struct pcm_session *session, struct pcm_service *service, uint64_t owner,
                     struct pcm_format format, struct pcm_ring *ring, size_t bytes, uint32_t capacity) {
    if (!session || !service || !owner || !ring || !format.rate) return PCM_INVALID;
    uint32_t frame_bytes = (uint32_t)pcm_frame_bytes(format);
    size_t required = pcm_ring_size(capacity, frame_bytes);
    if (required == 0 || bytes != required) return PCM_INVALID;

    session->service = service;
    session->ring = ring;
    session->owner = owner;
    session->format = format;
    session->capacity = capacity;
    session->consumed = 0;
    session->frame_bytes = frame_bytes;
    session->active = 0;
    return PCM_OK;
}

int pcm_session_start(struct pcm_session *session) {
    if (!session->ring || session->active) return PCM_INVALID;
    int status = pcm_service_start(session->service, session->owner, session->format);
    if (status == PCM_OK) session->active = 1;
    return status;
}

int pcm_session_stop(struct pcm_session *session) {
    int status = session->active ? pcm_service_stop(session->service, session->owner) : PCM_OK;
    session->ring = NULL;
    session->active = 0;
    return status;
}

int pcm_session_drain(struct pcm_session *session) {
    if (!session->active) return PCM_INVALID;
    struct pcm_ring *ring = session->ring;
    uint32_t produced = __atomic_load_n(&ring->produced, __ATOMIC_ACQUIRE);
    uint32_t available = produced - session->consumed;
    if (!available) return PCM_WOULD_BLOCK;
    if (available > session->capacity) {
        pcm_session_stop(session);
        return PCM_INVALID;
    }

    uint32_t count = available < PCM_MAX_BLOCK_FRAMES ? available : PCM_MAX_BLOCK_FRAMES;
    uint32_t first = contiguous(session->consumed, session->capacity, count);
    const uint8_t *payload = (const uint8_t *)(ring + 1);
    uint32_t offset = session->consumed & (session->capacity - 1);
    size_t frame_bytes = session->frame_bytes;
    memcpy(session->scratch, payload + offset * frame_bytes, first * frame_bytes);
    if (first < count) {
        memcpy(session->scratch + first * frame_bytes, payload, (count - first) * frame_bytes);
    }
    int status = pcm_push(session->service, session->owner, session->scratch, count * frame_bytes);
    if (status != PCM_OK) {
        pcm_session_stop(session);
        return status;
    }
    session->consumed += count;
    __atomic_store_n(&ring->consumed, session->consumed, __ATOMIC_RELEASE);
    return PCM_OK;
}
