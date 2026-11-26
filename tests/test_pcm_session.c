#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "pcm_session.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

struct fake {
    unsigned int opens, closes, writes, rate;
    unsigned char data[64];
    size_t bytes;
};

static int open_pcm(void *context, struct pcm_format request, struct pcm_format *actual) {
    struct fake *f = context;
    f->opens++;
    f->rate = request.rate;
    *actual = request;
    return PCM_OK;
}

static int write_pcm(void *context, const uint8_t *data, size_t frames) {
    struct fake *f = context;
    size_t bytes = frames * 4;
    assert(f->bytes + bytes <= sizeof(f->data));
    memcpy(f->data + f->bytes, data, bytes);
    f->bytes += bytes;
    f->writes++;
    return PCM_OK;
}
static void close_pcm(void *context) { ((struct fake *)context)->closes++; }

int main(void) {
    struct fake f = {0};
    struct pcm_service service;
    pcm_service_init(&service, (struct pcm_backend){&f, open_pcm, write_pcm, close_pcm});
    struct pcm_session session, other;
    size_t bytes = pcm_ring_size(4, 4);
    struct pcm_ring *ring = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(ring != MAP_FAILED && pcm_ring_init(ring, bytes, 4, 4) == PCM_OK);
    struct pcm_format format = {44100, 2, PCM_S16};
    assert(pcm_session_init(&session, &service, 1, format, ring, bytes - 1, 4) == PCM_INVALID);
    assert(pcm_session_init(&session, &service, 1, format, ring, bytes, 4) == PCM_OK);
    assert(pcm_session_init(&other, &service, 2, format, ring, bytes, 4) == PCM_OK);
    assert(f.opens == 0);
    assert(pcm_session_start(&session) == PCM_OK);
    assert(pcm_session_start(&other) == PCM_BUSY);
    assert(pcm_session_drain(&session) == PCM_WOULD_BLOCK && f.rate == 44100);
    const uint32_t pcm[] = {1, 2, 3, 4, 5, 6};
    pid_t peer = fork();
    assert(peer >= 0);
    if (peer == 0) {
        size_t written = 0;
        int result = pcm_ring_write(ring, 4, 4, pcm, 3, &written);
        _exit(result == PCM_OK && written == 3 ? 0 : 1);
    }
    int status;
    assert(waitpid(peer, &status, 0) == peer && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(pcm_session_drain(&session) == PCM_OK && f.bytes == 12);
    size_t written = 0;
    assert(pcm_ring_write(ring, 4, 4, pcm + 3, 3, &written) == PCM_OK && written == 3);
    assert(pcm_session_drain(&session) == PCM_OK && f.bytes == sizeof(pcm));
    assert(!memcmp(pcm, f.data, sizeof(pcm)));
    assert(pcm_ring_write(ring, 4, 4, pcm, 5, &written) == PCM_OK && written == 4);
    assert(pcm_ring_write(ring, 4, 4, pcm, 1, &written) == PCM_WOULD_BLOCK && written == 0);
    assert(pcm_session_stop(&session) == PCM_OK && f.closes == 1);
    assert(pcm_session_start(&session) == PCM_INVALID);
    assert(pcm_session_drain(&session) == PCM_INVALID);
    assert(f.bytes == sizeof(pcm));
    assert(pcm_ring_init(ring, bytes, 4, 4) == PCM_OK);
    assert(pcm_session_init(&session, &service, 3, format, ring, bytes, 4) == PCM_OK);
    assert(pcm_session_start(&session) == PCM_OK);
    __atomic_store_n(&ring->consumed, 5, __ATOMIC_RELEASE);
    __atomic_store_n(&ring->produced, 5, __ATOMIC_RELEASE);
    assert(pcm_session_drain(&session) == PCM_INVALID && service.owner == 0 && f.closes == 2);
    assert(f.bytes == sizeof(pcm));
    assert(pcm_ring_init(ring, bytes, 4, 4) == PCM_OK);
    assert(pcm_session_init(&session, &service, 4, format, ring, bytes, 4) == PCM_OK);
    assert(pcm_session_start(&session) == PCM_OK);
    session.consumed = UINT32_MAX - 1;
    __atomic_store_n(&ring->consumed, session.consumed, __ATOMIC_RELEASE);
    __atomic_store_n(&ring->produced, session.consumed, __ATOMIC_RELEASE);
    assert(pcm_ring_write(ring, 4, 4, pcm, 3, &written) == PCM_OK && written == 3);
    assert(pcm_session_drain(&session) == PCM_OK && session.consumed == 1);
    assert(!memcmp(pcm, f.data + sizeof(pcm), 12));
    assert(pcm_session_stop(&session) == PCM_OK && f.closes == 3);
    assert(munmap(ring, bytes) == 0);
    puts("Shared PCM session tests passed");
}
