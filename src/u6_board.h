#ifndef U6_BOARD_H
#define U6_BOARD_H

#include "u6_alsa.h"
#include "u6_volume.h"
#include <stdbool.h>

struct mixer;
struct mixer_ctl;

struct u6_board {
    const char *lock_path;
    int lock_fd;
    int (*gate)(int fd);
    struct mixer *mixer;
    struct mixer_ctl *mute;
    struct mixer_ctl *work_mode;
    struct mixer_ctl *fs_mode;
    struct mixer_ctl *gain;
    struct mixer_ctl *gain_mode;
    int original_work, original_fs, original_gain_mode;
    int original_left, original_right;
    struct u6_dac_volume volume;
    bool volume_valid;
    bool active;
    bool playback_muted;
    bool high_fs_mode;
    bool have_snapshot;
    bool have_powered_snapshot;
    bool faulted;
};

#ifdef __cplusplus
extern "C" {
#endif

struct u6_board_controls u6_board_init(struct u6_board *, const char *lock_path,
                                       bool playback_muted);

int u6_board_set_volume(struct u6_board *board, struct u6_dac_volume volume);

#ifdef __cplusplus
}
#endif

#endif
