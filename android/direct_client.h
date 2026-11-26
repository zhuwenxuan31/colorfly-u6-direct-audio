#ifndef U6_DIRECT_CLIENT_H
#define U6_DIRECT_CLIENT_H

#include "pcm_service.h"

#ifdef __cplusplus
extern "C" {
#endif

struct u6_direct_client;

/* A single native playback worker owns a client. Stop/destroy it before
 * restarting or seeking and obtain a new mapping with open. */
int u6_direct_open(struct pcm_format format, struct u6_direct_client **result);
int u6_direct_set_volume(struct u6_direct_client *client, float left, float right);
int u6_direct_start(struct u6_direct_client *client);
int u6_direct_write(struct u6_direct_client *client, const void *data, size_t frames,
                    size_t *accepted);
int u6_direct_position(struct u6_direct_client *client, uint64_t *presented, int64_t *time_ns);
int u6_direct_close(struct u6_direct_client *client);

#ifdef __cplusplus
}
#endif
#endif
