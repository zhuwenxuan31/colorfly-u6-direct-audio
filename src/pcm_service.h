#ifndef U6_PCM_SERVICE_H
#define U6_PCM_SERVICE_H

#include <stddef.h>
#include <stdint.h>

enum pcm_encoding { PCM_S16, PCM_S24_PACKED, PCM_S32, PCM_FLOAT };

enum pcm_status {
    PCM_OK, PCM_INVALID, PCM_UNSUPPORTED, PCM_BUSY, PCM_NOT_OWNER,
    PCM_FORMAT_CHANGED, PCM_IO, PCM_WOULD_BLOCK
};

struct pcm_format {
    uint32_t rate;
    uint32_t channels;
    enum pcm_encoding encoding;
};

/* open may report negotiated parameters. On error it must release its handle.
 * write consumes the entire block on success. Errors end the stream. */
struct pcm_backend {
    void *context;
    int (*open)(void *context, struct pcm_format requested, struct pcm_format *actual);
    int (*write)(void *context, const uint8_t *data, size_t frames);
    void (*close)(void *context);
};

enum { PCM_MAX_BLOCK_FRAMES = 4096 };

struct pcm_service {
    struct pcm_backend backend;
    struct pcm_format source;
    uint64_t owner;
    uint64_t frames_written;
    uint64_t non_finite;
    uint8_t conversion[PCM_MAX_BLOCK_FRAMES * 8];
};

#ifdef __cplusplus
extern "C" {
#endif

/* A single output worker serializes these calls. Owner 0 is reserved. */
size_t pcm_frame_bytes(struct pcm_format source);
void pcm_service_init(struct pcm_service *service, struct pcm_backend backend);
int pcm_service_start(struct pcm_service *service, uint64_t owner, struct pcm_format source);
int pcm_push(struct pcm_service *service, uint64_t owner, const void *data, size_t bytes);
int pcm_service_stop(struct pcm_service *service, uint64_t owner);

#ifdef __cplusplus
}
#endif

#endif
