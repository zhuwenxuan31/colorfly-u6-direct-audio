#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct overlay_patch {
    size_t offset, size;
    const unsigned char *data;
};

struct overlay_file {
    const char *source;
    const char *target;
    size_t length;
    mode_t mode;
    unsigned char sha256[32];
    const char *stock;
    unsigned char stock_sha256[32];
    const struct overlay_patch *patches;
    size_t patch_count;
};
#include "overlay_manifest.h"

static unsigned char *(*sha256)(const unsigned char *, size_t, unsigned char *);

static bool transfer(int fd, unsigned char *data, size_t length, bool writing) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = writing ? write(fd, data + offset, length - offset)
                               : read(fd, data + offset, length - offset);
        if (count <= 0) return false;
        offset += (size_t)count;
    }
    return true;
}

static bool copy_verified(int source, int stage, const struct overlay_file *file) {
    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    int input = file->stock ? open(file->stock, flags) : openat(source, file->source, flags);
    if (input < 0) return false;
    struct stat metadata;
    unsigned char *bytes = malloc(file->length);
    unsigned char digest[32];
    bool valid = bytes && fstat(input, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
                 metadata.st_size == (off_t)file->length &&
                 transfer(input, bytes, file->length, false);
    if (valid && file->stock)
        valid = sha256(bytes, file->length, digest) &&
                memcmp(digest, file->stock_sha256, sizeof(digest)) == 0;
    for (size_t i = 0; valid && i < file->patch_count; ++i) {
        const struct overlay_patch *patch = &file->patches[i];
        valid = patch->offset <= file->length && patch->size <= file->length - patch->offset;
        if (valid) memcpy(bytes + patch->offset, patch->data, patch->size);
    }
    if (valid)
        valid = sha256(bytes, file->length, digest) &&
                memcmp(digest, file->sha256, sizeof(digest)) == 0;
    close(input);
    if (valid) {
        int output = openat(stage, file->target,
                            O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        valid = output >= 0 && transfer(output, bytes, file->length, true) &&
                fchmod(output, file->mode) == 0;
        if (output >= 0 && close(output) != 0) valid = false;
    }
    free(bytes);
    return valid;
}

static void discard(int stage, const char *workspace) {
    if (stage >= 0) {
        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
            unlinkat(stage, files[i].target, 0);
        for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
            unlinkat(stage, dirs[i], AT_REMOVEDIR);
    }
    rmdir(workspace);
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    umask(022);

    void *crypto = dlopen("libcrypto.so", RTLD_NOW | RTLD_LOCAL);
    if (!crypto || !(sha256 = dlsym(crypto, "SHA256"))) return 1;

    int source = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (source < 0) return 1;
    struct stat existing;
    if (lstat(argv[2], &existing) == 0) {
        close(source);
        return 1;
    }

    char *workspace = NULL;
    if (asprintf(&workspace, "%s.new", argv[2]) < 0 || mkdir(workspace, 0700) != 0) {
        free(workspace);
        close(source);
        return 1;
    }
    int stage = open(workspace, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    bool verified = stage >= 0;
    for (size_t i = 0; verified && i < sizeof(dirs) / sizeof(dirs[0]); ++i)
        verified = mkdirat(stage, dirs[i], 0755) == 0;
    for (size_t i = 0; verified && i < sizeof(files) / sizeof(files[0]); ++i) {
        verified = copy_verified(source, stage, &files[i]);
        if (!verified) fprintf(stderr, "u6 overlay rejected %s\n", files[i].source);
    }
    bool published = verified && fchmod(stage, 0755) == 0 && rename(workspace, argv[2]) == 0;
    if (!published) discard(stage, workspace);
    if (stage >= 0) close(stage);
    close(source);
    free(workspace);
    return published ? 0 : 1;
}
