#include "u6_volume.h"
#include <math.h>

static int attenuation(float decibels) {
    if (decibels <= -127.5f) return 255;
    // The stock DAC LUT has one index per 0.5 dB; choose nearest amplitude.
    float exact = -2.0f * decibels;
    int lower = (int)exact;
    const float midpoint = 0.971160438f; // (1 + 10^(-0.5/20)) / 2
    return powf(10.0f, (decibels + lower * 0.5f) / 20.0f) < midpoint ? lower + 1 : lower;
}

static float media_db(int index, bool speaker) {
    // U6 2.0 libaudiopolicyenginedefault: MUSIC profile for headset and speaker.
    const int step[] = {1, 20, 60, 100};
    const int speaker_db[] = {-56, -34, -11, 0};
    const int headset_db[] = {-58, -40, -17, 0};
    const int *db = speaker ? speaker_db : headset_db;
    for (unsigned int i = 1; i < 4; ++i) {
        if (index <= step[i]) {
            return db[i - 1] + (float)(index - step[i - 1]) *
                   (db[i] - db[i - 1]) / (step[i] - step[i - 1]);
        }
    }
    return 0.0f;
}

static int channel(float base, float amplitude, float balance) {
    if (amplitude == 0.0f) return 255;
    int steps = attenuation(base + 20.0f * log10f(amplitude));
    int adjusted = (int)(steps * balance);
    return adjusted > 255 ? 255 : adjusted;
}

bool u6_volume_resolve(const struct u6_volume_state *state, float left, float right,
                       struct u6_dac_volume *dac) {
    if (state->index < 0 || state->index > 100 ||
        !(state->balance >= -1.0f && state->balance <= 1.0f) ||
        !(left >= 0.0f && left <= 1.0f && right >= 0.0f && right <= 1.0f) ||
        (state->device != U6_OUT_SPEAKER && state->device != U6_OUT_HEADPHONE &&
         state->device != U6_OUT_HEADSET))
        return false;
    dac->lineout = state->lineout;
    dac->muted = state->index == 0 || (left == 0.0f && right == 0.0f);
    float db = state->lineout ? -12.0f : media_db(state->index, state->device == U6_OUT_SPEAKER);
    float balance = state->lineout ? 0.0f : state->balance;
    dac->left = dac->muted ? 255 : channel(db, left, 1.0f + balance);
    dac->right = dac->muted ? 255 : channel(db, right, 1.0f - balance);
    return true;
}
