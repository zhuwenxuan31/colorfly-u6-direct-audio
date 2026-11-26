#ifndef U6_APPLE_TRACK_H
#define U6_APPLE_TRACK_H

#include <stdbool.h>

#include "direct_client.h"

#ifdef __cplusplus
extern "C" {
#endif

bool u6_is_music_track(int usage, int content_type, unsigned int flags);
bool apple_track_format(int rate, int channels, int index_mask, int encoding,
                        struct pcm_format *format);
int apple_track_write(struct u6_direct_client *client, const void *data, int units,
                      size_t frame_bytes, size_t unit_bytes);

#ifdef __cplusplus
}
#endif
#endif
