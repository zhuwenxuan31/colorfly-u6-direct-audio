#include "u6_alsa.h"
#include "u6_tinyalsa_abi.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct pcm { int ready; };
static struct pcm device = {1};
static char events[32];
static size_t event_count;
static unsigned int write_bytes;
static int io_error;
static struct pcm_config config;

static void event(char ch) { events[event_count++] = ch; }
static void check(const char *expected) {
    assert(event_count == strlen(expected));
    assert(memcmp(events, expected, event_count) == 0);
    event_count = 0;
}

struct pcm *pcm_open(unsigned int card, unsigned int node, unsigned int flags,
                     struct pcm_config *request) {
    assert(card == 2 && node == 0 && flags == 3);
    config = *request;
    event('O');
    return &device;
}
int pcm_is_ready(struct pcm *pcm) { return pcm->ready; }
int pcm_close(struct pcm *pcm) { assert(pcm == &device); event('C'); return 0; }
int pcm_mmap_write(struct pcm *pcm, const void *data, unsigned int bytes) {
    assert(pcm == &device && data);
    write_bytes = bytes;
    event('W');
    return io_error;
}
static int prepare(void *unused, unsigned int card, unsigned int rate) {
    assert(!unused && card == 2 && rate > 0);
    event('P');
    return PCM_OK;
}
static int activate(void *unused) { assert(!unused); event('A'); return PCM_OK; }
static void silence(void *unused) { assert(!unused); event('M'); }
static void release(void *unused) { assert(!unused); event('R'); }

static void put(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    assert(f && fputs(contents, f) >= 0 && fclose(f) == 0);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(chdir(argv[1]) == 0);
    assert(mkdir("card2", 0700) == 0 && mkdir("card2/pcm0p", 0700) == 0 &&
           mkdir("card2/pcm0p/sub0", 0700) == 0);
    const char *params = "card2/pcm0p/sub0/hw_params";
    put("cards", " 0 [audiocodec    ]: other\n 2 [accusdiocard  ]: accu_sdio_card\n");
    struct u6_alsa_backend alsa;
    struct pcm_service service;
    struct u6_board_controls board = {NULL, prepare, activate, silence, release, NULL};
    pcm_service_init(&service, u6_alsa_init(&alsa, ".", board));

    put(params, "access: MMAP_INTERLEAVED\nformat: S32_LE\nchannels: 2\nrate: 48000 (48000/1)\n");
    assert(pcm_service_start(&service, 1, (struct pcm_format){192000, 2, PCM_FLOAT}) == PCM_FORMAT_CHANGED);
    check("POMCR");
    assert(service.owner == 0 && write_bytes == 0);

    put(params, "closed\n");
    assert(pcm_service_start(&service, 1, (struct pcm_format){192000, 2, PCM_FLOAT}) == PCM_IO);
    check("POMCR");

    put(params, "access: MMAP_INTERLEAVED\nformat: S32_LE\nchannels: 2\nrate: 192000 (192000/1)\n");
    assert(pcm_service_start(&service, 1, (struct pcm_format){192000, 2, PCM_FLOAT}) == PCM_OK);
    check("POA");
    assert(config.rate == 192000 && config.format == 1 && config.period_size == 4096 && config.period_count == 4);
    const float samples[] = {0.5f, -0.5f, 0.25f, -0.25f};
    assert(pcm_push(&service, 1, samples, sizeof(samples)) == PCM_OK);
    assert(write_bytes == 16);
    check("W");
    io_error = -5;
    assert(pcm_push(&service, 1, samples, sizeof(samples)) == PCM_IO);
    check("WMCR");
    assert(service.owner == 0);
    io_error = 0;
    put(params, "access: MMAP_INTERLEAVED\nformat: S16_LE\nchannels: 2\nrate: 44100 (44100/1)\n");
    const uint8_t cd[] = {1, 2, 3, 4};
    assert(pcm_service_start(&service, 2, (struct pcm_format){44100, 2, PCM_S16}) == PCM_OK);
    check("POA");
    assert(config.rate == 44100 && config.format == 0);
    assert(pcm_push(&service, 2, cd, sizeof(cd)) == PCM_OK && write_bytes == sizeof(cd));
    check("W");
    assert(pcm_service_stop(&service, 2) == PCM_OK);
    check("MCR");

    device.ready = 0;
    assert(pcm_service_start(&service, 2, (struct pcm_format){44100, 2, PCM_S16}) == PCM_IO);
    check("POMCR");
    assert(service.owner == 0);
    put("cards", " 0 [audiocodec    ]: other\n");
    assert(pcm_service_start(&service, 2, (struct pcm_format){44100, 2, PCM_S16}) == PCM_IO);
    check("");
    puts("U6 ALSA backend tests passed");
}
