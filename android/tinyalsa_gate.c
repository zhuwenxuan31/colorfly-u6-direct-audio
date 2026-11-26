#include "u6_lease.h"
#include "u6_tinyalsa_abi.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

static struct {
    struct pcm *(*open)(unsigned int, unsigned int, unsigned int, struct pcm_config *);
    int (*close)(struct pcm *);
    int (*ready)(struct pcm *);
    const char *(*control_name)(struct mixer_ctl *);
    int (*set_value)(struct mixer_ctl *, unsigned int, int);
    int (*set_array)(struct mixer_ctl *, const void *, size_t);
    int (*set_percent)(struct mixer_ctl *, unsigned int, int);
    int (*set_enum)(struct mixer_ctl *, const char *);
} real;

static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static int lease_fd = -1;
static int board_fd = -1;
static struct pcm *leased_pcm;
static _Thread_local bool inside_percent;

static void resolve(void) {
    void *library = dlopen("libu6_alsa.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) return;
    real.open = dlsym(library, "pcm_open");
    real.close = dlsym(library, "pcm_close");
    real.ready = dlsym(library, "pcm_is_ready");
    real.control_name = dlsym(library, "mixer_ctl_get_name");
    real.set_value = dlsym(library, "mixer_ctl_set_value");
    real.set_array = dlsym(library, "mixer_ctl_set_array");
    real.set_percent = dlsym(library, "mixer_ctl_set_percent");
    real.set_enum = dlsym(library, "mixer_ctl_set_enum_by_string");
}

// The daemon opens the same lock before changing the board and owns the descriptor.
int u6_alsa_board_lease(int fd) {
    pthread_mutex_lock(&state_lock);
    int status = PCM_OK;
    if (fd == -1) {
        board_fd = -1;
    } else if (board_fd != -1 || lease_fd != -1) {
        status = PCM_BUSY;
    } else {
        status = u6_lease_validate(U6_ALSA_LOCK_PATH, fd);
        if (status == PCM_OK) board_fd = fd;
    }
    pthread_mutex_unlock(&state_lock);
    return status;
}

static bool board_owner(void) {
    return board_fd >= 0 && u6_lease_validate(U6_ALSA_LOCK_PATH, board_fd) == PCM_OK;
}

struct pcm *pcm_open(unsigned int card, unsigned int device, unsigned int flags,
                     struct pcm_config *config) {
    pthread_once(&resolve_once, resolve);
    if (!real.open || !real.close || !real.ready) return NULL;
    // tinyalsa PCM_IN is bit 28. The internal DAC is card 1, device 0, playback.
    if (card != 1 || device != 0 || (flags & 0x10000000u))
        return real.open(card, device, flags, config);

    pthread_mutex_lock(&state_lock);
    bool borrowed = board_owner();
    int fd;
    int status = borrowed ? PCM_OK : u6_lease_take(U6_ALSA_LOCK_PATH, &fd);
    if (!borrowed && status == PCM_OK) lease_fd = fd;
    pthread_mutex_unlock(&state_lock);
    if (status != PCM_OK) {
        // Ask the stock library for its normal non-null failed-open handle.
        struct pcm *failed = real.open(UINT_MAX, device, flags, config);
        errno = status == PCM_BUSY ? EBUSY : EACCES;
        return failed;
    }

    struct pcm_config direct_config;
    if (borrowed && config && config->rate == 192000 && config->period_size == 4096 &&
        config->period_count == 4) {
        // Preserve the startup buffer verified with Apple Music at 192 kHz.
        direct_config = *config;
        direct_config.period_count = 8;
        direct_config.start_threshold = 6 * direct_config.period_size;
        config = &direct_config;
    }
    struct pcm *pcm = real.open(card, device, flags, config);
    if (!borrowed) {
        pthread_mutex_lock(&state_lock);
        if (pcm && real.ready(pcm)) {
            leased_pcm = pcm;
        } else {
            close(fd);
            lease_fd = -1;
        }
        pthread_mutex_unlock(&state_lock);
    }
    return pcm;
}

int pcm_close(struct pcm *pcm) {
    pthread_once(&resolve_once, resolve);
    if (!real.close) return -ENOSYS;
    int result = real.close(pcm);
    pthread_mutex_lock(&state_lock);
    if (pcm && pcm == leased_pcm) {
        close(lease_fd);
        lease_fd = -1;
        leased_pcm = NULL;
    }
    pthread_mutex_unlock(&state_lock);
    return result;
}

static bool protected_control(const char *name) {
    if (!name) return false;
    // The original settings service still owns the user's analog gain switch.
    return !strncmp(name, "Es9038 ", 7) || !strcmp(name, "Sdio Work Mode");
}

static int control_lease(struct mixer_ctl *control, int *fd) {
    *fd = -1;
    if (!real.control_name) return -ENOSYS;
    if (!protected_control(control ? real.control_name(control) : NULL)) return 0;
    pthread_mutex_lock(&state_lock);
    int owner = board_owner() ? board_fd : lease_fd;
    int status;
    if (owner >= 0) {
        *fd = fcntl(owner, F_DUPFD_CLOEXEC, 0);
        status = *fd >= 0 ? PCM_OK : PCM_IO;
    } else {
        status = u6_lease_take(U6_ALSA_LOCK_PATH, fd);
    }
    pthread_mutex_unlock(&state_lock);
    if (status == PCM_OK) return 0;
    return status == PCM_BUSY ? -EBUSY : -EACCES;
}

int mixer_ctl_set_value(struct mixer_ctl *control, unsigned int index, int value) {
    pthread_once(&resolve_once, resolve);
    if (!real.set_value) return -ENOSYS;
    if (inside_percent) return real.set_value(control, index, value);
    int fd, status = control_lease(control, &fd);
    if (!status) status = real.set_value(control, index, value);
    if (fd >= 0) close(fd);
    return status;
}

int mixer_ctl_set_array(struct mixer_ctl *control, const void *values, size_t count) {
    pthread_once(&resolve_once, resolve);
    if (!real.set_array) return -ENOSYS;
    int fd, status = control_lease(control, &fd);
    if (!status) status = real.set_array(control, values, count);
    if (fd >= 0) close(fd);
    return status;
}

int mixer_ctl_set_percent(struct mixer_ctl *control, unsigned int index, int percent) {
    pthread_once(&resolve_once, resolve);
    if (!real.set_percent) return -ENOSYS;
    int fd, status = control_lease(control, &fd);
    if (!status) {
        // The original percent API calls its value writer through the PLT.
        inside_percent = true;
        status = real.set_percent(control, index, percent);
        inside_percent = false;
    }
    if (fd >= 0) close(fd);
    return status;
}

int mixer_ctl_set_enum_by_string(struct mixer_ctl *control, const char *value) {
    pthread_once(&resolve_once, resolve);
    if (!real.set_enum) return -ENOSYS;
    int fd, status = control_lease(control, &fd);
    if (!status) status = real.set_enum(control, value);
    if (fd >= 0) close(fd);
    return status;
}
