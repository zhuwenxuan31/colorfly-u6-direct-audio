#include "apple_track.h"

#include <dlfcn.h>
#include <jni.h>
#include <new>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

namespace {
constexpr jint kError = -1;
constexpr jint kBadValue = -2;
constexpr jint kInvalidOperation = -3;
constexpr jint kWouldBlock = -11;
constexpr jint kMusicStream = 3;
constexpr jint kModeStatic = 0;
constexpr jint kModeStream = 1;
constexpr jlong kRetired = 1;

struct Track {
    pcm_format format;
    jint java_encoding;
    jint frame_count;
    u6_direct_client *client;
    uint64_t position_base;
    float left, right;
};

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
jfieldID jni_data, stream_type, usage_field, content_field, flags_field;

class Guard {
public:
    Guard() { pthread_mutex_lock(&lock); }
    ~Guard() { pthread_mutex_unlock(&lock); }
};

bool music_track(JNIEnv *env, jobject attributes) {
    if (!attributes) return false;
    jint usage = env->GetIntField(attributes, usage_field);
    jint content = env->GetIntField(attributes, content_field);
    return u6_is_music_track(usage, content, env->GetIntField(attributes, flags_field));
}

class Entry {
public:
    Entry(JNIEnv *env, jobject thiz) : value_(env->GetLongField(thiz, jni_data)) {}
    bool direct() const { return (value_ & kRetired) != 0; }
    Track *get() const { return reinterpret_cast<Track *>(
        static_cast<uintptr_t>(value_ & ~kRetired)); }
private:
    jlong value_;
};

template <typename Fn> Fn original(void *function) { return reinterpret_cast<Fn>(function); }
using Setup = jint (*)(JNIEnv *, jobject, jobject, jobject, jint, jint, jint, jint, jint, jint,
                        jintArray);
using Control = void (*)(JNIEnv *, jobject);
using GetInt = jint (*)(JNIEnv *, jobject);
using Timestamp = jint (*)(JNIEnv *, jobject, jlongArray);
using SetVolume = void (*)(JNIEnv *, jobject, jfloat, jfloat);
using SetInt = jint (*)(JNIEnv *, jobject, jint);
using Reg = int (*)(JNIEnv *, const char *, const JNINativeMethod *, int);

Setup stock_setup;
Control stock_start, stock_stop, stock_pause, stock_flush, stock_release, stock_finalize;
GetInt stock_position, stock_frames, stock_latency, stock_rate;
Timestamp stock_timestamp;
SetVolume stock_volume;
SetInt stock_set_rate;

void playback_error(JNIEnv *env, const char *message) {
    jclass exception = env->FindClass("java/lang/IllegalStateException");
    if (exception) env->ThrowNew(exception, message);
}

jint setup(JNIEnv *env, jobject thiz, jobject weak, jobject attributes, jint rate,
           jint channels, jint index_mask, jint encoding, jint buffer_bytes, jint mode,
           jintArray session) {
    if (!music_track(env, attributes) ||
        (mode == kModeStatic && rate == 4000 && encoding == 2 &&
         (channels == 4 || channels == 12) && buffer_bytes == (channels == 4 ? 2 : 4))) {
        return stock_setup(env, thiz, weak, attributes, rate, channels, index_mask, encoding,
                           buffer_bytes, mode, session);
    }

    pcm_format format;
    if (mode != kModeStream || !apple_track_format(rate, channels, index_mask, encoding, &format) ||
        !session || env->GetArrayLength(session) != 1 || buffer_bytes <= 0)
        return kBadValue;
    size_t frame_bytes = pcm_frame_bytes(format);
    if (static_cast<size_t>(buffer_bytes) % frame_bytes != 0) return kBadValue;

    jint id = 0;
    env->GetIntArrayRegion(session, 0, 1, &id);
    if (env->ExceptionCheck()) return kError;
    if (!id) {
        // This allocates an ID only. It does not open an Android audio output.
        auto allocate = original<jint (*)()>(
            dlsym(RTLD_DEFAULT, "_ZN7android11AudioSystem16newAudioUniqueIdEv"));
        if (!allocate || (id = allocate()) <= 0) return kError;
        env->SetIntArrayRegion(session, 0, 1, &id);
        if (env->ExceptionCheck()) return kError;
    }

    Track *track = new (std::nothrow) Track{
        format, encoding, static_cast<jint>(buffer_bytes / frame_bytes), nullptr, 0, 1.0f, 1.0f};
    if (!track) return kError;
    env->SetIntField(thiz, stream_type, kMusicStream);
    env->SetLongField(thiz, jni_data,
                      static_cast<jlong>(reinterpret_cast<uintptr_t>(track)) | kRetired);
    return 0;
}

void close(Track *track, bool keep_position) {
    if (!track) return;
    if (track->client) {
        if (keep_position) {
            uint64_t frames;
            int64_t stamp;
            if (u6_direct_position(track->client, &frames, &stamp) == PCM_OK)
                track->position_base += frames;
        }
        u6_direct_close(track->client);
        track->client = nullptr;
    }
    if (!keep_position) track->position_base = 0;
}

void start(JNIEnv *env, jobject thiz) {
    {
        Guard guard;
        Entry entry(env, thiz);
        if (entry.direct()) {
            Track *track = entry.get();
            if (track && track->client) return;
            if (track && u6_direct_open(track->format, &track->client) == PCM_OK &&
                u6_direct_set_volume(track->client, track->left, track->right) == PCM_OK &&
                u6_direct_start(track->client) == PCM_OK)
                return;
            close(track, true);
            playback_error(env, "U6 direct output is unavailable");
            return;
        }
    }
    stock_start(env, thiz);
}

template <Control *Stock, bool KeepPosition, bool Release>
void stop(JNIEnv *env, jobject thiz) {
    {
        Guard guard;
        Entry entry(env, thiz);
        if (entry.direct()) {
            Track *track = entry.get();
            close(track, KeepPosition);
            if (Release) {
                env->SetLongField(thiz, jni_data, kRetired);
                delete track;
            }
            return;
        }
    }
    (*Stock)(env, thiz);
}

void flush(JNIEnv *env, jobject thiz) {
    {
        Guard guard;
        Entry entry(env, thiz);
        if (entry.direct()) {
            Track *track = entry.get();
            if (track && !track->client) track->position_base = 0;
            return;
        }
    }
    stock_flush(env, thiz);
}

template <class Array, class Value>
using ArrayWrite = jint (*)(JNIEnv *, jobject, Array, jint, jint, jint, jboolean);
ArrayWrite<jbyteArray, jbyte> stock_bytes;
ArrayWrite<jshortArray, jshort> stock_shorts;
ArrayWrite<jfloatArray, jfloat> stock_floats;
ArrayWrite<jobject, jbyte> stock_buffer;

jbyte *array_data(JNIEnv *env, jbyteArray data) { return env->GetByteArrayElements(data, nullptr); }
jshort *array_data(JNIEnv *env, jshortArray data) { return env->GetShortArrayElements(data, nullptr); }
jfloat *array_data(JNIEnv *env, jfloatArray data) { return env->GetFloatArrayElements(data, nullptr); }
void unpin(JNIEnv *env, jbyteArray data, jbyte *ptr) { env->ReleaseByteArrayElements(data, ptr, JNI_ABORT); }
void unpin(JNIEnv *env, jshortArray data, jshort *ptr) { env->ReleaseShortArrayElements(data, ptr, JNI_ABORT); }
void unpin(JNIEnv *env, jfloatArray data, jfloat *ptr) { env->ReleaseFloatArrayElements(data, ptr, JNI_ABORT); }

template <class Array, class Value>
jint write_array(JNIEnv *env, Track *track, Array data, jint offset, jint count) {
    if (!track || !track->client) return kInvalidOperation;
    if (!data || offset < 0 || count < 0 || offset > env->GetArrayLength(data) - count)
        return kBadValue;
    Value *ptr = array_data(env, data);
    if (!ptr) return kBadValue;
    jint result = apple_track_write(track->client, ptr + offset, count,
                                    pcm_frame_bytes(track->format), sizeof(Value));
    unpin(env, data, ptr);
    return result;
}

template <class Array, class Value, ArrayWrite<Array, Value> *Stock>
jint write(JNIEnv *env, jobject thiz, Array data, jint offset, jint count, jint encoding,
           jboolean blocking) {
    {
        Guard guard;
        Entry entry(env, thiz);
        if (entry.direct()) {
            Track *track = entry.get();
            if (!track) return kInvalidOperation;
            if (track->java_encoding != encoding) return kBadValue;
            return write_array<Array, Value>(env, track, data, offset, count);
        }
    }
    return (*Stock)(env, thiz, data, offset, count, encoding, blocking);
}

jint buffer(JNIEnv *env, jobject thiz, jobject data, jint offset, jint count, jint encoding,
            jboolean blocking) {
    {
        Guard guard;
        Entry entry(env, thiz);
        if (entry.direct()) {
            Track *track = entry.get();
            if (!track || !track->client) return kInvalidOperation;
            if (track->java_encoding != encoding || !data || offset < 0 || count < 0)
                return kBadValue;
            const auto *memory = static_cast<const uint8_t *>(env->GetDirectBufferAddress(data));
            if (memory) {
                if (static_cast<jlong>(offset) + count > env->GetDirectBufferCapacity(data))
                    return kBadValue;
                return apple_track_write(track->client, memory + offset, count,
                                         pcm_frame_bytes(track->format), 1);
            }
            jclass bytes = env->FindClass("[B");
            if (!bytes) return kError;
            bool byte_array = env->IsInstanceOf(data, bytes);
            env->DeleteLocalRef(bytes);
            return byte_array ? write_array<jbyteArray, jbyte>(env, track,
                       static_cast<jbyteArray>(data), offset, count) : kBadValue;
        }
    }
    return stock_buffer(env, thiz, data, offset, count, encoding, blocking);
}

jint position(JNIEnv *env, jobject thiz) {
    {
        Guard guard;
        Entry entry(env, thiz);
        if (entry.direct()) {
            Track *track = entry.get();
            if (!track) return kInvalidOperation;
            uint64_t frames = 0;
            int64_t stamp;
            if (track->client && u6_direct_position(track->client, &frames, &stamp) != PCM_OK)
                frames = 0;
            return static_cast<jint>(static_cast<uint32_t>(track->position_base + frames));
        }
    }
    return stock_position(env, thiz);
}

jint timestamp(JNIEnv *env, jobject thiz, jlongArray java_stamp) {
    {
        Guard guard;
        Entry entry(env, thiz);
        if (entry.direct()) {
            Track *track = entry.get();
            if (!track || !track->client) return kWouldBlock;
            if (!java_stamp || env->GetArrayLength(java_stamp) != 2) return kBadValue;
            uint64_t frames;
            int64_t stamp;
            if (u6_direct_position(track->client, &frames, &stamp) != PCM_OK) return kWouldBlock;
            jlong data[] = {static_cast<uint32_t>(track->position_base + frames), stamp};
            env->SetLongArrayRegion(java_stamp, 0, 2, data);
            return env->ExceptionCheck() ? kError : 0;
        }
    }
    return stock_timestamp(env, thiz, java_stamp);
}

enum class Query { Frames, Rate };
template <GetInt *Stock, Query Field>
jint track_value(JNIEnv *env, jobject thiz) {
    {
        Guard guard;
        Entry entry(env, thiz);
        if (entry.direct()) {
            Track *track = entry.get();
            if (!track) return kInvalidOperation;
            return Field == Query::Frames ? track->frame_count : track->format.rate;
        }
    }
    return (*Stock)(env, thiz);
}

jint latency(JNIEnv *env, jobject thiz) {
    {
        Guard guard;
        // Driver timestamps carry the latency. A synthetic latency can offset them twice.
        if (Entry(env, thiz).direct()) return 0;
    }
    return stock_latency(env, thiz);
}

jint rate(JNIEnv *env, jobject thiz, jint requested_rate) {
    {
        Guard guard;
        Entry entry(env, thiz);
        if (entry.direct()) {
            Track *track = entry.get();
            return track && requested_rate == static_cast<jint>(track->format.rate) ? 0 : kBadValue;
        }
    }
    return stock_set_rate(env, thiz, requested_rate);
}

void volume(JNIEnv *env, jobject thiz, jfloat left, jfloat right) {
    {
        Guard guard;
        Entry entry(env, thiz);
        if (entry.direct()) {
            Track *track = entry.get();
            if (!track || !(left >= 0.0f && left <= 1.0f && right >= 0.0f && right <= 1.0f)) {
                playback_error(env, "Invalid U6 direct track volume");
                return;
            }
            if (track->client && u6_direct_set_volume(track->client, left, right) != PCM_OK) {
                close(track, true);
                playback_error(env, "U6 hardware volume is unavailable");
                return;
            }
            track->left = left;
            track->right = right;
            return;
        }
    }
    stock_volume(env, thiz, left, right);
}

template <class Fn>
bool redirect(JNINativeMethod *table, int count, const char *name, const char *signature,
              Fn replacement, Fn *stock) {
    for (int i = 0; i < count; ++i) {
        if (strcmp(name, table[i].name) || strcmp(signature, table[i].signature)) continue;
        if (!table[i].fnPtr) return false;
        *stock = original<Fn>(table[i].fnPtr);
        table[i].fnPtr = reinterpret_cast<void *>(replacement);
        return true;
    }
    return false;
}
} // namespace

extern "C" int u6_register_audiotrack(JNIEnv *env, const char *name,
                                      const JNINativeMethod *methods, int count) {
    auto register_native = original<Reg>(dlsym(RTLD_DEFAULT,
        "_ZN7android14AndroidRuntime21registerNativeMethodsEP7_JNIEnvPKcPK15JNINativeMethodi"));
    if (!register_native || !name || !methods) return JNI_ERR;
    if (strcmp(name, "android/media/AudioTrack"))
        return register_native(env, name, methods, count);

    JNINativeMethod table[35];
    if (count != static_cast<int>(sizeof(table) / sizeof(table[0]))) return JNI_ERR;
    memcpy(table, methods, sizeof(table));
    jclass track = env->FindClass(name);
    if (!track) return JNI_ERR;
    jni_data = env->GetFieldID(track, "mJniData", "J");
    stream_type = jni_data ? env->GetFieldID(track, "mStreamType", "I") : nullptr;
    env->DeleteLocalRef(track);
    if (!stream_type) return JNI_ERR;
    jclass attributes = env->FindClass("android/media/AudioAttributes");
    if (!attributes) return JNI_ERR;
    usage_field = env->GetFieldID(attributes, "mUsage", "I");
    content_field = usage_field ? env->GetFieldID(attributes, "mContentType", "I") : nullptr;
    flags_field = content_field ? env->GetFieldID(attributes, "mFlags", "I") : nullptr;
    env->DeleteLocalRef(attributes);
    if (!flags_field) return JNI_ERR;

    const bool matched =
        redirect(table, count, "native_setup", "(Ljava/lang/Object;Ljava/lang/Object;IIIIII[I)I", setup, &stock_setup) &&
        redirect(table, count, "native_start", "()V", start, &stock_start) &&
        redirect(table, count, "native_stop", "()V", stop<&stock_stop, false, false>, &stock_stop) &&
        redirect(table, count, "native_pause", "()V", stop<&stock_pause, true, false>, &stock_pause) &&
        redirect(table, count, "native_flush", "()V", flush, &stock_flush) &&
        redirect(table, count, "native_release", "()V", stop<&stock_release, false, true>, &stock_release) &&
        redirect(table, count, "native_finalize", "()V", stop<&stock_finalize, false, true>, &stock_finalize) &&
        redirect(table, count, "native_write_byte", "([BIIIZ)I", write<jbyteArray, jbyte, &stock_bytes>, &stock_bytes) &&
        redirect(table, count, "native_write_short", "([SIIIZ)I", write<jshortArray, jshort, &stock_shorts>, &stock_shorts) &&
        redirect(table, count, "native_write_float", "([FIIIZ)I", write<jfloatArray, jfloat, &stock_floats>, &stock_floats) &&
        redirect(table, count, "native_write_native_bytes", "(Ljava/lang/Object;IIIZ)I", buffer, &stock_buffer) &&
        redirect(table, count, "native_get_position", "()I", position, &stock_position) &&
        redirect(table, count, "native_get_timestamp", "([J)I", timestamp, &stock_timestamp) &&
        redirect(table, count, "native_get_latency", "()I", latency, &stock_latency) &&
        redirect(table, count, "native_set_playback_rate", "(I)I", rate, &stock_set_rate) &&
        redirect(table, count, "native_setVolume", "(FF)V", volume, &stock_volume) &&
        redirect(table, count, "native_get_playback_rate", "()I", track_value<&stock_rate, Query::Rate>, &stock_rate) &&
        redirect(table, count, "native_get_native_frame_count", "()I",
                 track_value<&stock_frames, Query::Frames>, &stock_frames);
    return matched ? register_native(env, name, table, count) : JNI_ERR;
}
