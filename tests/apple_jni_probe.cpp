#include <dlfcn.h>
#include <jni.h>
#include <stdint.h>

extern "C" JNIEXPORT jint JNICALL Java_U6BridgeProbe_install(JNIEnv *env, jclass, jstring path) {
    const char *filename = env->GetStringUTFChars(path, nullptr);
    if (!filename) return JNI_ERR;
    void *client = dlopen(filename, RTLD_NOW | RTLD_LOCAL);
    env->ReleaseStringUTFChars(path, filename);
    if (!client) return -10;
    using Register = int (*)(JNIEnv *, const char *, const JNINativeMethod *, int);
    auto adapter = reinterpret_cast<Register>(dlsym(client, "u6_register_audiotrack"));
    void *stock_register = dlsym(RTLD_DEFAULT,
        "_ZN7android14AndroidRuntime21registerNativeMethodsEP7_JNIEnvPKcPK15JNINativeMethodi");
    Dl_info runtime;
    if (!adapter || !stock_register || !dladdr(stock_register, &runtime)) return -11;

    // check_apple_jni.sh verifies the stock image hash before using these RVAs.
    // dli_fbase points to the first mapped LOAD, whose virtual address is nonzero.
    uintptr_t first_load = sizeof(void *) == 8 ? 0x2c000 : 0xf000;
    uintptr_t table_address = sizeof(void *) == 8 ? 0x1a9a40 : 0xe952c;
    auto *table = reinterpret_cast<const JNINativeMethod *>(
        reinterpret_cast<uintptr_t>(runtime.dli_fbase) - first_load + table_address);
    return adapter(env, "android/media/AudioTrack", table, 35);
}
