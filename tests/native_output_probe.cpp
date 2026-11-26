#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/types.h>
#include <time.h>

struct NativeTimestamp {
    uint32_t position;
    timespec time;
};

template <class Fn>
static Fn resolve(void *library, const char *name) {
    auto result = reinterpret_cast<Fn>(dlsym(library, name));
    if (!result) std::fprintf(stderr, "%s\n", dlerror());
    assert(result);
    return result;
}

using Open = int (*)(void *, unsigned, int, unsigned, unsigned, int, void *, void *,
                     unsigned, const void *, bool, unsigned);
static const char open_symbol[] =
    "_ZN7android18MediaPlayerService11AudioOutput4openEjij14audio_format_tiPFjPNS_15"
    "MediaPlayerBase9AudioSinkEPvjS6_NS4_10cb_event_tEES6_20audio_output_flags_tPK20audio_offload_info_tbj";

static bool ready(const void *self) {
    const uintptr_t *table;
    std::memcpy(&table, self, sizeof(table));
    return reinterpret_cast<bool (*)(const void *)>(table[6])(self);
}

int main(int argc, char **argv) {
    assert(argc == 2 && sizeof(void *) == 4);
    bool mock = std::strcmp(argv[1], "mock") == 0;
    void *library = dlopen("libmediaplayerservice.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) std::fprintf(stderr, "%s\n", dlerror());
    assert(library);
    auto construct = resolve<void (*)(void *, int, int, int, const void *)>(
        library, "_ZN7android18MediaPlayerService11AudioOutputC1EiiiPK18audio_attributes_t");
    auto destroy = resolve<void (*)(void *)>(
        library, "_ZN7android18MediaPlayerService11AudioOutputD2Ev");
    auto interposed = resolve<Open>(RTLD_DEFAULT, open_symbol);
    assert(interposed != resolve<Open>(library, open_symbol));

    const unsigned rates[] = {44100, 96000};
    for (unsigned rate : rates) {
        alignas(16) unsigned char self[512] = {};
        alignas(4) uint32_t attributes[0x110 / sizeof(uint32_t)] = {2, 1};
        construct(self, 17, 1000, 0, attributes);
        auto *table = *reinterpret_cast<const uintptr_t **>(self);
        auto open = reinterpret_cast<Open>(table[19]);
        auto start = reinterpret_cast<int (*)(void *)>(table[20]);
        auto write = reinterpret_cast<ssize_t (*)(void *, const void *, size_t, bool)>(table[21]);
        auto stop = reinterpret_cast<void (*)(void *)>(table[22]);
        auto flush = reinterpret_cast<void (*)(void *)>(table[23]);
        auto pause = reinterpret_cast<void (*)(void *)>(table[24]);
        auto close = reinterpret_cast<void (*)(void *)>(table[25]);
        auto frame_size = reinterpret_cast<ssize_t (*)(void *)>(table[10]);
        auto timestamp = reinterpret_cast<int (*)(void *, NativeTimestamp *)>(table[14]);
        auto position = resolve<int (*)(const void *, uint32_t *)>(
            RTLD_DEFAULT, "_ZNK7android18MediaPlayerService11AudioOutput11getPositionEPj");
        assert(open == interposed);
        assert(!ready(self));

        unsigned format = rate == 44100 ? 1 : 3;
        assert(open(self, rate, 2, 3, format, 4, nullptr, nullptr, 0, nullptr, false, 0) == 0);
        assert(ready(self));
        assert(frame_size(self) == (format == 1 ? 4 : 8));
        NativeTimestamp stamp{};
        if (mock) {
            assert(start(self) == 0);
            const uint32_t samples[] = {0x80000001, 0x7fffffff, 0xffaabbcc, 0x00000002};
            assert(write(self, samples, sizeof(samples), false) == sizeof(samples));
            assert(timestamp(self, &stamp) == 0);
            assert(stamp.position == 1 && stamp.time.tv_sec == 2 && stamp.time.tv_nsec == 1000);
            auto unavailable = resolve<void (*)(bool)>(RTLD_DEFAULT, "u6_mock_position_unavailable");
            uint32_t head = 0;
            assert(position(self, &head) == 0 && head == 1);
            unavailable(true);
            assert(timestamp(self, &stamp) == -EAGAIN);
            assert(position(self, &head) == 0 && head == 1);
            pause(self);
            assert(position(self, &head) == 0 && head == 1);
            unavailable(false);
            assert(write(self, samples, sizeof(samples), false) == -EAGAIN);
            flush(self);
            stop(self);
            assert(write(self, samples, sizeof(samples), false) == -EAGAIN);
            assert(start(self) == 0);
            assert(write(self, samples, sizeof(samples), false) == sizeof(samples));
            assert(timestamp(self, &stamp) == 0);
            assert(stamp.position == 1);
        } else {
            assert(start(self) == -ENODEV);
            const uint32_t samples[] = {0x80000001, 0x7fffffff};
            assert(write(self, samples, sizeof(samples), false) == -ENODEV);
            assert(timestamp(self, &stamp) == -ENODEV);
        }
        if (rate == 44100) {
            auto destroy_via_table = reinterpret_cast<void (*)(void *)>(table[0]);
            assert(destroy_via_table != destroy);
            destroy_via_table(self);
        } else {
            close(self);
            assert(!ready(self));
            destroy(self);
        }
        std::printf("native AudioOutput %u Hz format=%u %s OK\n", rate, format, argv[1]);
    }
}
