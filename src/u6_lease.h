#ifndef U6_LEASE_H
#define U6_LEASE_H

#include "pcm_service.h"

#ifndef U6_ALSA_LOCK_PATH
#define U6_ALSA_LOCK_PATH "/data/misc/u6_direct_audio/board.lock"
#endif

int u6_lease_take(const char *path, int *fd);
int u6_lease_validate(const char *path, int fd);

#endif
