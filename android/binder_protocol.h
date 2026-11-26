#ifndef U6_BINDER_PROTOCOL_H
#define U6_BINDER_PROTOCOL_H

#include <binder/IBinder.h>
#include <utils/String16.h>

namespace u6 {
constexpr char service_name[] = "u6.direct_audio";
constexpr uint32_t open_track = android::IBinder::FIRST_CALL_TRANSACTION;
constexpr uint32_t start_track = android::IBinder::FIRST_CALL_TRANSACTION;
constexpr uint32_t pump_track = start_track + 1;
constexpr uint32_t stop_track = start_track + 2;
constexpr uint32_t position_track = start_track + 3;
constexpr uint32_t volume_track = start_track + 4;

inline const android::String16 &service_descriptor() {
    static const android::String16 value("net.colorfly.u6.IDirectAudio");
    return value;
}
inline const android::String16 &track_descriptor() {
    static const android::String16 value("net.colorfly.u6.IDirectTrack");
    return value;
}
}

#endif
