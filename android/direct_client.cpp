#include "direct_client.h"
#include "binder_protocol.h"
#include "pcm_session.h"

#include <binder/Binder.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <sys/mman.h>

using namespace android;

struct u6_direct_client {
    sp<IBinder> peer;
    sp<IBinder> track;
    struct pcm_ring *ring;
    size_t mapping_size;
    uint32_t capacity;
    uint32_t frame_bytes;
    bool started;
};

static int command(const sp<IBinder> &track, uint32_t operation) {
    Parcel request, reply;
    int32_t status;
    if (request.writeInterfaceToken(u6::track_descriptor()) != OK ||
        track->transact(operation, request, &reply) != OK ||
        reply.readInt32(&status) != OK || status < PCM_OK || status > PCM_WOULD_BLOCK ||
        reply.dataAvail() != 0)
        return PCM_IO;
    return status;
}

int u6_direct_open(struct pcm_format format, struct u6_direct_client **result) {
    if (!result) return PCM_INVALID;
    *result = nullptr;
    size_t frame_bytes = pcm_frame_bytes(format);
    if (!frame_bytes || !format.rate || format.rate > 384000) return PCM_UNSUPPORTED;
    sp<IServiceManager> manager = defaultServiceManager();
    if (manager == nullptr) return PCM_IO;
    sp<IBinder> service = manager->checkService(String16(u6::service_name));
    if (service == nullptr) return PCM_IO;
    sp<IBinder> peer = new BBinder();

    Parcel request, reply;
    if (request.writeInterfaceToken(u6::service_descriptor()) != OK ||
        request.writeInt32(format.rate) != OK || request.writeInt32(format.channels) != OK ||
        request.writeInt32(format.encoding) != OK || request.writeStrongBinder(peer) != OK)
        return PCM_IO;
    status_t status = service->transact(u6::open_track, request, &reply);
    if (status != OK) return status == WOULD_BLOCK ? PCM_BUSY : PCM_IO;

    int32_t capacity, actual_frame_bytes;
    if (reply.readInt32(&capacity) != OK || reply.readInt32(&actual_frame_bytes) != OK ||
        capacity <= 0 || actual_frame_bytes != static_cast<int32_t>(frame_bytes))
        return PCM_IO;
    size_t size = pcm_ring_size(capacity, actual_frame_bytes);
    int fd = reply.readFileDescriptor();
    sp<IBinder> track = reply.readStrongBinder();
    if (fd < 0 || !size || track == nullptr || reply.dataAvail() != 0) return PCM_IO;
    void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        command(track, u6::stop_track);
        return PCM_IO;
    }
    *result = new u6_direct_client{peer, track, static_cast<struct pcm_ring *>(mapping),
                                   size, static_cast<uint32_t>(capacity),
                                   static_cast<uint32_t>(frame_bytes), false};
    return PCM_OK;
}

int u6_direct_set_volume(struct u6_direct_client *client, float left, float right) {
    if (!client || !(left >= 0.0f && left <= 1.0f && right >= 0.0f && right <= 1.0f))
        return PCM_INVALID;
    Parcel request, reply;
    int32_t status;
    if (request.writeInterfaceToken(u6::track_descriptor()) != OK ||
        request.writeFloat(left) != OK || request.writeFloat(right) != OK ||
        client->track->transact(u6::volume_track, request, &reply) != OK ||
        reply.readInt32(&status) != OK || status < PCM_OK || status > PCM_WOULD_BLOCK ||
        reply.dataAvail() != 0)
        return PCM_IO;
    return status;
}

int u6_direct_start(struct u6_direct_client *client) {
    if (!client || client->started) return PCM_INVALID;
    int status = command(client->track, u6::start_track);
    client->started = status == PCM_OK;
    return status;
}

int u6_direct_write(struct u6_direct_client *client, const void *data, size_t frames,
                    size_t *accepted) {
    if (!accepted) return PCM_INVALID;
    *accepted = 0;
    if (!client || !client->started) return PCM_INVALID;
    int status = pcm_ring_write(client->ring, client->capacity, client->frame_bytes,
                                data, frames, accepted);
    if (status != PCM_OK && status != PCM_WOULD_BLOCK) return status;
    // Signal the output worker and surface any failure from the prior drain.
    int output = command(client->track, u6::pump_track);
    if (output == PCM_OK) return status;
    client->started = false;
    return output == PCM_WOULD_BLOCK ? PCM_IO : output;
}

int u6_direct_position(struct u6_direct_client *client, uint64_t *presented, int64_t *time_ns) {
    if (!client || !client->started || !presented || !time_ns) return PCM_INVALID;
    Parcel request, reply;
    int32_t status;
    if (request.writeInterfaceToken(u6::track_descriptor()) != OK ||
        client->track->transact(u6::position_track, request, &reply) != OK ||
        reply.readInt32(&status) != OK || status < PCM_OK || status > PCM_WOULD_BLOCK)
        return PCM_IO;
    if (status != PCM_OK) return reply.dataAvail() == 0 ? status : PCM_IO;

    int64_t frames, stamp;
    if (reply.readInt64(&frames) != OK || reply.readInt64(&stamp) != OK ||
        reply.dataAvail() != 0 || frames < 0 || stamp <= 0) return PCM_IO;
    *presented = static_cast<uint64_t>(frames);
    *time_ns = stamp;
    return PCM_OK;
}

int u6_direct_close(struct u6_direct_client *client) {
    if (!client) return PCM_INVALID;
    int status = command(client->track, u6::stop_track);
    munmap(client->ring, client->mapping_size);
    delete client;
    return status;
}
