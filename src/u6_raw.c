#define _POSIX_C_SOURCE 200809L
#include "u6_raw.h"
#include "u6_tinyalsa_abi.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <errno.h>
#endif
#include <limits.h>
#include <string.h>
#include <time.h>

/* Verified against both U6 2.0 libplayer.so builds. */
size_t u6_raw_pcm_offset(void) {
    return sizeof(void *) == 8 ? 0xa0 : 0x70;
}

static void raw_event(void *context, void *event) {
    /* The upstream decoder is separate. The PCM manager handles its own
     * open/close results and invalidates the session on errors. */
    (void)context;
    (void)event;
}

static void close_raw(void *context) {
    struct u6_raw *raw = context;
    raw->controls.silence(raw->controls.context);
    if (raw->context) {
        if (raw->started) {
            raw->api.stop(raw->context);
        } else if (raw->pcm) {
            /* The vendor can leave a failed-open handle with state stopped. */
            pcm_close(raw->pcm);
            struct pcm *none = NULL;
            memcpy((uint8_t *)raw->context + u6_raw_pcm_offset(), &none, sizeof(none));
        }
        raw->api.release(raw->context);
    }
    raw->context = NULL;
    raw->pcm = NULL;
    raw->frame_bytes = 0;
    raw->started = 0;
    raw->activated = 0;
    raw->last_presented = 0;
    raw->last_position_ns = 0;
    raw->controls.release(raw->controls.context);
}


static int open_raw(void *context, struct pcm_format requested, struct pcm_format *actual) {
    struct u6_raw *raw = context;
    if (raw->context) return PCM_BUSY;
    if (!raw->api.init || !raw->api.start || !raw->api.stop || !raw->api.release ||
        !raw->controls.prepare || !raw->controls.activate ||
        !raw->controls.silence || !raw->controls.release)
        return PCM_IO;
    if (requested.rate == 0 || requested.rate > 192000 || requested.channels != 2 ||
        (requested.encoding != PCM_S16 && requested.encoding != PCM_S32))
        return PCM_UNSUPPORTED;
    unsigned int card;
    if (!u6_find_card(raw->proc_asound, &card) || card != 1) return PCM_IO;

    int status = raw->controls.prepare(raw->controls.context, card, requested.rate);
    if (status != PCM_OK) return status;
    uint32_t bits = requested.encoding == PCM_S16 ? 16 : 32;
    uint32_t vendor_format[6] = {0, 1, requested.channels, requested.rate, bits, 0};
    raw->context = raw->api.init(vendor_format, raw, raw_event);
    if (!raw->context) {
        close_raw(raw);
        return PCM_IO;
    }
    raw->started = raw->api.start(raw->context) == 0;
    memcpy(&raw->pcm, (uint8_t *)raw->context + u6_raw_pcm_offset(), sizeof(raw->pcm));
    if (!raw->started || !raw->pcm || !pcm_is_ready(raw->pcm) ||
        !u6_read_hw_params(raw->proc_asound, card, actual)) {
        status = PCM_IO;
    } else if (requested.rate != actual->rate || requested.channels != actual->channels ||
               requested.encoding != actual->encoding) {
        status = PCM_FORMAT_CHANGED;
    } else if (raw->controls.opened) {
        status = raw->controls.opened(raw->controls.context);
    }
    if (status != PCM_OK) {
        close_raw(raw);
        return status;
    }
    raw->frame_bytes = requested.channels * (bits / 8);
    return PCM_OK;
}

bool u6_raw_running(const struct u6_raw *raw) {
    unsigned int available;
    struct timespec stamp;
    return raw && raw->started && raw->pcm &&
           pcm_get_htimestamp(raw->pcm, &available, &stamp) == 0;
}

static int64_t to_nanoseconds(struct timespec ts);


static int write_raw(void *context, const uint8_t *data, size_t frames) {
    struct u6_raw *raw = context;
    if (!raw->started || !raw->pcm || !raw->frame_bytes || !data || !frames ||
        frames > UINT_MAX / raw->frame_bytes)
        return PCM_INVALID;
    /* The stock byte-count wrapper can mask tinyalsa write errors. */
    unsigned int bytes = (unsigned int)(frames * raw->frame_bytes);
#ifdef __ANDROID__
    struct timespec call_start = {0};
    clock_gettime(CLOCK_MONOTONIC, &call_start);
#endif
    int result = pcm_mmap_write(raw->pcm, data, bytes);
    if (result != 0) {
#ifdef __ANDROID__
        int error = errno;
        struct timespec call_end = {0};
        clock_gettime(CLOCK_MONOTONIC, &call_end);
        int64_t elapsed = to_nanoseconds(call_end) - to_nanoseconds(call_start);
        __android_log_print(ANDROID_LOG_ERROR, "U6Direct",
                            "PCM write failed: result=%d errno=%d frames=%zu write_us=%lld",
                            result, error, frames, (long long)(elapsed / 1000));
#endif
        return PCM_IO;
    }
    // The ES9038 driver refuses to unmute before the stream is running.
    if (!raw->activated && u6_raw_running(raw)) {
#ifdef __ANDROID__
        struct timespec before = {0}, after = {0};
        clock_gettime(CLOCK_MONOTONIC, &before);
#endif
        int status = raw->controls.activate(raw->controls.context);
#ifdef __ANDROID__
        clock_gettime(CLOCK_MONOTONIC, &after);
        int64_t duration = to_nanoseconds(after) - to_nanoseconds(before);
        __android_log_print(ANDROID_LOG_INFO, "U6Direct", "DAC activation: result=%d duration_us=%lld running=%d",
                            status, (long long)(duration / 1000), u6_raw_running(raw));
#endif
        if (status != PCM_OK) return status;
        raw->activated = 1;
    }
    return PCM_OK;
}

static int64_t to_nanoseconds(struct timespec ts) {
    const int64_t ns_per_second = 1000000000;
    int64_t seconds = (int64_t)ts.tv_sec;
    if (seconds < 0 || seconds > (INT64_MAX - ns_per_second) / ns_per_second ||
        ts.tv_nsec < 0 || ts.tv_nsec >= ns_per_second)
        return -1;
    return seconds * ns_per_second + ts.tv_nsec;
}

static int64_t monotonic_timestamp(struct timespec hardware) {
    const int64_t max_age_ns = 1000000000;
    struct timespec mono, real;
    if (clock_gettime(CLOCK_MONOTONIC, &mono) || clock_gettime(CLOCK_REALTIME, &real))
        return -1;
    int64_t stamp = to_nanoseconds(hardware);
    int64_t monotonic_now = to_nanoseconds(mono);
    int64_t realtime_now = to_nanoseconds(real);
    if (stamp <= 0 || monotonic_now <= 0 || realtime_now <= 0) return -1;
    if (stamp <= monotonic_now && monotonic_now - stamp <= max_age_ns) return stamp;
    if (stamp > realtime_now || realtime_now - stamp > max_age_ns) return -1;
    return monotonic_now - (realtime_now - stamp);
}

int u6_raw_position(struct u6_raw *raw, uint64_t written, uint64_t *presented, int64_t *time_ns) {
    if (!raw || !presented || !time_ns || !raw->started || !raw->pcm) return PCM_INVALID;
    struct timespec hardware;
    unsigned int available;
    if (pcm_get_htimestamp(raw->pcm, &available, &hardware) != 0) return PCM_WOULD_BLOCK;
    unsigned int buffer = pcm_get_buffer_size(raw->pcm);
    if (buffer == 0 || available > buffer || buffer - available > written)
        return PCM_WOULD_BLOCK;
    uint64_t played = written - (buffer - available);
    int64_t now = monotonic_timestamp(hardware);
    if (now <= 0 || played < raw->last_presented || now < raw->last_position_ns)
        return PCM_WOULD_BLOCK;

    raw->last_presented = played;
    raw->last_position_ns = now;
    *presented = played;
    *time_ns = now;
    return PCM_OK;
}

struct pcm_backend u6_raw_init(struct u6_raw *raw, const char *proc_asound,
                               struct u6_raw_api api, struct u6_board_controls controls) {
    *raw = (struct u6_raw){.proc_asound = proc_asound ? proc_asound : "/proc/asound",
                          .api = api, .controls = controls};
    return (struct pcm_backend){raw, open_raw, write_raw, close_raw};
}
