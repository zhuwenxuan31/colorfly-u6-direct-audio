#include "u6_board.h"
#include "u6_tinyalsa_abi.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct mixer { int unused; };
struct mixer_ctl { int value; char name; };
static struct mixer device;
static struct mixer_ctl mute = {0, 'm'}, work = {1, 'w'}, fs = {0, 'f'};
static struct mixer_ctl gain = {0, 'g'}, mode = {1, 'a'};
static int channels[] = {37, 41};
static int fail_gain;
static unsigned int gain_reads;
static char changes[128];
static size_t changed;
static int fail_fs;
static int fail_work_restore;
static bool dac_powered;

struct mixer *mixer_open(unsigned int card) { assert(card == 3); return &device; }
void mixer_close(struct mixer *m) { assert(m == &device); }
struct mixer_ctl *mixer_get_ctl_by_name(struct mixer *m, const char *name) {
    assert(m == &device);
    if (!strcmp(name, "Es9038 Dac Mute")) return &mute;
    if (!strcmp(name, "Sdio Work Mode")) return &work;
    if (!strcmp(name, "Es9038 Fs Mode")) return &fs;
    if (!strcmp(name, "Es9038 Gain")) return &gain;
    if (!strcmp(name, "Audio Gain Mode")) return &mode;
    return NULL;
}
unsigned int mixer_ctl_get_num_values(struct mixer_ctl *ctl) { return ctl == &gain ? 2 : ctl != NULL; }
int mixer_ctl_get_value(struct mixer_ctl *ctl, unsigned int index) {
    if (ctl == &gain || ctl == &fs) assert(dac_powered);
    if (ctl == &gain) {
        ++gain_reads;
        assert(index < 2);
        return channels[index];
    }
    assert(index == 0);
    return ctl->value;
}
int mixer_ctl_set_value(struct mixer_ctl *ctl, unsigned int index, int value) {
    assert(index == 0);
    if (ctl == &fs) assert(dac_powered);
    if (ctl == &fs && fail_fs && value == 1) return -1;
    if (ctl == &work && fail_work_restore && value == 1) return -1;
    changes[changed++] = ctl->name;
    ctl->value = value;
    return 0;
}

int mixer_ctl_set_array(struct mixer_ctl *ctl, const void *values, size_t count) {
    assert(dac_powered && ctl == &gain && count == 2);
    if (fail_gain) return -1;
    const long *gain_values = values;
    channels[0] = (int)gain_values[0];
    channels[1] = (int)gain_values[1];
    changes[changed++] = 'g';
    return 0;
}

int main(int argc, char **argv) {
    assert(argc == 2 && chdir(argv[1]) == 0);
    int fd = open("board.lock", O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0 && close(fd) == 0);
    struct u6_board one, two;
    struct u6_board_controls a = u6_board_init(&one, "board.lock", false);
    struct u6_board_controls b = u6_board_init(&two, "board.lock", false);
    assert(a.prepare(a.context, 3, 384000) == PCM_OK);
    assert(changed == 2 && !memcmp(changes, "mw", 2));
    assert(mute.value == 1 && work.value == 0 && fs.value == 0);
    assert(b.prepare(b.context, 3, 192000) == PCM_BUSY);
    assert(u6_board_set_volume(&one, (struct u6_dac_volume){22, 35, false, false}) == PCM_OK);
    dac_powered = true;
    assert(a.opened(a.context) == PCM_OK);
    assert(mute.value == 1 && channels[0] == 22 && channels[1] == 35);
    assert(a.activate(a.context) == PCM_OK && mute.value == 0 && fs.value == 1);
    assert(channels[0] == 22 && channels[1] == 35);
    assert(changed == 5 && !memcmp(changes, "mwfgm", 5));
    unsigned int initial_gain_reads = gain_reads;
    assert(u6_board_set_volume(&one, (struct u6_dac_volume){22, 35, false, false}) == PCM_OK);
    assert(gain_reads == initial_gain_reads);
    assert(u6_board_set_volume(&one, (struct u6_dac_volume){255, 255, true, false}) == PCM_OK);
    assert(mute.value == 1 && channels[0] == 255);
    assert(u6_board_set_volume(&one, (struct u6_dac_volume){18, 18, false, true}) == PCM_IO);
    assert(mute.value == 1);
    mode.value = 2;
    assert(u6_board_set_volume(&one, (struct u6_dac_volume){24, 24, false, true}) == PCM_OK);
    assert(mute.value == 0 && channels[0] == 24);
    mode.value = 1;
    a.silence(a.context);
    assert(mute.value == 1);
    dac_powered = false;
    a.release(a.context);
    assert(mute.value == 1 && work.value == 1 && fs.value == 0);
    assert(channels[0] == 37 && channels[1] == 41);
    assert(one.lock_fd == -1 && b.prepare(b.context, 3, 192000) == PCM_OK);
    assert(fs.value == 0);
    b.release(b.context);

    assert(u6_board_set_volume(&one, (struct u6_dac_volume){28, 28, false, false}) == PCM_OK);
    assert(a.prepare(a.context, 3, 192000) == PCM_OK);
    fail_gain = 1;
    dac_powered = true;
    assert(a.opened(a.context) == PCM_IO && mute.value == 1);
    fail_gain = 0;
    a.silence(a.context);
    dac_powered = false;
    a.release(a.context);
    assert(mute.value == 1 && channels[0] == 37);

    fail_fs = 1;
    assert(u6_board_set_volume(&one, (struct u6_dac_volume){28, 28, false, false}) == PCM_OK);
    assert(a.prepare(a.context, 3, 384000) == PCM_OK);
    dac_powered = true;
    assert(a.opened(a.context) == PCM_IO && mute.value == 1);
    a.silence(a.context);
    dac_powered = false;
    a.release(a.context);
    assert(one.lock_fd == -1 && work.value == 1 && mute.value == 1);
    fail_fs = 0;
    assert(a.prepare(a.context, 3, 192000) == PCM_OK);
    assert(u6_board_set_volume(&one, (struct u6_dac_volume){35, 37, false, false}) == PCM_OK);
    size_t before_activate = changed;
    dac_powered = true;
    assert(a.opened(a.context) == PCM_OK);
    assert(a.activate(a.context) == PCM_OK);
    assert(changed - before_activate == 2 && !memcmp(changes + before_activate, "gm", 2));
    fail_work_restore = 1;
    a.silence(a.context);
    dac_powered = false;
    a.release(a.context);
    assert(one.faulted && mute.value == 1 && one.lock_fd == -1);
    assert(a.prepare(a.context, 3, 192000) == PCM_IO);
    puts("U6 board controls tests passed");
}
