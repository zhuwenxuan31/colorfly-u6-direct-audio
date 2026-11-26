#include "apple_track.h"

bool u6_is_music_track(int usage, int content_type, unsigned int flags) {
    // Android 6 maps default and MEDIA attributes to MUSIC. Explicit call or
    // enforced-audibility flags take precedence over the usage.
    if (flags & ((1u << 0) | (1u << 2))) return false;
    return (usage == 0 || usage == 1) && (content_type == 0 || content_type == 2);
}

bool apple_track_format(int rate, int channels, int index_mask, int encoding,
                        struct pcm_format *format) {
    if (rate <= 0 || rate > 192000 || channels != 12 || index_mask != 0)
        return false;
    enum pcm_encoding pcm;
    switch (encoding) {
        case 2: pcm = PCM_S16; break;
        case 4: pcm = PCM_FLOAT; break;
        default: return false;
    }
    *format = (struct pcm_format){(uint32_t)rate, 2, pcm};
    return true;
}

int apple_track_write(struct u6_direct_client *client, const void *data, int units,
                      size_t frame_bytes, size_t unit_bytes) {
    if (!client) return -3;
    if (!data || units < 0 || !unit_bytes || !frame_bytes || frame_bytes % unit_bytes)
        return -2;
    size_t units_per_frame = frame_bytes / unit_bytes;
    size_t frames = (size_t)units / units_per_frame;
    if (frames == 0) return 0;

    size_t accepted = 0;
    int status = u6_direct_write(client, data, frames, &accepted);
    if (status == PCM_WOULD_BLOCK) return 0;
    if (status != PCM_OK || accepted > frames) return -6;
    return (int)(accepted * units_per_frame);
}
