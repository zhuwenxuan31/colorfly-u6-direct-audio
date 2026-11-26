#ifndef U6_ALSA_H
#define U6_ALSA_H

#include "pcm_service.h"
#include <stdbool.h>

struct pcm;

/* prepare acquires ownership and leaves the output muted. It unwinds on error.
 * opened reads powered registers after PCM negotiation.
 * silence restores those registers under mute before closing the device. */
struct u6_board_controls {
    void *context;
    int (*prepare)(void *context, unsigned int card, unsigned int rate);
    int (*activate)(void *context);
    void (*silence)(void *context);
    void (*release)(void *context);
    int (*opened)(void *context);
};

struct u6_alsa_backend {
    const char *proc_asound;
    struct u6_board_controls controls;
    struct pcm *handle;
    unsigned int frame_bytes;
};

#ifdef __cplusplus
extern "C" {
#endif

bool u6_find_card(const char *root, unsigned int *card);
bool u6_read_hw_params(const char *root, unsigned int card, struct pcm_format *actual);
struct pcm_backend u6_alsa_init(struct u6_alsa_backend *backend, const char *proc_asound,
                               struct u6_board_controls controls);

#ifdef __cplusplus
}
#endif

#endif
