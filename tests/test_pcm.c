#include "pcm_service.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct fake {
    int opens, closes, writes, mismatch, write_error;
    struct pcm_format requested;
    uint8_t bytes[24];
};

static int open_pcm(void *ctx, struct pcm_format requested, struct pcm_format *actual) {
    struct fake *f = ctx;
    f->opens++;
    f->requested = requested;
    *actual = requested;
    if (f->mismatch) actual->rate = 48000;
    return PCM_OK;
}

static int write_pcm(void *ctx, const uint8_t *data, size_t frames) {
    struct fake *f = ctx;
    size_t n = frames * 2 * (f->requested.encoding == PCM_S16 ? 2 : 4);
    assert(n <= sizeof(f->bytes));
    if (f->write_error) return PCM_IO;
    f->writes++;
    memcpy(f->bytes, data, n);
    return PCM_OK;
}

static void close_pcm(void *ctx) { ((struct fake *)ctx)->closes++; }

int main(void) {
    struct fake f = {0};
    struct pcm_service s;
    pcm_service_init(&s, (struct pcm_backend){&f, open_pcm, write_pcm, close_pcm});
    assert(pcm_service_start(&s, 0, (struct pcm_format){96000, 2, PCM_S24_PACKED}) == PCM_INVALID);
    assert(pcm_service_start(&s, 1, (struct pcm_format){96000, 1, PCM_S24_PACKED}) == PCM_UNSUPPORTED);
    assert(f.opens == 0);
    assert(pcm_service_start(&s, 1, (struct pcm_format){96000, 2, PCM_S24_PACKED}) == PCM_OK);
    assert(f.requested.rate == 96000 && f.requested.encoding == PCM_S32);
    assert(pcm_service_start(&s, 2, (struct pcm_format){44100, 2, PCM_S16}) == PCM_BUSY);
    assert(f.opens == 1);
    const uint8_t input[] = {1,0,0, 255,255,255, 255,255,127, 0,0,128};
    const uint8_t output[] = {0,1,0,0, 0,255,255,255, 0,255,255,127, 0,0,0,128};
    assert(pcm_push(&s, 2, input, sizeof(input)) == PCM_NOT_OWNER);
    assert(pcm_push(&s, 1, input, sizeof(input) - 1) == PCM_INVALID);
    assert(pcm_push(&s, 1, input, sizeof(input)) == PCM_OK);
    assert(memcmp(f.bytes, output, sizeof(output)) == 0 && s.frames_written == 2);
    assert(pcm_service_stop(&s, 1) == PCM_OK && f.closes == 1);

    f.mismatch = 1;
    assert(pcm_service_start(&s, 3, (struct pcm_format){192000, 2, PCM_FLOAT}) == PCM_FORMAT_CHANGED);
    assert(f.closes == 2 && f.writes == 1 && s.owner == 0);
    f.mismatch = 0;
    assert(pcm_service_start(&s, 3, (struct pcm_format){192000, 2, PCM_FLOAT}) == PCM_OK);
    const float floats[] = {0x1p-23f, -0x1p-23f, 1.f, -1.f, NAN, INFINITY};
    const uint8_t float_output[] = {
        0,1,0,0, 0,255,255,255, 255,255,255,127, 0,0,0,128, 0,0,0,0, 0,0,0,0
    };
    assert(pcm_push(&s, 3, floats, sizeof(floats)) == PCM_OK);
    assert(memcmp(f.bytes, float_output, sizeof(float_output)) == 0);
    assert(s.frames_written == 3 && s.non_finite == 2);
    f.write_error = 1;
    assert(pcm_push(&s, 3, floats, sizeof(floats)) == PCM_IO);
    assert(s.owner == 0 && f.closes == 3);
    f.write_error = 0;

    const uint8_t cd[] = {0x1, 0x80, 0xff, 0x7f};
    assert(pcm_service_start(&s, 4, (struct pcm_format){44100, 2, PCM_S16}) == PCM_OK);
    assert(pcm_push(&s, 4, cd, sizeof(cd)) == PCM_OK);
    assert(f.requested.rate == 44100 && f.requested.encoding == PCM_S16);
    assert(memcmp(f.bytes, cd, sizeof(cd)) == 0);
    assert(pcm_service_stop(&s, 4) == PCM_OK && f.closes == 4);
    puts("PCM service tests passed");
}
