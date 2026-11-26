#include "binder_protocol.h"
#include "pcm_session.h"
#include "platform_volume.h"
#include "u6_board.h"
#include "u6_raw.h"

#include <android/log.h>
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/MemoryHeapBase.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <utils/Condition.h>
#include <utils/Mutex.h>
#include <pthread.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

using namespace android;

namespace {
constexpr unsigned int max_sessions = 8;
constexpr uint32_t ring_frames = 8192;

int64_t monotonic_us() {
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

class StockPlayer final {
public:
    StockPlayer() {
        const char *file = sizeof(void *) == 8 ? "/system/lib64/libplayer.so" : "/system/lib/libplayer.so";
        handle_ = dlopen(file, RTLD_NOW | RTLD_LOCAL);
        if (!handle_) return;
        api.init = reinterpret_cast<decltype(api.init)>(dlsym(handle_, "_Z19SoundDeviceInit_rawPvS_PFvS_S_E"));
        api.start = reinterpret_cast<decltype(api.start)>(dlsym(handle_, "_Z20SoundDeviceStart_rawPPv"));
        api.stop = reinterpret_cast<decltype(api.stop)>(dlsym(handle_, "_Z19SoundDeviceStop_rawPPv"));
        api.release = reinterpret_cast<decltype(api.release)>(dlsym(handle_, "_Z22SoundDeviceRelease_rawPPv"));
        lease_gate = reinterpret_cast<decltype(lease_gate)>(dlsym(handle_, "u6_alsa_board_lease"));
    }
    ~StockPlayer() { if (handle_) dlclose(handle_); }
    StockPlayer(const StockPlayer &) = delete;
    StockPlayer &operator=(const StockPlayer &) = delete;
    bool ready() const { return api.init && api.start && api.stop && api.release; }
    u6_raw_api api = {};
    int (*lease_gate)(int) = nullptr;
private:
    void *handle_ = nullptr;
};

class Output final : public RefBase {
public:
    Output(const char *lock_path, u6_raw_api api, int (*lease_gate)(int)) {
        pcm_service_init(&service, u6_raw_init(&raw, nullptr, api,
                                               u6_board_init(&board, lock_path, false)));
        board.gate = lease_gate;
        worker_ready = pthread_create(&worker, nullptr, &Output::run_worker, this) == 0;
    }

    ~Output() override {
        if (!worker_ready) return;
        {
            Mutex::Autolock guard(event_lock);
            shutting_down = true;
            wake.signal();
        }
        pthread_join(worker, nullptr);
    }

    bool ready() const { return worker_ready; }

    void notify() {
        Mutex::Autolock guard(event_lock);
        ++event;
        wake.signal();
    }

    // Session ownership and hardware I/O are serialized by lock. Producer wakeups
    // use a separate mutex so the app never waits for pcm_mmap_write().
    int start(pcm_session &session, int &status, float left, float right) {
        if (!worker_ready || !session.ring || session.active) return PCM_INVALID;
        if (service.owner) return PCM_BUSY;
        int result = refresh_volume(left, right, true);
        if (result == PCM_OK) result = pcm_session_start(&session);
        if (result == PCM_OK) {
            current = &session;
            current_status = &status;
            current_left = left;
            current_right = right;
            last_pcm_write_us = 0;
            last_volume_work_us = 0;
            __atomic_store_n(current_status, PCM_OK, __ATOMIC_RELEASE);
            notify();
        }
        return result;
    }

    int volume(pcm_session &session, float left, float right) {
        if (current != &session) return PCM_OK;
        int result = refresh_volume(left, right, true);
        if (result == PCM_OK) {
            current_left = left;
            current_right = right;
        } else {
            stop(session, result);
        }
        return result;
    }

    void stop(pcm_session &session, int reason) {
        if (current == &session) {
            __atomic_store_n(current_status, reason, __ATOMIC_RELEASE);
            current = nullptr;
            current_status = nullptr;
        }
        pcm_session_stop(&session);
    }

    uint64_t reserve() {
        Mutex::Autolock guard(lock);
        if (sessions >= max_sessions) return 0;
        ++sessions;
        if (++next_owner == 0) ++next_owner;
        return next_owner;
    }

    void release() {
        Mutex::Autolock guard(lock);
        --sessions;
    }

    int refresh_volume(float left, float right, bool force) {
        // A transient xrun puts the DAC back in its hardware-muted state.
        // Let tinyalsa recover on the next write before touching the mute control.
        if (service.owner && !u6_raw_running(&raw)) return PCM_OK;
        timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return PCM_IO;
        int64_t ns = static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
        // The physical volume keys use the existing policy service. Check every 100 ms
        // during playback, and immediately for an explicit app volume change.
        if (!force && ns - last_volume_check_ns < 100000000) return PCM_OK;
        u6_volume_state state;
        u6_dac_volume gain;
        if (!platform_volume.read(&state) || !u6_volume_resolve(&state, left, right, &gain))
            return PCM_IO;
        int status = u6_board_set_volume(&board, gain);
        if (status == PCM_OK) last_volume_check_ns = ns;
        return status;
    }

    int position(uint64_t owner, uint64_t *played, int64_t *time_ns) {
        if (owner != service.owner) return PCM_NOT_OWNER;
        return u6_raw_position(&raw, service.frames_written, played, time_ns);
    }

    Mutex lock;
    pcm_service service;
private:
    static void *run_worker(void *argument) {
        static_cast<Output *>(argument)->drain();
        return nullptr;
    }

    void drain() {
        for (;;) {
            uint64_t observed;
            {
                Mutex::Autolock guard(event_lock);
                if (shutting_down) return;
                observed = event;
            }
            {
                Mutex::Autolock guard(lock);
                if (current) {
                    const uint32_t queued = __atomic_load_n(&current->ring->produced, __ATOMIC_ACQUIRE)
                                            - current->consumed;
                    const uint64_t written = service.frames_written;
                    const int64_t before = monotonic_us();
                    int result = pcm_session_drain(current);
                    const int64_t after = monotonic_us();
                    if (result == PCM_OK) {
                        last_pcm_write_us = after;
                        result = refresh_volume(current_left, current_right, false);
                        last_volume_work_us = monotonic_us() - after;
                        if (result == PCM_OK) continue;
                    }
                    if (result != PCM_WOULD_BLOCK) {
                        __android_log_print(ANDROID_LOG_ERROR, "U6Direct",
                            "drain failed: rate=%u result=%d queued_before=%u written=%llu drain_us=%lld gap_us=%lld volume_us=%lld",
                            current->format.rate, result, queued, static_cast<unsigned long long>(written),
                            static_cast<long long>(after - before),
                            static_cast<long long>(last_pcm_write_us ? before - last_pcm_write_us : 0),
                            static_cast<long long>(last_volume_work_us));
                        stop(*current, result);
                    }
                }
            }
            Mutex::Autolock guard(event_lock);
            while (!shutting_down && observed == event) wake.wait(event_lock);
        }
    }

    pthread_t worker = {};
    bool worker_ready = false;
    Mutex event_lock;
    Condition wake;
    uint64_t event = 0;
    bool shutting_down = false;
    pcm_session *current = nullptr;
    int *current_status = nullptr;
    float current_left = 1.0f, current_right = 1.0f;
    PlatformVolume platform_volume;
    int64_t last_volume_check_ns = 0;
    int64_t last_pcm_write_us = 0, last_volume_work_us = 0;
    struct u6_board board;
    struct u6_raw raw;
    unsigned int sessions = 0;
    uint64_t next_owner = 0;
};

class DirectTrack;
class PeerDeath final : public IBinder::DeathRecipient {
public:
    explicit PeerDeath(const sp<DirectTrack> &track) : track_(track) {}
    void binderDied(const wp<IBinder> &) override;
private:
    wp<DirectTrack> track_;
};

class DirectTrack final : public BBinder {
public:
    DirectTrack(const sp<Output> &out, pid_t pid, uid_t uid) : out_(out), pid_(pid), uid_(uid) {}

    ~DirectTrack() override {
        if (peer_ != nullptr && death_ != nullptr) peer_->unlinkToDeath(death_);
        retire();
        out_->release();
    }

    status_t attach(const sp<DirectTrack> &self, uint64_t owner, pcm_format format,
                    const sp<IBinder> &peer, Parcel *reply) {
        uint32_t frame_bytes = static_cast<uint32_t>(pcm_frame_bytes(format));
        size_t size = pcm_ring_size(ring_frames, frame_bytes);
        if (!size) return BAD_VALUE;

        // Keep initialization, publication and client-death cleanup ordered.
        Mutex::Autolock guard(out_->lock);
        heap_ = new MemoryHeapBase(size, 0, "u6-pcm");
        if (heap_->getHeapID() < 0 || !heap_->getBase() || heap_->getBase() == MAP_FAILED)
            return NO_MEMORY;
        auto *ring = static_cast<struct pcm_ring *>(heap_->getBase());
        if (pcm_ring_init(ring, size, ring_frames, frame_bytes) != PCM_OK ||
            pcm_session_init(&session_, &out_->service, owner, format, ring, size, ring_frames) != PCM_OK)
            return BAD_VALUE;
        peer_ = peer;
        death_ = new PeerDeath(self);
        status_t status = peer_->linkToDeath(death_);
        if (status != OK) return status;
        if ((status = reply->writeInt32(ring_frames)) != OK ||
            (status = reply->writeInt32(frame_bytes)) != OK ||
            (status = reply->writeDupFileDescriptor(heap_->getHeapID())) != OK)
            return status;
        return reply->writeStrongBinder(self);
    }

    const String16 &getInterfaceDescriptor() const override { return u6::track_descriptor(); }

    void retire() {
        Mutex::Autolock guard(out_->lock);
        out_->stop(session_, PCM_INVALID);
        heap_.clear();
    }

protected:
    status_t onTransact(uint32_t code, const Parcel &data, Parcel *reply, uint32_t flags) override {
        if (code < u6::start_track || code > u6::volume_track)
            return BBinder::onTransact(code, data, reply, flags);
        if (!reply || !data.enforceInterface(getInterfaceDescriptor()) ||
            (code != u6::volume_track && data.dataAvail() != 0))
            return BAD_VALUE;
        IPCThreadState *caller = IPCThreadState::self();
        if (caller->getCallingPid() != pid_ || caller->getCallingUid() != uid_)
            return PERMISSION_DENIED;

        if (code == u6::stop_track) {
            retire();
            return reply->writeInt32(PCM_OK);
        }

        if (code == u6::pump_track) {
            int status = __atomic_load_n(&writer_status_, __ATOMIC_ACQUIRE);
            if (status == PCM_OK) out_->notify();
            return reply->writeInt32(status);
        }

        Mutex::Autolock guard(out_->lock);
        if (code == u6::position_track) {
            uint64_t played = 0;
            int64_t time_ns = 0;
            int result = session_.active ? out_->position(session_.owner, &played, &time_ns) : PCM_INVALID;
            status_t status = reply->writeInt32(result);
            if (status != OK || result != PCM_OK) return status;
            if ((status = reply->writeInt64(static_cast<int64_t>(played))) != OK) return status;
            return reply->writeInt64(time_ns);
        }
        int status;
        if (code == u6::volume_track) {
            float left, right;
            if (data.readFloat(&left) != OK || data.readFloat(&right) != OK || data.dataAvail() ||
                !(left >= 0.0f && left <= 1.0f && right >= 0.0f && right <= 1.0f))
                return BAD_VALUE;
            if (!session_.ring) return reply->writeInt32(PCM_INVALID);
            left_ = left;
            right_ = right;
            status = out_->volume(session_, left_, right_);
        } else {
            status = out_->start(session_, writer_status_, left_, right_);
        }
        return reply->writeInt32(status);
    }

private:
    sp<Output> out_;
    pid_t pid_;
    uid_t uid_;
    struct pcm_session session_ = {};
    int writer_status_ = PCM_INVALID;
    float left_ = 1.0f, right_ = 1.0f;
    sp<MemoryHeapBase> heap_;
    sp<IBinder> peer_;
    sp<PeerDeath> death_;
};

void PeerDeath::binderDied(const wp<IBinder> &) {
    sp<DirectTrack> track = track_.promote();
    if (track != nullptr) track->retire();
}

class DirectAudio final : public BBinder {
public:
    DirectAudio(const char *lock_path, u6_raw_api api, int (*lease_gate)(int))
        : out_(new Output(lock_path, api, lease_gate)) {}
    bool ready() const { return out_->ready(); }
    const String16 &getInterfaceDescriptor() const override { return u6::service_descriptor(); }
protected:
    status_t onTransact(uint32_t code, const Parcel &data, Parcel *reply, uint32_t flags) override {
        if (code != u6::open_track) return BBinder::onTransact(code, data, reply, flags);
        if (!reply || !data.enforceInterface(getInterfaceDescriptor())) return BAD_VALUE;
        int32_t rate, channels, encoding;
        if (data.readInt32(&rate) != OK || data.readInt32(&channels) != OK ||
            data.readInt32(&encoding) != OK || rate <= 0 || rate > 384000 || channels != 2 ||
            encoding < PCM_S16 || encoding > PCM_FLOAT)
            return BAD_VALUE;
        sp<IBinder> peer = data.readStrongBinder();
        if (peer == nullptr || peer->remoteBinder() == nullptr || data.dataAvail() != 0)
            return BAD_VALUE;

        IPCThreadState *caller = IPCThreadState::self();
        pid_t pid = caller->getCallingPid();
        if (pid <= 0) return PERMISSION_DENIED;
        uint64_t owner = out_->reserve();
        if (owner == 0) return WOULD_BLOCK;
        sp<DirectTrack> track = new DirectTrack(out_, pid, caller->getCallingUid());
        pcm_format format = {static_cast<uint32_t>(rate), static_cast<uint32_t>(channels),
                             static_cast<pcm_encoding>(encoding)};
        return track->attach(track, owner, format, peer, reply);
    }

private:
    sp<Output> out_;
};
}

int main(int argc, char **argv) {
    StockPlayer stock;
    if (!stock.ready()) {
        fprintf(stderr, "u6-direct-audio: stock raw endpoint is unavailable\n");
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "--read-volume") == 0) {
        PlatformVolume platform;
        u6_volume_state state;
        u6_dac_volume volume;
        if (!platform.read(&state) || !u6_volume_resolve(&state, 1.0f, 1.0f, &volume)) return 1;
        printf("u6-volume index=%d device=%u lineout=%d balance=%.6f dac=%d,%d muted=%d\n",
               state.index, state.device, state.lineout, state.balance,
               volume.left, volume.right, volume.muted);
        return 0;
    }
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "--check") == 0) {
        if (argc == 3) {
            void *client = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
            if (!client) {
                fprintf(stderr, "client load failed: %s\n", dlerror());
                return 1;
            }
            const char *entries[] = {"u6_direct_open", "u6_direct_start", "u6_direct_write",
                                     "u6_direct_position", "u6_direct_close", "u6_direct_set_volume",
                                     "u6_register_audiotrack"};
            for (const char *entry : entries) {
                if (!dlsym(client, entry)) {
                    fprintf(stderr, "client export missing: %s\n", entry);
                    dlclose(client);
                    return 1;
                }
            }
            dlclose(client);
            puts("u6-direct-audio: client exports resolved");
        }
        puts("u6-direct-audio: original raw symbols resolved");
        if (stock.lease_gate) puts("u6-direct-audio: ALSA ownership gate resolved");
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: u6-direct-audio /path/to/existing-board-lock\n");
        return 2;
    }
    if (!stock.lease_gate) {
        fprintf(stderr, "u6-direct-audio: stock ALSA ownership gate is missing\n");
        return 1;
    }
    sp<IServiceManager> manager = defaultServiceManager();
    sp<DirectAudio> audio = new DirectAudio(argv[1], stock.api, stock.lease_gate);
    if (manager == nullptr || !audio->ready() ||
        manager->addService(String16(u6::service_name), audio) != OK)
        return 1;
    ProcessState::self()->startThreadPool();
    IPCThreadState::self()->joinThreadPool();
    return 0;
}
