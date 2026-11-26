#define _GNU_SOURCE
#include "u6_tinyalsa_abi.h"
#include "pcm_service.h"

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    void *gate = dlopen("libtinyalsa.so", RTLD_NOW | RTLD_LOCAL);
    if (!gate) fprintf(stderr, "%s\n", dlerror());
    assert(gate);
    void *original = dlopen("libu6_alsa.so", RTLD_NOW | RTLD_LOCAL);
    assert(original);
    int (*reserve)(int) = dlsym(gate, "u6_alsa_board_lease");
    struct pcm *(*open_pcm)(unsigned int, unsigned int, unsigned int, struct pcm_config *) =
        dlsym(gate, "pcm_open");
    int (*is_ready)(struct pcm *) = dlsym(gate, "pcm_is_ready");
    int (*close_pcm)(struct pcm *) = dlsym(gate, "pcm_close");
    assert(reserve && open_pcm && is_ready && close_pcm);
    assert(dlsym(gate, "pcm_open") != dlsym(original, "pcm_open"));
    assert(dlsym(gate, "pcm_is_ready") == dlsym(original, "pcm_is_ready"));
    assert(dlsym(gate, "mixer_ctl_get_value") == dlsym(original, "mixer_ctl_get_value"));

    int fd = open(argv[1], O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    assert(fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0);
    assert(reserve(fd) == PCM_OK);
    assert(reserve(fd) == PCM_BUSY);
    assert(reserve(-1) == PCM_OK);

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(fd);
        struct pcm_config config = {.channels = 2, .rate = 48000, .period_size = 4096,
                                    .period_count = 4, .format = U6_PCM_FORMAT_S16_LE};
        struct pcm *refused = open_pcm(1, 0, U6_PCM_MMAP_NOIRQ, &config);
        assert(errno == EBUSY && refused && !is_ready(refused));
        close_pcm(refused);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(close(fd) == 0);
    puts("stock ALSA symbols and cross-process refusal verified");
}
