// AudioOutput ABI for the SHA-verified U6 2.0 ARMv7 libmediaplayerservice.
#include "apple_track.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>

namespace {
#define OUTPUT "_ZN7android18MediaPlayerService11AudioOutput"
#define CONST_OUTPUT "_ZNK7android18MediaPlayerService11AudioOutput"
#define OPEN_SYMBOL OUTPUT "4openEjij14audio_format_tiPFjPNS_15MediaPlayerBase9AudioSinkEPvjS6_NS4_10cb_event_tEES6_20audio_output_flags_tPK20audio_offload_info_tbj"

constexpr unsigned kBufferFrames = 8192;
constexpr size_t kVtableWords = 34;
constexpr size_t kVtableHeaderWords = 2;
constexpr size_t kReadySlot = 6;
constexpr char kVtableSymbol[] = "_ZTVN7android18MediaPlayerService11AudioOutputE";

struct NativeTimestamp {
    uint32_t position;
    timespec time;
};
static_assert(sizeof(NativeTimestamp) == 12, "U6 ARMv7 AudioTimestamp ABI");

struct NativeOutput {
    NativeOutput *next;
    void *object;
    pcm_format format;
    u6_direct_client *client;
    int start_error;
    uint64_t frames_written;
    uint64_t position_base;
    uint64_t last_presented;
};

pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
NativeOutput *outputs;
uintptr_t direct_vtable[kVtableWords];
const uintptr_t *stock_vtable;
void *stock_library;
pthread_once_t stock_once = PTHREAD_ONCE_INIT;

void load_stock() {
    stock_library = dlopen("libmediaplayerservice.so", RTLD_NOW | RTLD_LOCAL);
}

template <class Fn>
Fn stock(const char *name) {
    pthread_once(&stock_once, load_stock);
    return stock_library ? reinterpret_cast<Fn>(dlsym(stock_library, name)) : nullptr;
}

class Guard {
public:
    Guard() { pthread_mutex_lock(&output_lock); }
    ~Guard() { pthread_mutex_unlock(&output_lock); }
};

template <class T> T field(const void *object, size_t offset) {
    T result;
    std::memcpy(&result, static_cast<const unsigned char *>(object) + offset, sizeof(result));
    return result;
}

NativeOutput *find_output(const void *object) {
    for (NativeOutput *current = outputs; current; current = current->next)
        if (current->object == object) return current;
    return nullptr;
}

bool output_ready(const void *object) {
    Guard guard;
    if (find_output(object)) return true;
    auto stock_ready = reinterpret_cast<bool (*)(const void *)>(stock_vtable[kReadySlot]);
    return stock_ready(object);
}

bool install_ready_slot(void *object) {
    const auto *table = field<const uintptr_t *>(object, 0);
    const uintptr_t *direct_address = direct_vtable + kVtableHeaderWords;
    if (table == direct_address) return true;
    if (!stock_vtable) {
        const auto *base = stock<const uintptr_t *>(kVtableSymbol);
        if (!base || table != base + kVtableHeaderWords) return false;
        std::memcpy(direct_vtable, base, sizeof(direct_vtable));
        direct_vtable[kVtableHeaderWords + kReadySlot] = reinterpret_cast<uintptr_t>(&output_ready);
        stock_vtable = table;
    }
    if (table != stock_vtable) return false;

    // Only this object's vptr changes. The table remains valid through stock destruction.
    std::memcpy(object, &direct_address, sizeof(direct_address));
    return true;
}

bool music(const void *object) {
    const int stream = field<int>(object, 0x28);
    if (stream != 3) return false;
    const void *attributes = field<void *>(object, 0x2c);
    const int usage = attributes ? field<int>(attributes, 4) : 1;
    const int content = attributes ? field<int>(attributes, 0) : 0;
    const unsigned int flags = attributes ? field<unsigned int>(attributes, 12) : 0;
    return u6_is_music_track(usage, content, flags);
}

bool native_format(unsigned rate, int channels, unsigned mask, unsigned encoding, pcm_format *format) {
    if (channels != 2 || (mask != 0 && mask != 3) || !rate || rate > 192000)
        return false;
    pcm_encoding pcm;
    switch (encoding) {
        case 1: pcm = PCM_S16; break;
        case 3: pcm = PCM_S32; break;
        case 5: pcm = PCM_FLOAT; break;
        case 6: pcm = PCM_S24_PACKED; break;
        default: return false;
    }
    *format = {rate, 2, pcm};
    return true;
}

int read_position(NativeOutput *output, int64_t *time_ns) {
    uint64_t frames;
    int status = u6_direct_position(output->client, &frames, time_ns);
    if (status != PCM_OK) return status;
    if (frames < output->last_presented) return PCM_WOULD_BLOCK;
    output->last_presented = frames;
    return PCM_OK;
}

void release_client(NativeOutput *output, bool keep_position) {
    if (output->client) {
        if (keep_position) {
            int64_t time_ns;
            read_position(output, &time_ns);
            output->position_base += output->last_presented;
        }
        u6_direct_close(output->client);
        output->client = nullptr;
        output->last_presented = 0;
    }
    if (!keep_position) {
        output->frames_written = 0;
        output->position_base = 0;
    }
}

bool remove_output(void *self) {
    NativeOutput **link = &outputs;
    while (*link && (*link)->object != self) link = &(*link)->next;
    if (!*link) return false;
    NativeOutput *entry = *link;
    *link = entry->next;
    release_client(entry, false);
    std::free(entry);
    return true;
}

#define DESTRUCTOR(method, symbol) \
    extern "C" void method(void *) asm(symbol); \
    extern "C" void method(void *self) { \
        { Guard guard; remove_output(self); } \
        auto original = stock<decltype(&method)>(symbol); \
        if (original) original(self); \
    }
DESTRUCTOR(u6_output_destroy, OUTPUT "D2Ev")
DESTRUCTOR(u6_output_delete, OUTPUT "D0Ev")
#undef DESTRUCTOR
}

extern "C" int u6_output_open(void *self, unsigned rate, int channels, unsigned mask,
                             unsigned encoding, int buffers, void *callback, void *cookie,
                             unsigned flags, const void *offload, bool reconnect, unsigned frames)
    asm(OPEN_SYMBOL);
extern "C" int u6_output_open(void *self, unsigned rate, int channels, unsigned mask,
                             unsigned encoding, int buffers, void *callback, void *cookie,
                             unsigned flags, const void *offload, bool reconnect, unsigned frames) {
    {
        Guard guard;
        if (music(self)) {
            remove_output(self);
            pcm_format format;
            if (callback || offload || (flags & 0x10) || !native_format(rate, channels, mask, encoding, &format))
                return -EINVAL;
            auto *entry = static_cast<NativeOutput *>(std::calloc(1, sizeof(NativeOutput)));
            if (!entry) return -ENOMEM;
            if (!install_ready_slot(self)) {
                std::free(entry);
                return -ENOSYS;
            }
            entry->object = self;
            entry->format = format;
            entry->next = outputs;
            outputs = entry;
            return 0;
        }
        remove_output(self);
    }
    auto original = stock<decltype(&u6_output_open)>(OPEN_SYMBOL);
    return original ? original(self, rate, channels, mask, encoding, buffers, callback, cookie,
                               flags, offload, reconnect, frames) : -ENOSYS;
}

extern "C" int u6_output_start(void *self) asm(OUTPUT "5startEv");
extern "C" int u6_output_start(void *self) {
    {
        Guard guard;
        NativeOutput *entry = find_output(self);
        if (entry) {
            if (entry->client) return 0;
            int status = u6_direct_open(entry->format, &entry->client);
            if (status == PCM_OK)
                status = u6_direct_set_volume(entry->client, field<float>(self, 0x30),
                                              field<float>(self, 0x34));
            if (status == PCM_OK) status = u6_direct_start(entry->client);
            entry->start_error = status == PCM_OK ? 0 : status == PCM_BUSY ? -EBUSY : -ENODEV;
            if (status == PCM_OK) return 0;
            release_client(entry, true);
            return entry->start_error;
        }
    }
    auto original = stock<decltype(&u6_output_start)>(OUTPUT "5startEv");
    return original ? original(self) : -ENOSYS;
}

extern "C" ssize_t u6_output_write(void *self, const void *data, size_t bytes, bool blocking)
    asm(OUTPUT "5writeEPKvjb");
extern "C" ssize_t u6_output_write(void *self, const void *data, size_t bytes, bool blocking) {
    {
        Guard guard;
        NativeOutput *entry = find_output(self);
        if (entry) {
            // NuPlayer can still try to prefill after pausing or flushing.
            if (!entry->client) return entry->start_error ? entry->start_error : -EAGAIN;
            size_t width = pcm_frame_bytes(entry->format);
            if (!width || bytes % width || (!data && bytes)) return -EINVAL;
            if (!bytes) return 0;
            size_t accepted = 0;
            int status = u6_direct_write(entry->client, data, bytes / width, &accepted);
            if (status == PCM_WOULD_BLOCK) return -EAGAIN;
            if (status != PCM_OK || accepted > bytes / width) return -EIO;
            entry->frames_written += accepted;
            return static_cast<ssize_t>(accepted * width);
        }
    }
    auto original = stock<decltype(&u6_output_write)>(OUTPUT "5writeEPKvjb");
    return original ? original(self, data, bytes, blocking) : -ENOSYS;
}

#define CONTROL(method, symbol, call) \
    extern "C" void method(void *) asm(symbol); \
    extern "C" void method(void *self) { \
        { Guard guard; if (auto *entry = find_output(self)) { call; return; } } \
        auto original = stock<decltype(&method)>(symbol); \
        if (original) original(self); \
    }
CONTROL(u6_output_pause, OUTPUT "5pauseEv", release_client(entry, true))
CONTROL(u6_output_stop, OUTPUT "4stopEv", release_client(entry, false))
CONTROL(u6_output_flush, OUTPUT "5flushEv", release_client(entry, false))
CONTROL(u6_output_close, OUTPUT "5closeEv", remove_output(entry->object))
#undef CONTROL

#define QUERY(type, method, symbol, expression) \
    extern "C" type method(const void *) asm(symbol); \
    extern "C" type method(const void *self) { \
        { Guard guard; if (auto *entry = find_output(self)) return (expression); } \
        auto original = stock<decltype(&method)>(symbol); \
        return original ? original(self) : 0; \
    }
QUERY(ssize_t, u6_output_frame_size, CONST_OUTPUT "9frameSizeEv", pcm_frame_bytes(entry->format))
QUERY(ssize_t, u6_output_buffer_size, CONST_OUTPUT "10bufferSizeEv", pcm_frame_bytes(entry->format) * kBufferFrames)
QUERY(ssize_t, u6_output_frame_count, CONST_OUTPUT "10frameCountEv", kBufferFrames)
QUERY(ssize_t, u6_output_channels, CONST_OUTPUT "12channelCountEv", entry->format.channels)
QUERY(uint32_t, u6_output_sample_rate, CONST_OUTPUT "13getSampleRateEv", entry->format.rate)
QUERY(float, u6_output_ms, CONST_OUTPUT "13msecsPerFrameEv", 1000.0f / entry->format.rate)
#undef QUERY

extern "C" int u6_output_written(const void *, uint32_t *) asm(CONST_OUTPUT "16getFramesWrittenEPj");
extern "C" int u6_output_written(const void *self, uint32_t *written) {
    {
        Guard guard;
        if (const auto *entry = find_output(self)) {
            if (!written) return -EINVAL;
            *written = static_cast<uint32_t>(entry->frames_written);
            return 0;
        }
    }
    auto original = stock<decltype(&u6_output_written)>(CONST_OUTPUT "16getFramesWrittenEPj");
    return original ? original(self, written) : -ENOSYS;
}
extern "C" int u6_output_position(const void *, uint32_t *) asm(CONST_OUTPUT "11getPositionEPj");
extern "C" int u6_output_position(const void *self, uint32_t *position) {
    {
        Guard guard;
        if (auto *entry = find_output(self)) {
            if (!position) return -EINVAL;
            int64_t when;
            if (entry->client) read_position(entry, &when);
            // NuPlayer uses this counter to schedule its next decoder drain.
            *position = static_cast<uint32_t>(entry->position_base + entry->last_presented);
            return 0;
        }
    }
    auto original = stock<decltype(&u6_output_position)>(CONST_OUTPUT "11getPositionEPj");
    return original ? original(self, position) : -ENOSYS;
}

#define TIMESTAMP_SYMBOL CONST_OUTPUT "12getTimestampERNS_14AudioTimestampE"
extern "C" int u6_output_timestamp(const void *, NativeTimestamp *) asm(TIMESTAMP_SYMBOL);
extern "C" int u6_output_timestamp(const void *self, NativeTimestamp *timestamp) {
    {
        Guard guard;
        if (auto *entry = find_output(self)) {
            if (!timestamp) return -EINVAL;
            if (!entry->client) return -ENODEV;
            int64_t nanoseconds;
            int status = read_position(entry, &nanoseconds);
            if (status != PCM_OK) return status == PCM_WOULD_BLOCK ? -EAGAIN : -EIO;
            timestamp->position = static_cast<uint32_t>(entry->position_base + entry->last_presented);
            timestamp->time.tv_sec = nanoseconds / 1000000000;
            timestamp->time.tv_nsec = nanoseconds % 1000000000;
            return 0;
        }
    }
    auto original = stock<decltype(&u6_output_timestamp)>(TIMESTAMP_SYMBOL);
    return original ? original(self, timestamp) : -ENOSYS;
}
