#ifndef U6_PCM_SESSION_H
#define U6_PCM_SESSION_H

#include "pcm_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-width shared header for 32-bit and 64-bit clients. PCM follows it. */
struct pcm_ring {
    uint32_t produced;
    uint32_t consumed;
};

struct pcm_session {
    struct pcm_service *service;
    struct pcm_ring *ring;
    uint64_t owner;
    struct pcm_format format;
    uint32_t capacity;
    uint32_t consumed;
    uint32_t frame_bytes;
    int active;
    uint8_t scratch[PCM_MAX_BLOCK_FRAMES * 8];
};

size_t pcm_ring_size(uint32_t capacity, uint32_t frame_bytes);
int pcm_ring_init(struct pcm_ring *ring, size_t bytes, uint32_t capacity, uint32_t frame_bytes);
int pcm_ring_write(struct pcm_ring *ring, uint32_t capacity, uint32_t frame_bytes,
                   const void *input, size_t frames, size_t *written);

/* The service creates a fresh mapping and serializes these calls on its output
 * worker. The owner ID and mapping size come from server-owned state. */
int pcm_session_init(struct pcm_session *session, struct pcm_service *service, uint64_t owner,
                     struct pcm_format format, struct pcm_ring *ring, size_t bytes, uint32_t capacity);
int pcm_session_start(struct pcm_session *session);
int pcm_session_drain(struct pcm_session *session);
/* Pause, seek, close and client death retire this mapping. A new start uses a
 * new session and mapping, so pending writes from an old client are inert. */
int pcm_session_stop(struct pcm_session *session);

#ifdef __cplusplus
}
#endif

#endif
