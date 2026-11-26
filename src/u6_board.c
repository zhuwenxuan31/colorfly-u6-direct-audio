#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "u6_board.h"
#include "u6_tinyalsa_abi.h"
#include "u6_lease.h"

#include <unistd.h>

static int acquire(struct u6_board *board) {
    if (board->faulted) return PCM_IO;
    if (board->lock_fd >= 0) return PCM_BUSY;
    int status = u6_lease_take(board->lock_path, &board->lock_fd);
    if (status == PCM_OK && board->gate) {
        status = board->gate(board->lock_fd);
        if (status != PCM_OK) {
            close(board->lock_fd);
            board->lock_fd = -1;
        }
    }
    return status;
}

static struct mixer_ctl *control(struct mixer *mixer, const char *name, unsigned int values) {
    struct mixer_ctl *ctl = mixer_get_ctl_by_name(mixer, name);
    return ctl && mixer_ctl_get_num_values(ctl) == values ? ctl : NULL;
}

static bool set_value(struct mixer_ctl *ctl, int value) {
    return mixer_ctl_set_value(ctl, 0, value) == 0 && mixer_ctl_get_value(ctl, 0) == value;
}

static bool gain_valid(int index) { return index >= 0 && index <= 255; }

static bool mode_valid(int mode) { return mode >= 0 && mode <= 2; }

static bool set_gain(struct mixer_ctl *ctl, int left, int right) {
    if (mixer_ctl_get_value(ctl, 0) == left && mixer_ctl_get_value(ctl, 1) == right)
        return true;
    // The stock tinyalsa integer-array ABI follows ALSA's native long width.
    const long values[] = {left, right};
    return mixer_ctl_set_array(ctl, values, 2) == 0 &&
           mixer_ctl_get_value(ctl, 0) == left && mixer_ctl_get_value(ctl, 1) == right;
}

static bool set_playback_mute(struct u6_board *board, int value) {
    return mixer_ctl_get_value(board->mute, 0) == value || set_value(board->mute, value);
}

static int apply_volume(struct u6_board *board, bool unchanged) {
    const struct u6_dac_volume *volume = &board->volume;
    int mode = mixer_ctl_get_value(board->gain_mode, 0);
    if (!mode_valid(mode) || volume->lineout != (mode == 2)) {
        if (!set_value(board->mute, 1)) board->faulted = true;
        return PCM_IO;
    }
    if (unchanged) return PCM_OK;
    bool muted = volume->muted || board->playback_muted;
    if ((!muted || set_playback_mute(board, 1)) &&
        set_gain(board->gain, volume->left, volume->right) &&
        (muted || set_playback_mute(board, 0)))
        return PCM_OK;
    if (!set_value(board->mute, 1)) board->faulted = true;
    return PCM_IO;
}

int u6_board_set_volume(struct u6_board *board, struct u6_dac_volume volume) {
    if (!gain_valid(volume.left) || !gain_valid(volume.right)) return PCM_INVALID;
    bool unchanged = board->volume_valid && board->volume.left == volume.left &&
                     board->volume.right == volume.right && board->volume.muted == volume.muted &&
                     board->volume.lineout == volume.lineout;
    board->volume = volume;
    int status = board->active ? apply_volume(board, unchanged) : PCM_OK;
    board->volume_valid = status == PCM_OK;
    return status;
}

static void release(void *context) {
    struct u6_board *board = context;
    if (board->have_snapshot && mixer_ctl_get_value(board->work_mode, 0) != board->original_work &&
        !set_value(board->work_mode, board->original_work))
        board->faulted = true;
    if (board->mixer) mixer_close(board->mixer);
    if (board->lock_fd >= 0) {
        if (board->gate) board->gate(-1);
        close(board->lock_fd);
    }
    board->lock_fd = -1;
    board->mixer = NULL;
    board->mute = NULL;
    board->work_mode = NULL;
    board->fs_mode = NULL;
    board->gain = NULL;
    board->gain_mode = NULL;
    board->active = false;
    board->volume_valid = false;
    board->have_snapshot = false;
    board->have_powered_snapshot = false;
}

static bool snapshot(struct u6_board *board, unsigned int card) {
    board->mixer = mixer_open(card);
    if (!board->mixer) return false;
    board->mute = control(board->mixer, "Es9038 Dac Mute", 1);
    board->work_mode = control(board->mixer, "Sdio Work Mode", 1);
    board->fs_mode = control(board->mixer, "Es9038 Fs Mode", 1);
    board->gain = control(board->mixer, "Es9038 Gain", 2);
    board->gain_mode = control(board->mixer, "Audio Gain Mode", 1);
    if (!board->mute || !board->work_mode || !board->fs_mode || !board->gain || !board->gain_mode)
        return false;
    board->original_work = mixer_ctl_get_value(board->work_mode, 0);
    return board->original_work >= 0;
}

static int opened(void *context) {
    struct u6_board *board = context;
    if (!board->have_snapshot) return PCM_IO;
    board->original_fs = mixer_ctl_get_value(board->fs_mode, 0);
    board->original_left = mixer_ctl_get_value(board->gain, 0);
    board->original_right = mixer_ctl_get_value(board->gain, 1);
    board->original_gain_mode = mixer_ctl_get_value(board->gain_mode, 0);
    board->have_powered_snapshot = board->original_fs >= 0 && gain_valid(board->original_left) &&
        gain_valid(board->original_right) && mode_valid(board->original_gain_mode);
    if (!board->have_powered_snapshot || !board->volume_valid ||
        board->volume.lineout != (board->original_gain_mode == 2))
        return PCM_IO;
    if (board->original_fs != board->high_fs_mode && !set_value(board->fs_mode, board->high_fs_mode))
        return PCM_IO;
    // The driver can release hardware mute shortly after PCM START.
    return set_gain(board->gain, board->volume.left, board->volume.right) ? PCM_OK : PCM_IO;
}

static int prepare(void *context, unsigned int card, unsigned int rate) {
    struct u6_board *board = context;
    int status = acquire(board);
    if (status != PCM_OK) return status;
    board->have_snapshot = snapshot(board, card);
    if (!board->have_snapshot || !set_value(board->mute, 1) ||
        !set_value(board->work_mode, 0)) {
        release(board);
        return PCM_IO;
    }
    board->high_fs_mode = rate > 192000;
    return PCM_OK;
}

static int activate(void *context) {
    struct u6_board *board = context;
    if (!board->have_powered_snapshot || !board->volume_valid) return PCM_IO;
    int status = apply_volume(board, true);
    if (status == PCM_OK && !set_playback_mute(board, board->volume.muted || board->playback_muted))
        status = PCM_IO;
    board->active = status == PCM_OK;
    return status;
}

static void silence(void *context) {
    struct u6_board *board = context;
    if (!board->have_snapshot) return;
    if (!set_playback_mute(board, 1)) {
        board->faulted = true;
        return;
    }
    if (!board->have_powered_snapshot) return;

    // Restore ES9038 registers while the PCM handle still powers the DAC.
    int mode = mixer_ctl_get_value(board->gain_mode, 0);
    bool fs_restored = mixer_ctl_get_value(board->fs_mode, 0) == board->original_fs ||
                       set_value(board->fs_mode, board->original_fs);
    // devsettings owns the analog mode and may have changed it during playback.
    bool gain_restored = mode_valid(mode) && (mode != board->original_gain_mode ||
        set_gain(board->gain, board->original_left, board->original_right));
    if (!fs_restored || !gain_restored) board->faulted = true;
    board->have_powered_snapshot = false;
    board->active = false;
}

struct u6_board_controls u6_board_init(struct u6_board *board, const char *lock_path,
                                       bool playback_muted) {
    *board = (struct u6_board){.lock_path = lock_path, .lock_fd = -1,
                               .playback_muted = playback_muted};
    return (struct u6_board_controls){board, prepare, activate, silence, release, opened};
}
