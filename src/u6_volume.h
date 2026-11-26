#ifndef U6_VOLUME_H
#define U6_VOLUME_H

#include <stdbool.h>

enum { U6_OUT_SPEAKER = 2, U6_OUT_HEADSET = 4, U6_OUT_HEADPHONE = 8 };

struct u6_volume_state {
    int index;
    unsigned int device;
    bool lineout;
    float balance;
};

struct u6_dac_volume {
    int left, right;
    bool muted;
    bool lineout;
};

#ifdef __cplusplus
extern "C" {
#endif

bool u6_volume_resolve(const struct u6_volume_state *state, float left, float right,
                       struct u6_dac_volume *dac);

#ifdef __cplusplus
}
#endif

#endif
