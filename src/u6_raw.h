#ifndef U6_RAW_H
#define U6_RAW_H

#include "u6_alsa.h"

struct u6_raw_api {
    void *(*init)(void *format, void *context, void (*event)(void *, void *));
    int (*start)(void *context);
    int (*stop)(void *context);
    void (*release)(void *context);
};

struct u6_raw {
    const char *proc_asound;
    struct u6_raw_api api;
    struct u6_board_controls controls;
    void *context;
    struct pcm *pcm;
    unsigned int frame_bytes;
    int started;
    int activated;
    uint64_t last_presented;
    int64_t last_position_ns;
};

#ifdef __cplusplus
extern "C" {
#endif
size_t u6_raw_pcm_offset(void);
bool u6_raw_running(const struct u6_raw *raw);
int u6_raw_position(struct u6_raw *raw, uint64_t written, uint64_t *presented, int64_t *time_ns);
struct pcm_backend u6_raw_init(struct u6_raw *raw, const char *proc_asound,
                               struct u6_raw_api api, struct u6_board_controls controls);
#ifdef __cplusplus
}
#endif
#endif
