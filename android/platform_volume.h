#ifndef U6_PLATFORM_VOLUME_H
#define U6_PLATFORM_VOLUME_H

#include "u6_volume.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>

class PlatformVolume final {
public:
    PlatformVolume() {
        library_ = dlopen("libmedia.so", RTLD_NOW | RTLD_LOCAL);
        if (!library_) return;
        devices_ = reinterpret_cast<decltype(devices_)>(dlsym(library_,
            "_ZN7android11AudioSystem19getDevicesForStreamE19audio_stream_type_t"));
        index_ = reinterpret_cast<decltype(index_)>(dlsym(library_,
            "_ZN7android11AudioSystem20getStreamVolumeIndexE19audio_stream_type_tPij"));
    }
    ~PlatformVolume() { if (library_) dlclose(library_); }
    PlatformVolume(const PlatformVolume &) = delete;
    PlatformVolume &operator=(const PlatformVolume &) = delete;

    bool read(u6_volume_state *state) const {
        constexpr int music = 3;
        if (!devices_ || !index_) return false;
        uint32_t device = devices_(music);
        if (device != U6_OUT_SPEAKER && device != U6_OUT_HEADPHONE && device != U6_OUT_HEADSET)
            return false;
        int index;
        if (index_(music, &index, device) != 0) return false;

        char lineout[PROP_VALUE_MAX], balance[PROP_VALUE_MAX];
        if (!__system_property_get("setting.audio.lineout", lineout) ||
            !__system_property_get("setting.audio.balance", balance) ||
            (strcmp(lineout, "0") && strcmp(lineout, "1")))
            return false;
        char *end;
        float value = strtof(balance, &end);
        if (end == balance || *end) return false;
        *state = {index, device, lineout[0] == '1', value};
        return true;
    }

private:
    void *library_ = nullptr;
    uint32_t (*devices_)(int) = nullptr;
    int (*index_)(int, int *, uint32_t) = nullptr;
};

#endif
