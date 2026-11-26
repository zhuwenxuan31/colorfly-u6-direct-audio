#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/types.h>

struct Sink {
    const uintptr_t *functions;
    uint32_t rate;
    int channels;
    unsigned format;
    int started, writes, pauses, flushes, stops, closes;
    int16_t captured[8];
};

static int unexpected() {
    std::fputs("Unexpected stock AudioSink method\n", stderr);
    std::exit(90);
}

static int open_sink(Sink *sink, uint32_t rate, int channels, unsigned, unsigned format,
                     int buffers, void *callback, void *cookie) {
    assert(buffers == 4 && !callback && !cookie);
    sink->rate = rate;
    sink->channels = channels;
    sink->format = format;
    return 0;
}

static int position_sink(Sink *, uint32_t *position) { *position = 0; return 0; }
static ssize_t frame_size(Sink *) { return 4; }
static int start_sink(Sink *sink) { ++sink->started; return 0; }
static void pause_sink(Sink *sink) { ++sink->pauses; }
static void stop_sink(Sink *sink) { ++sink->stops; }
static void close_sink(Sink *sink) { ++sink->closes; }
static void flush_sink(Sink *sink) { ++sink->flushes; }

static ssize_t write_sink(Sink *sink, const void *bytes, size_t count, bool blocking) {
    assert(count == sizeof(sink->captured) && blocking);
    std::memcpy(sink->captured, bytes, count);
    ++sink->writes;
    return count;
}

template <typename Type>
static Type resolve(void *library, const char *symbol) {
    auto pointer = reinterpret_cast<Type>(dlsym(library, symbol));
    if (!pointer) std::fprintf(stderr, "%s: %s\n", symbol, dlerror());
    assert(pointer);
    return pointer;
}

int main() {
    const char *path = sizeof(void *) == 4 ? "/system/lib/libplayer.so" : "/system/lib64/libplayer.so";
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) std::fprintf(stderr, "%s\n", dlerror());
    assert(library);
    auto init = resolve<void *(*)(void *)>(library, "_Z15SoundDeviceInitPv");
    auto format = resolve<void (*)(void *, unsigned, unsigned)>(library, "_Z20SoundDeviceSetFormatPPvjj");
    auto start = resolve<int (*)(void *)>(library, "_Z16SoundDeviceStartPPv");
    auto write = resolve<int (*)(void *, void *, int)>(library, "_Z16SoundDeviceWritePPvS_i");
    auto drop = resolve<void (*)(void *)>(library, "_Z18SoundDeviceReleasePPv");

    // The U6 2.0 vendor calls these slots directly in either ABI.
    uintptr_t functions[32];
    for (auto &entry : functions) entry = reinterpret_cast<uintptr_t>(unexpected);
    functions[10] = reinterpret_cast<uintptr_t>(frame_size);
    functions[13] = reinterpret_cast<uintptr_t>(position_sink);
    functions[19] = reinterpret_cast<uintptr_t>(open_sink);
    functions[20] = reinterpret_cast<uintptr_t>(start_sink);
    functions[21] = reinterpret_cast<uintptr_t>(write_sink);
    functions[22] = reinterpret_cast<uintptr_t>(stop_sink);
    functions[23] = reinterpret_cast<uintptr_t>(flush_sink);
    functions[24] = reinterpret_cast<uintptr_t>(pause_sink);
    functions[25] = reinterpret_cast<uintptr_t>(close_sink);

    const unsigned rates[] = {44100, 96000};
    for (const auto rate : rates) {
        Sink sink = {};
        sink.functions = functions;
        void *device = init(&sink);
        assert(device);
        format(device, rate, 2);
        assert(start(device) == 0);
        assert(sink.rate == rate && sink.channels == 2 && sink.format == 1 && sink.started == 1);
        int16_t sample[] = {32767, -32768, 255, -127, 64, -32, 1, -1};
        assert(write(device, sample, sizeof(sample)) == sizeof(sample));
        assert(sink.writes == 1 && std::memcmp(sample, sink.captured, sizeof(sample)) == 0);
        drop(device);
        assert(sink.pauses == 1 && sink.flushes == 1 && sink.stops == 1 && sink.closes == 1);
        std::printf("U6 stock AudioSink %u Hz format=%u lifecycle OK\n", rate, sink.format);
    }
    dlclose(library);
}
