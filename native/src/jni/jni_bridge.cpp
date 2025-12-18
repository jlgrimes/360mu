/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * JNI bridge - Native interface for Android app
 */

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>

#include "x360mu/emulator.h"
#include <string>
#include <memory>

#define LOG_TAG "360mu-jni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

using namespace x360mu;

// Helper to convert jstring to std::string
static std::string jstring_to_string(JNIEnv* env, jstring jstr) {
    if (!jstr) return "";
    
    const char* chars = env->GetStringUTFChars(jstr, nullptr);
    std::string result(chars);
    env->ReleaseStringUTFChars(jstr, chars);
    return result;
}

// Helper to throw Java exception
static void throw_exception(JNIEnv* env, const char* message) {
    jclass cls = env->FindClass("java/lang/RuntimeException");
    if (cls) {
        env->ThrowNew(cls, message);
        env->DeleteLocalRef(cls);
    }
}

extern "C" {

// ============================================================================
// Emulator lifecycle
// ============================================================================

JNIEXPORT jlong JNICALL
Java_com_x360mu_core_NativeEmulator_nativeCreate(JNIEnv* env, jobject /* this */) {
    LOGI("Creating emulator instance");
    
    auto* emulator = new Emulator();
    return reinterpret_cast<jlong>(emulator);
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeDestroy(JNIEnv* env, jobject /* this */, jlong handle) {
    LOGI("Destroying emulator instance");
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (emulator) {
        emulator->shutdown();
        delete emulator;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_x360mu_core_NativeEmulator_nativeInitialize(
    JNIEnv* env, jobject /* this */, jlong handle,
    jstring dataPath, jstring cachePath, jstring savePath,
    jboolean enableJit, jboolean enableVulkan) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (!emulator) {
        throw_exception(env, "Invalid emulator handle");
        return JNI_FALSE;
    }
    
    EmulatorConfig config;
    config.data_path = jstring_to_string(env, dataPath);
    config.cache_path = jstring_to_string(env, cachePath);
    config.save_path = jstring_to_string(env, savePath);
    config.enable_jit = enableJit;
    config.use_vulkan = enableVulkan;
    
    LOGI("Initializing emulator:");
    LOGI("  Data path: %s", config.data_path.c_str());
    LOGI("  Cache path: %s", config.cache_path.c_str());
    LOGI("  Save path: %s", config.save_path.c_str());
    LOGI("  JIT: %s", config.enable_jit ? "enabled" : "disabled");
    LOGI("  Vulkan: %s", config.use_vulkan ? "enabled" : "disabled");
    
    Status status = emulator->initialize(config);
    if (status != Status::Ok) {
        LOGE("Failed to initialize emulator: %s", status_to_string(status));
        return JNI_FALSE;
    }
    
    LOGI("Emulator initialized successfully");
    return JNI_TRUE;
}

// ============================================================================
// Game loading
// ============================================================================

JNIEXPORT jboolean JNICALL
Java_com_x360mu_core_NativeEmulator_nativeLoadGame(
    JNIEnv* env, jobject /* this */, jlong handle, jstring path) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (!emulator) {
        throw_exception(env, "Invalid emulator handle");
        return JNI_FALSE;
    }
    
    std::string gamePath = jstring_to_string(env, path);
    LOGI("Loading game: %s", gamePath.c_str());
    
    Status status = emulator->load_game(gamePath);
    if (status != Status::Ok) {
        LOGE("Failed to load game: %s", status_to_string(status));
        return JNI_FALSE;
    }
    
    LOGI("Game loaded successfully");
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeUnloadGame(
    JNIEnv* env, jobject /* this */, jlong handle) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (emulator) {
        LOGI("Unloading game");
        emulator->unload_game();
    }
}

// ============================================================================
// Execution control
// ============================================================================

JNIEXPORT jboolean JNICALL
Java_com_x360mu_core_NativeEmulator_nativeRun(
    JNIEnv* env, jobject /* this */, jlong handle) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (!emulator) {
        throw_exception(env, "Invalid emulator handle");
        return JNI_FALSE;
    }
    
    Status status = emulator->run();
    return status == Status::Ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativePause(
    JNIEnv* env, jobject /* this */, jlong handle) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (emulator) {
        emulator->pause();
    }
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeStop(
    JNIEnv* env, jobject /* this */, jlong handle) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (emulator) {
        emulator->stop();
    }
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeReset(
    JNIEnv* env, jobject /* this */, jlong handle) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (emulator) {
        emulator->reset();
    }
}

JNIEXPORT jint JNICALL
Java_com_x360mu_core_NativeEmulator_nativeGetState(
    JNIEnv* env, jobject /* this */, jlong handle) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (!emulator) {
        return static_cast<jint>(EmulatorState::Uninitialized);
    }
    return static_cast<jint>(emulator->get_state());
}

// ============================================================================
// Display
// ============================================================================

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSetSurface(
    JNIEnv* env, jobject /* this */, jlong handle, jobject surface) {
    
    LOGI("nativeSetSurface called, handle=%p, surface=%p", (void*)handle, (void*)surface);
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (!emulator) {
        LOGE("nativeSetSurface: invalid emulator handle!");
        return;
    }
    
    if (surface) {
        ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
        if (window) {
            int width = ANativeWindow_getWidth(window);
            int height = ANativeWindow_getHeight(window);
            LOGI("Setting surface: window=%p, size=%dx%d", (void*)window, width, height);
            emulator->set_surface(window);
            
            // Test render - clear screen to a color to verify Vulkan is working
            LOGI("Performing test render...");
            if (emulator->gpu()) {
                // Try to present a frame to verify the pipeline works
                LOGI("GPU available, test render possible");
            } else {
                LOGE("GPU not available for test render");
            }
        } else {
            LOGE("Failed to get ANativeWindow from surface!");
        }
    } else {
        LOGI("Clearing surface");
        emulator->set_surface(nullptr);
    }
    
    LOGI("nativeSetSurface completed");
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeResizeSurface(
    JNIEnv* env, jobject /* this */, jlong handle, jint width, jint height) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (emulator) {
        LOGD("Resizing surface to %dx%d", width, height);
        emulator->resize_surface(static_cast<u32>(width), static_cast<u32>(height));
    }
}

// ============================================================================
// Input
// ============================================================================

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSetButton(
    JNIEnv* env, jobject /* this */, jlong handle,
    jint player, jint button, jboolean pressed) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (emulator) {
        emulator->set_button(
            static_cast<u32>(player),
            static_cast<u32>(button),
            pressed == JNI_TRUE
        );
    }
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSetTrigger(
    JNIEnv* env, jobject /* this */, jlong handle,
    jint player, jint trigger, jfloat value) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (emulator) {
        emulator->set_trigger(
            static_cast<u32>(player),
            static_cast<u32>(trigger),
            static_cast<f32>(value)
        );
    }
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSetStick(
    JNIEnv* env, jobject /* this */, jlong handle,
    jint player, jint stick, jfloat x, jfloat y) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (emulator) {
        emulator->set_stick(
            static_cast<u32>(player),
            static_cast<u32>(stick),
            static_cast<f32>(x),
            static_cast<f32>(y)
        );
    }
}

// ============================================================================
// Save states
// ============================================================================

JNIEXPORT jboolean JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSaveState(
    JNIEnv* env, jobject /* this */, jlong handle, jstring path) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (!emulator) return JNI_FALSE;
    
    std::string statePath = jstring_to_string(env, path);
    Status status = emulator->save_state(statePath);
    return status == Status::Ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_x360mu_core_NativeEmulator_nativeLoadState(
    JNIEnv* env, jobject /* this */, jlong handle, jstring path) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (!emulator) return JNI_FALSE;
    
    std::string statePath = jstring_to_string(env, path);
    Status status = emulator->load_state(statePath);
    return status == Status::Ok ? JNI_TRUE : JNI_FALSE;
}

// ============================================================================
// Statistics
// ============================================================================

JNIEXPORT jdouble JNICALL
Java_com_x360mu_core_NativeEmulator_nativeGetFps(
    JNIEnv* env, jobject /* this */, jlong handle) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (!emulator) return 0.0;
    return emulator->get_stats().fps;
}

JNIEXPORT jdouble JNICALL
Java_com_x360mu_core_NativeEmulator_nativeGetFrameTime(
    JNIEnv* env, jobject /* this */, jlong handle) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (!emulator) return 0.0;
    return emulator->get_stats().frame_time_ms;
}

// ============================================================================
// Settings
// ============================================================================

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSetResolutionScale(
    JNIEnv* env, jobject /* this */, jlong handle, jint scale) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (emulator && emulator->gpu()) {
        // TODO: Implement resolution scale setting
        LOGD("Setting resolution scale to %dx", scale);
    }
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSetVsync(
    JNIEnv* env, jobject /* this */, jlong handle, jboolean enabled) {
    
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (emulator && emulator->gpu()) {
        // TODO: Implement vsync setting
        LOGD("Setting vsync to %s", enabled ? "enabled" : "disabled");
    }
}

} // extern "C"

