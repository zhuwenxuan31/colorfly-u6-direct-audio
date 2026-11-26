#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "u6_lease.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int claim(int fd) {
    int result;
    do {
        result = flock(fd, LOCK_EX | LOCK_NB);
    } while (result != 0 && errno == EINTR);
    if (result == 0) return PCM_OK;
    return errno == EWOULDBLOCK || errno == EAGAIN ? PCM_BUSY : PCM_IO;
}

int u6_lease_take(const char *path, int *fd) {
    *fd = -1;
    if (!path) return PCM_IO;
    int candidate = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (candidate < 0) return PCM_IO;

    struct stat metadata;
    int status = fstat(candidate, &metadata) == 0 && S_ISREG(metadata.st_mode)
               ? claim(candidate) : PCM_IO;
    if (status == PCM_OK) *fd = candidate;
    else close(candidate);
    return status;
}

int u6_lease_validate(const char *path, int fd) {
    struct stat descriptor, expected;
    if (fstat(fd, &descriptor) != 0 || lstat(path, &expected) != 0 ||
        !S_ISREG(expected.st_mode) || descriptor.st_dev != expected.st_dev ||
        descriptor.st_ino != expected.st_ino)
        return PCM_IO;
    return claim(fd);
}
