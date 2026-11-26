#include "pcm_service.h"

#include <limits.h>
#include <math.h>
#include <string.h>

_Static_assert(sizeof(float) == sizeof(uint32_t), "Expected binary32 PCM");

static size_t sample_bytes(enum pcm_encoding encoding) {
    switch (encoding) {
    case PCM_S16: return 2;
    case PCM_S24_PACKED: return 3;
    case PCM_S32:
    case PCM_FLOAT: return 4;
    }
    return 0;
}

size_t pcm_frame_bytes(struct pcm_format source) {
    return source.channels == 2 ? 2 * sample_bytes(source.encoding) : 0;
}

void pcm_service_init(struct pcm_service *service, struct pcm_backend backend) {
    memset(service, 0, sizeof(*service));
    service->backend = backend;
}

int pcm_service_start(struct pcm_service *service, uint64_t owner, struct pcm_format source) {
    if (owner == 0 || source.rate == 0) return PCM_INVALID;
    if (service->owner != 0) return PCM_BUSY;
    if (pcm_frame_bytes(source) == 0) return PCM_UNSUPPORTED;

    struct pcm_format requested = source;
    if (source.encoding != PCM_S16) requested.encoding = PCM_S32;
    struct pcm_format actual = {0};
    struct pcm_backend *backend = &service->backend;
    int status = backend->open(backend->context, requested, &actual);
    if (status != PCM_OK) return status;
    if (actual.rate != requested.rate || actual.channels != requested.channels ||
        actual.encoding != requested.encoding) {
        backend->close(backend->context);
        return PCM_FORMAT_CHANGED;
    }
    service->source = source;
    service->owner = owner;
    service->frames_written = 0;
    service->non_finite = 0;
    return PCM_OK;
}

static uint32_t little_endian_float_to_s32(const uint8_t *sample, uint64_t *non_finite) {
    uint32_t bits = (uint32_t)sample[0] | (uint32_t)sample[1] << 8 |
                    (uint32_t)sample[2] << 16 | (uint32_t)sample[3] << 24;
    float value;
    memcpy(&value, &bits, sizeof(value));
    if (!isfinite(value)) {
        (*non_finite)++;
        return 0;
    }
    if (value <= -1.0f) return (uint32_t)INT32_MIN;
    if (value >= 1.0f) return INT32_MAX;
    return (uint32_t)(int32_t)llround((double)value * 0x1p31);
}

static void convert(struct pcm_service *service, const uint8_t *input, size_t samples) {
    uint8_t *output = service->conversion;
    if (service->source.encoding == PCM_S24_PACKED) {
        for (size_t i = 0; i < samples; i++, input += 3, output += 4) {
            output[0] = 0;
            memcpy(output + 1, input, 3);
        }
        return;
    }
    for (size_t i = 0; i < samples; i++, input += 4, output += 4) {
        uint32_t value = little_endian_float_to_s32(input, &service->non_finite);
        output[0] = (uint8_t)value;
        output[1] = (uint8_t)(value >> 8);
        output[2] = (uint8_t)(value >> 16);
        output[3] = (uint8_t)(value >> 24);
    }
}

int pcm_push(struct pcm_service *service, uint64_t owner, const void *data, size_t bytes) {
    if (owner == 0 || owner != service->owner) return PCM_NOT_OWNER;
    size_t frame_bytes = pcm_frame_bytes(service->source);
    size_t frames = bytes / frame_bytes;
    if (!data || bytes == 0 || bytes % frame_bytes || frames > PCM_MAX_BLOCK_FRAMES)
        return PCM_INVALID;

    const uint8_t *output = data;
    if (service->source.encoding == PCM_FLOAT || service->source.encoding == PCM_S24_PACKED) {
        convert(service, data, frames * service->source.channels);
        output = service->conversion;
    }
    int status = service->backend.write(service->backend.context, output, frames);
    if (status == PCM_OK) {
        service->frames_written += frames;
    } else {
        pcm_service_stop(service, owner);
    }
    return status;
}

int pcm_service_stop(struct pcm_service *service, uint64_t owner) {
    if (owner == 0 || owner != service->owner) return PCM_NOT_OWNER;
    service->backend.close(service->backend.context);
    service->owner = 0;
    return PCM_OK;
}
