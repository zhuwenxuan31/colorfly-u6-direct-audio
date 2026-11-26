#include "u6_volume.h"
#include <assert.h>

int main(void) {
    struct u6_volume_state state = {60, U6_OUT_SPEAKER, false, 0.0f};
    struct u6_dac_volume dac;
    assert(u6_volume_resolve(&state, 1.0f, 1.0f, &dac));
    assert(dac.left == 22 && dac.right == 22 && !dac.muted);
    state.device = U6_OUT_HEADPHONE;
    assert(u6_volume_resolve(&state, 1.0f, 0.0f, &dac));
    assert(dac.left == 34 && dac.right == 255 && !dac.muted);
    state.balance = 0.5f;
    assert(u6_volume_resolve(&state, 1.0f, 0.5f, &dac));
    assert(dac.left == 51 && dac.right == 23);
    state.index = 0;
    assert(u6_volume_resolve(&state, 1.0f, 1.0f, &dac) && dac.muted);
    state.index = 50;
    state.lineout = true;
    assert(u6_volume_resolve(&state, 1.0f, 1.0f, &dac));
    assert(dac.left == 24 && dac.right == 24 && !dac.muted);
    assert(u6_volume_resolve(&state, 0.0f, 0.0f, &dac) && dac.muted);
    state.device = 0x4000;
    assert(!u6_volume_resolve(&state, 1.0f, 1.0f, &dac));
}
