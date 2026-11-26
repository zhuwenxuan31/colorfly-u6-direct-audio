#ifndef U6_TINYALSA_ABI_H
#define U6_TINYALSA_ABI_H

#include <stddef.h>

struct pcm;
struct timespec;

/* U6 2.0 libtinyalsa.so reads ten 32-bit words in either ABI. */
struct pcm_config {
    unsigned int channels;
    unsigned int rate;
    unsigned int period_size;
    unsigned int period_count;
    unsigned int format;
    unsigned int start_threshold;
    unsigned int stop_threshold;
    unsigned int silence_threshold;
    unsigned int silence_size;
    int avail_min;
};

_Static_assert(sizeof(struct pcm_config) == 40, "U6 2.0 tinyalsa config size mismatch");
_Static_assert(offsetof(struct pcm_config, avail_min) == 0x24, "U6 tinyalsa ABI mismatch");

enum { U6_PCM_MMAP_NOIRQ = 3, U6_PCM_FORMAT_S16_LE = 0, U6_PCM_FORMAT_S32_LE = 1 };

struct pcm *pcm_open(unsigned int card, unsigned int device, unsigned int flags,
                     struct pcm_config *config);
int pcm_is_ready(struct pcm *pcm);
unsigned int pcm_get_buffer_size(struct pcm *pcm);
int pcm_get_htimestamp(struct pcm *pcm, unsigned int *available, struct timespec *stamp);
int pcm_close(struct pcm *pcm);
int pcm_mmap_write(struct pcm *pcm, const void *data, unsigned int bytes);

struct mixer;
struct mixer_ctl;

struct mixer *mixer_open(unsigned int card);
void mixer_close(struct mixer *mixer);
struct mixer_ctl *mixer_get_ctl_by_name(struct mixer *mixer, const char *name);
unsigned int mixer_ctl_get_num_values(struct mixer_ctl *control);
int mixer_ctl_get_value(struct mixer_ctl *control, unsigned int index);
int mixer_ctl_set_value(struct mixer_ctl *control, unsigned int index, int value);

int mixer_ctl_set_array(struct mixer_ctl *control, const void *values, size_t count);

#endif
