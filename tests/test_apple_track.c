#include "apple_track.h"

#include <assert.h>
#include <string.h>

struct u6_direct_client { int dummy; };
static struct u6_direct_client endpoint;
static int write_status = PCM_OK;
static size_t written_frames;

int u6_direct_write(struct u6_direct_client *client, const void *data, size_t frames,
                    size_t *accepted) {
    assert(client == &endpoint);
    assert(data);
    written_frames = frames;
    *accepted = frames;
    return write_status;
}

int main(void) {
    struct pcm_format format;
    assert(u6_is_music_track(1, 0, 0));
    assert(u6_is_music_track(0, 0, 0));
    assert(u6_is_music_track(1, 2, 0));
    assert(u6_is_music_track(0, 2, 0));
    assert(!u6_is_music_track(1, 3, 0));
    assert(!u6_is_music_track(1, 1, 0));
    assert(!u6_is_music_track(14, 2, 0));
    assert(!u6_is_music_track(5, 0, 0));
    assert(!u6_is_music_track(0, 0, 1));
    assert(!u6_is_music_track(0, 0, 4));
    assert(apple_track_format(48000, 12, 0, 4, &format));
    assert(format.rate == 48000 && format.encoding == PCM_FLOAT);
    assert(apple_track_format(192000, 12, 0, 4, &format));
    assert(format.rate == 192000 && format.encoding == PCM_FLOAT);
    assert(apple_track_format(44100, 12, 0, 2, &format));
    assert(format.encoding == PCM_S16);
    assert(!apple_track_format(48000, 4, 0, 4, &format));
    assert(!apple_track_format(48000, 12, 1, 4, &format));
    assert(!apple_track_format(48000, 12, 0, 7, &format));

    unsigned char data[24] = {0};
    assert(apple_track_write(&endpoint, data, 24, 8, 1) == 24);
    assert(written_frames == 3);
    assert(apple_track_write(&endpoint, data, 6, 8, 4) == 6);
    assert(apple_track_write(&endpoint, data, 5, 8, 4) == 4);
    assert(apple_track_write(&endpoint, data, 1, 8, 4) == 0);
    write_status = PCM_IO;
    assert(apple_track_write(&endpoint, data, 24, 8, 1) == -6);
    return 0;
}
