#include <stdbool.h>
#include "direct_client.h"
#include <assert.h>
#include <string.h>

struct u6_direct_client { struct pcm_format format; };
static struct u6_direct_client endpoint;
static bool playing;
static bool position_unavailable;

void u6_mock_position_unavailable(bool value) { position_unavailable = value; }

int u6_direct_open(struct pcm_format format, struct u6_direct_client **result) {
    assert(!playing && format.channels == 2);
    assert((format.rate == 44100 && format.encoding == PCM_S16) ||
           (format.rate == 96000 && format.encoding == PCM_S32));
    endpoint.format = format;
    *result = &endpoint;
    return PCM_OK;
}
int u6_direct_set_volume(struct u6_direct_client *client, float left, float right) {
    assert(client == &endpoint && left == 1 && right == 1);
    return PCM_OK;
}
int u6_direct_start(struct u6_direct_client *client) {
    assert(client == &endpoint && !playing);
    playing = true;
    return PCM_OK;
}
int u6_direct_write(struct u6_direct_client *client, const void *data, size_t frames, size_t *accepted) {
    const uint32_t expected[] = {0x80000001, 0x7fffffff, 0xffaabbcc, 0x00000002};
    assert(client == &endpoint && playing);
    assert(frames * pcm_frame_bytes(endpoint.format) == sizeof(expected));
    assert(memcmp(data, expected, sizeof(expected)) == 0);
    *accepted = frames;
    return PCM_OK;
}
int u6_direct_position(struct u6_direct_client *client, uint64_t *frames, int64_t *time_ns) {
    assert(client == &endpoint);
    if (position_unavailable) return PCM_WOULD_BLOCK;
    *frames = 1;
    *time_ns = 2000001000;
    return PCM_OK;
}
int u6_direct_close(struct u6_direct_client *client) {
    assert(client == &endpoint && playing);
    playing = false;
    return PCM_OK;
}
