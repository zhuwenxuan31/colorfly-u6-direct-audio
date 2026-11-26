#define _POSIX_C_SOURCE 200809L
#include "u6_raw.h"
#include "u6_tinyalsa_abi.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct pcm { int ready; };
static struct pcm device = {1};
static union { void *align; unsigned char data[0xb0]; } stock;
static int starts, stops, releases, silence_calls, activates, direct_error, failures, start_error;
static size_t bytes_written;
static uint32_t stock_request[6];
static unsigned int pcm_available = 4096;
static int timestamp_error;
static clockid_t timestamp_clock = CLOCK_REALTIME;
unsigned int pcm_get_buffer_size(struct pcm *pcm) { assert(pcm == &device); return 4096; }
int pcm_get_htimestamp(struct pcm *pcm, unsigned int *available, struct timespec *stamp) {
    assert(pcm == &device);
    if (timestamp_error) return -1;
    *available = pcm_available;
    return clock_gettime(timestamp_clock, stamp);
}


struct pcm *pcm_open(unsigned int card, unsigned int node, unsigned int flags, struct pcm_config *conf) {
    (void)card; (void)node; (void)flags; (void)conf;
    assert(0 && "raw adapter must use stock start, not open its own output");
    return NULL;
}
int pcm_is_ready(struct pcm *pcm) { return pcm == &device; }
int pcm_close(struct pcm *pcm) { assert(pcm == &device); failures++; return 0; }
int pcm_mmap_write(struct pcm *pcm, const void *data, unsigned int count) {
    assert(pcm == &device && data);
    if (direct_error) return -1;
    bytes_written += count;
    return 0;
}

static void *init(void *format, void *owner, void (*callback)(void *, void *)) {
    memcpy(stock_request, format, sizeof(stock_request));
    assert(owner && callback);
    memset(stock.data, 0, sizeof(stock.data));
    return stock.data;
}
static int start(void *ctx) {
    assert(ctx == stock.data);
    struct pcm *handle = &device;
    memcpy(stock.data + u6_raw_pcm_offset(), &handle, sizeof(handle));
    starts++;
    return start_error;
}
static int stop(void *ctx) { assert(ctx == stock.data); stops++; return 0; }
static void drop(void *ctx) { assert(ctx == stock.data); releases++; }
static int prepare(void *ctx, unsigned int card, unsigned int rate) {
    assert(!ctx && card == 1 && rate == 192000);
    return PCM_OK;
}
static int activate(void *ctx) { assert(!ctx); activates++; return PCM_OK; }
static void silence(void *ctx) { assert(!ctx); silence_calls++; }
static void release(void *ctx) { assert(!ctx); }

static void put(const char *filename, const char *data) {
    FILE *f = fopen(filename, "w");
    assert(f && fputs(data, f) >= 0 && fclose(f) == 0);
}

int main(int argc, char **argv) {
    assert(argc == 2 && chdir(argv[1]) == 0);
    assert(mkdir("card1", 0700) == 0 && mkdir("card1/pcm0p", 0700) == 0 &&
           mkdir("card1/pcm0p/sub0", 0700) == 0);
    put("cards", " 1 [accusdiocard  ]: accu_sdio_card\n");
    const char *params = "card1/pcm0p/sub0/hw_params";
    struct u6_raw raw;
    struct pcm_service service;
    struct u6_raw_api api = {init, start, stop, drop};
    struct u6_board_controls controls = {NULL, prepare, activate, silence, release, NULL};
    pcm_service_init(&service, u6_raw_init(&raw, ".", api, controls));

    put(params, "access: MMAP_INTERLEAVED\nformat: S32_LE\nchannels: 2\nrate: 48000 (48000/1)\n");
    assert(pcm_service_start(&service, 1, (struct pcm_format){192000, 2, PCM_FLOAT}) == PCM_FORMAT_CHANGED);
    assert(starts == 1 && stops == 1 && releases == 1 && activates == 0 && silence_calls == 1);
    put(params, "access: MMAP_INTERLEAVED\nformat: S32_LE\nchannels: 2\nrate: 192000 (192000/1)\n");
    assert(pcm_service_start(&service, 2, (struct pcm_format){192000, 2, PCM_FLOAT}) == PCM_OK);
    assert(stock_request[1] == 1 && stock_request[2] == 2 &&
           stock_request[3] == 192000 && stock_request[4] == 32);
    assert(activates == 0);
    const float stereo[] = {0.5f, -0.5f};
    timestamp_error = 1;
    assert(pcm_push(&service, 2, stereo, sizeof(stereo)) == PCM_OK && bytes_written == 8);
    assert(activates == 0);
    timestamp_error = 0;
    assert(pcm_push(&service, 2, stereo, sizeof(stereo)) == PCM_OK && bytes_written == 16);
    assert(activates == 1);
    uint64_t played = UINT64_MAX;
    int64_t time_ns = -1;
    pcm_available = 4095;
    assert(u6_raw_position(&raw, 1, &played, &time_ns) == PCM_OK);
    assert(played == 0 && time_ns > 0);
    struct timespec mono;
    assert(clock_gettime(CLOCK_MONOTONIC, &mono) == 0);
    int64_t now_ns = (int64_t)mono.tv_sec * 1000000000 + mono.tv_nsec;
    assert(time_ns <= now_ns && now_ns - time_ns < 1000000000);
    pcm_available = 4096;
    timestamp_clock = CLOCK_MONOTONIC;
    assert(u6_raw_position(&raw, 1, &played, &time_ns) == PCM_OK && played == 1);
    pcm_available = 4094;
    assert(u6_raw_position(&raw, 1, &played, &time_ns) == PCM_WOULD_BLOCK);
    timestamp_error = 1;
    assert(u6_raw_position(&raw, 1, &played, &time_ns) == PCM_WOULD_BLOCK);
    direct_error = 1;
    assert(pcm_push(&service, 2, stereo, sizeof(stereo)) == PCM_IO);
    assert(service.owner == 0 && stops == 2 && releases == 2 && silence_calls == 2 && failures == 0);
    start_error = -1;
    assert(pcm_service_start(&service, 3, (struct pcm_format){192000, 2, PCM_FLOAT}) == PCM_IO);
    assert(service.owner == 0 && starts == 3 && stops == 2 && releases == 3 &&
           silence_calls == 3 && failures == 1);
    puts("U6 stock raw bridge tests passed");
}
