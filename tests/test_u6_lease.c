#define _POSIX_C_SOURCE 200809L
#include "u6_lease.h"
#include <assert.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    assert(argc == 2 && chdir(argv[1]) == 0);
    int fd = open("pcm.lock", O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0 && close(fd) == 0);
    assert(symlink("pcm.lock", "linked.lock") == 0);
    assert(u6_lease_take("linked.lock", &fd) == PCM_IO);
    assert(u6_lease_take("pcm.lock", &fd) == PCM_OK);
    assert(u6_lease_validate("pcm.lock", fd) == PCM_OK);
    assert(u6_lease_validate("linked.lock", fd) == PCM_IO);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(fd);
        int other;
        assert(u6_lease_take("pcm.lock", &other) == PCM_BUSY);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(close(fd) == 0);
    assert(u6_lease_take("pcm.lock", &fd) == PCM_OK && close(fd) == 0);
    return 0;
}
