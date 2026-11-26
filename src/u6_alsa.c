#include "u6_alsa.h"
#include "u6_tinyalsa_abi.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static FILE *open_proc(const char *root, const char *relative) {
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/%s", root, relative);
    if (length < 0 || (size_t)length >= sizeof(path)) return NULL;
    return fopen(path, "r");
}

bool u6_find_card(const char *root, unsigned int *card) {
    FILE *file = open_proc(root, "cards");
    if (!file) return false;
    bool found = false;
    char line[256], id[64];
    unsigned int number;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, " %u [%63[^] ]", &number, id) != 2 || strcmp(id, "accusdiocard"))
            continue;
        if (found) {
            found = false;
            break;
        }
        *card = number;
        found = true;
    }
    bool valid = found && !ferror(file);
    fclose(file);
    return valid;
}

bool u6_read_hw_params(const char *root, unsigned int card, struct pcm_format *actual) {
    char relative[64];
    snprintf(relative, sizeof(relative), "card%u/pcm0p/sub0/hw_params", card);
    FILE *file = open_proc(root, relative);
    if (!file) return false;

    bool known_format = false;
    bool mmap_access = false;
    char line[128], value[32];
    *actual = (struct pcm_format){0};
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "access: %31s", value) == 1) {
            mmap_access = strcmp(value, "MMAP_INTERLEAVED") == 0;
        } else if (sscanf(line, "format: %31s", value) == 1) {
            known_format = !strcmp(value, "S16_LE") || !strcmp(value, "S32_LE");
            actual->encoding = !strcmp(value, "S32_LE") ? PCM_S32 : PCM_S16;
        } else if (sscanf(line, "channels: %u", &actual->channels) != 1) {
            sscanf(line, "rate: %u", &actual->rate);
        }
    }
    bool complete = !ferror(file) && mmap_access && known_format && actual->channels && actual->rate;
    fclose(file);
    return complete;
}

static void close_output(void *context) {
    struct u6_alsa_backend *backend = context;
    backend->controls.silence(backend->controls.context);
    if (backend->handle) pcm_close(backend->handle);
    backend->handle = NULL;
    backend->frame_bytes = 0;
    backend->controls.release(backend->controls.context);
}

static int open_output(void *context, struct pcm_format requested, struct pcm_format *actual) {
    struct u6_alsa_backend *backend = context;
    struct u6_board_controls *controls = &backend->controls;
    if (!controls->prepare || !controls->activate || !controls->silence || !controls->release)
        return PCM_INVALID;
    if (requested.rate == 0 || requested.rate > 384000 || requested.channels != 2 ||
        (requested.encoding != PCM_S16 && requested.encoding != PCM_S32))
        return PCM_UNSUPPORTED;
    if (backend->handle) return PCM_BUSY;

    unsigned int card;
    if (!u6_find_card(backend->proc_asound, &card)) return PCM_IO;
    int status = controls->prepare(controls->context, card, requested.rate);
    if (status != PCM_OK) return status;

    struct pcm_config config = {
        .channels = requested.channels,
        .rate = requested.rate,
        .period_size = requested.rate <= 192000 ? 4096 : 32768,
        .period_count = 4,
        .format = requested.encoding == PCM_S16 ? U6_PCM_FORMAT_S16_LE : U6_PCM_FORMAT_S32_LE,
    };
    backend->handle = pcm_open(card, 0, U6_PCM_MMAP_NOIRQ, &config);
    if (!backend->handle || !pcm_is_ready(backend->handle) ||
        !u6_read_hw_params(backend->proc_asound, card, actual)) {
        status = PCM_IO;
    } else if (requested.rate != actual->rate || requested.channels != actual->channels ||
               requested.encoding != actual->encoding) {
        status = PCM_FORMAT_CHANGED;
    } else {
        if (controls->opened) status = controls->opened(controls->context);
        if (status == PCM_OK) status = controls->activate(controls->context);
    }
    if (status != PCM_OK) {
        close_output(backend);
        return status;
    }
    backend->frame_bytes = requested.channels * (requested.encoding == PCM_S16 ? 2 : 4);
    return PCM_OK;
}

static int write_output(void *context, const uint8_t *data, size_t frames) {
    struct u6_alsa_backend *backend = context;
    if (!backend->handle || !backend->frame_bytes || !data || frames == 0 ||
        frames > UINT_MAX / backend->frame_bytes) return PCM_INVALID;
    unsigned int bytes = (unsigned int)(frames * backend->frame_bytes);
    return pcm_mmap_write(backend->handle, data, bytes) == 0 ? PCM_OK : PCM_IO;
}

struct pcm_backend u6_alsa_init(struct u6_alsa_backend *backend, const char *proc_asound,
                               struct u6_board_controls controls) {
    *backend = (struct u6_alsa_backend){.proc_asound = proc_asound ? proc_asound : "/proc/asound",
                                      .controls = controls};
    return (struct pcm_backend){backend, open_output, write_output, close_output};
}
