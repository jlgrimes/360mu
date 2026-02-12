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
#include "x360mu/feature_flags.h"
#include "input/input_manager.h"
#include "kernel/xex_loader.h"
#include "kernel/kernel.h"
#include "kernel/game_info.h"
#include "core/crash_handler.h"
#include "core/log_buffer.h"
#include <string>
#include <memory>
#include <sstream>

#define LOG_TAG "360mu-jni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

using namespace x360mu;

// ============================================================================
// JavaVM cache for reverse JNI callbacks
// ============================================================================

static JavaVM* g_jvm = nullptr;
static jobject  g_vibration_listener = nullptr;   // Global ref to Java callback object
static jmethodID g_on_vibration_method = nullptr;  // onVibration(int player, int left, int right)

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /* reserved */) {
    g_jvm = vm;
    LOGI("JNI_OnLoad: JavaVM cached");
    return JNI_VERSION_1_6;
}

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
    config.enable_jit = enableJit;  // Re-enabled for debugging
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

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeTestRender(
    JNIEnv* env, jobject /* this */, jlong handle) {

    LOGI("nativeTestRender called");

    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (!emulator) {
        LOGE("nativeTestRender: invalid emulator handle!");
        return;
    }

    // Call test render on the GPU
    emulator->test_render();

    LOGI("nativeTestRender completed");
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
    if (emulator) {
        emulator->set_vsync(enabled == JNI_TRUE);
        LOGI("VSync set to %s", enabled ? "enabled" : "disabled");
    }
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSetFrameSkip(
    JNIEnv* env, jobject /* this */, jlong handle, jint count) {

    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (emulator) {
        emulator->set_frame_skip(static_cast<u32>(count));
        LOGI("Frame skip set to %d", count);
    }
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSetTargetFps(
    JNIEnv* env, jobject /* this */, jlong handle, jint fps) {

    auto* emulator = reinterpret_cast<Emulator*>(handle);
    if (emulator) {
        emulator->set_target_fps(static_cast<u32>(fps));
        LOGI("Target FPS set to %d", fps);
    }
}

// ============================================================================
// Feature Flags
// ============================================================================

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSetFeatureFlag(
    JNIEnv* env, jobject /* this */, jstring flagName, jboolean enabled) {
    
    std::string name = jstring_to_string(env, flagName);
    bool value = enabled == JNI_TRUE;
    
    LOGI("Setting feature flag '%s' = %s", name.c_str(), value ? "true" : "false");
    
    // Map flag names to actual flags
    if (name == "jit_trace_memory") {
        FeatureFlags::jit_trace_memory = value;
    } else if (name == "jit_trace_mirror_access") {
        FeatureFlags::jit_trace_mirror_access = value;
    } else if (name == "jit_trace_boundary_access") {
        FeatureFlags::jit_trace_boundary_access = value;
    } else if (name == "jit_trace_blocks") {
        FeatureFlags::jit_trace_blocks = value;
    } else if (name == "jit_trace_mmio") {
        FeatureFlags::jit_trace_mmio = value;
    } else if (name == "gpu_trace_registers") {
        FeatureFlags::gpu_trace_registers = value;
    } else if (name == "gpu_trace_shaders") {
        FeatureFlags::gpu_trace_shaders = value;
    } else if (name == "gpu_trace_draws") {
        FeatureFlags::gpu_trace_draws = value;
    } else if (name == "kernel_trace_syscalls") {
        FeatureFlags::kernel_trace_syscalls = value;
    } else if (name == "kernel_trace_threads") {
        FeatureFlags::kernel_trace_threads = value;
    } else if (name == "kernel_trace_files") {
        FeatureFlags::kernel_trace_files = value;
    } else if (name == "disable_fastmem") {
        FeatureFlags::disable_fastmem = value;
    } else if (name == "force_interpreter") {
        FeatureFlags::force_interpreter = value;
    } else {
        LOGE("Unknown feature flag: %s", name.c_str());
    }
}

JNIEXPORT jboolean JNICALL
Java_com_x360mu_core_NativeEmulator_nativeGetFeatureFlag(
    JNIEnv* env, jobject /* this */, jstring flagName) {

    std::string name = jstring_to_string(env, flagName);

    if (name == "jit_trace_memory") return FeatureFlags::jit_trace_memory ? JNI_TRUE : JNI_FALSE;
    if (name == "jit_trace_mirror_access") return FeatureFlags::jit_trace_mirror_access ? JNI_TRUE : JNI_FALSE;
    if (name == "jit_trace_boundary_access") return FeatureFlags::jit_trace_boundary_access ? JNI_TRUE : JNI_FALSE;
    if (name == "jit_trace_blocks") return FeatureFlags::jit_trace_blocks ? JNI_TRUE : JNI_FALSE;
    if (name == "jit_trace_mmio") return FeatureFlags::jit_trace_mmio ? JNI_TRUE : JNI_FALSE;
    if (name == "gpu_trace_registers") return FeatureFlags::gpu_trace_registers ? JNI_TRUE : JNI_FALSE;
    if (name == "gpu_trace_shaders") return FeatureFlags::gpu_trace_shaders ? JNI_TRUE : JNI_FALSE;
    if (name == "gpu_trace_draws") return FeatureFlags::gpu_trace_draws ? JNI_TRUE : JNI_FALSE;
    if (name == "kernel_trace_syscalls") return FeatureFlags::kernel_trace_syscalls ? JNI_TRUE : JNI_FALSE;
    if (name == "kernel_trace_threads") return FeatureFlags::kernel_trace_threads ? JNI_TRUE : JNI_FALSE;
    if (name == "kernel_trace_files") return FeatureFlags::kernel_trace_files ? JNI_TRUE : JNI_FALSE;
    if (name == "disable_fastmem") return FeatureFlags::disable_fastmem ? JNI_TRUE : JNI_FALSE;
    if (name == "force_interpreter") return FeatureFlags::force_interpreter ? JNI_TRUE : JNI_FALSE;

    LOGE("Unknown feature flag: %s", name.c_str());
    return JNI_FALSE;
}

// ============================================================================
// Input - Touch Controls
// ============================================================================

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeOnTouchDown(
    JNIEnv* env, jobject /* this */, jlong handle,
    jint player, jint pointerId, jfloat x, jfloat y,
    jfloat screenWidth, jfloat screenHeight) {

    get_input_manager().on_touch_down(
        static_cast<u32>(player),
        static_cast<s32>(pointerId),
        static_cast<f32>(x), static_cast<f32>(y),
        static_cast<f32>(screenWidth), static_cast<f32>(screenHeight)
    );
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeOnTouchMove(
    JNIEnv* env, jobject /* this */, jlong handle,
    jint player, jint pointerId, jfloat x, jfloat y,
    jfloat screenWidth, jfloat screenHeight) {

    get_input_manager().on_touch_move(
        static_cast<u32>(player),
        static_cast<s32>(pointerId),
        static_cast<f32>(x), static_cast<f32>(y),
        static_cast<f32>(screenWidth), static_cast<f32>(screenHeight)
    );
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeOnTouchUp(
    JNIEnv* env, jobject /* this */, jlong handle,
    jint player, jint pointerId) {

    get_input_manager().on_touch_up(
        static_cast<u32>(player),
        static_cast<s32>(pointerId)
    );
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSetControllerConnected(
    JNIEnv* env, jobject /* this */, jlong handle,
    jint player, jboolean connected) {

    get_input_manager().set_controller_connected(
        static_cast<u32>(player),
        connected == JNI_TRUE
    );
}

JNIEXPORT jint JNICALL
Java_com_x360mu_core_NativeEmulator_nativeGetVibrationLeft(
    JNIEnv* env, jobject /* this */, jlong handle, jint player) {

    auto vib = get_input_manager().get_vibration(static_cast<u32>(player));
    return static_cast<jint>(vib.left_motor_speed);
}

JNIEXPORT jint JNICALL
Java_com_x360mu_core_NativeEmulator_nativeGetVibrationRight(
    JNIEnv* env, jobject /* this */, jlong handle, jint player) {

    auto vib = get_input_manager().get_vibration(static_cast<u32>(player));
    return static_cast<jint>(vib.right_motor_speed);
}

// ============================================================================
// Input - Dead Zone Configuration
// ============================================================================

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSetStickDeadZone(
    JNIEnv* env, jobject /* this */, jlong handle,
    jint stickId, jfloat inner, jfloat outer) {

    get_input_manager().set_stick_dead_zone(
        static_cast<u32>(stickId),
        static_cast<f32>(inner),
        static_cast<f32>(outer)
    );
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSetTriggerDeadZone(
    JNIEnv* env, jobject /* this */, jlong handle, jfloat threshold) {

    get_input_manager().set_trigger_dead_zone(static_cast<f32>(threshold));
}

// ============================================================================
// Vibration Callback Registration
// ============================================================================

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSetVibrationListener(
    JNIEnv* env, jobject /* this */, jlong handle, jobject listener) {

    // Clear previous listener
    if (g_vibration_listener) {
        env->DeleteGlobalRef(g_vibration_listener);
        g_vibration_listener = nullptr;
        g_on_vibration_method = nullptr;
    }

    if (listener) {
        g_vibration_listener = env->NewGlobalRef(listener);

        jclass cls = env->GetObjectClass(listener);
        g_on_vibration_method = env->GetMethodID(cls, "onVibration", "(III)V");
        env->DeleteLocalRef(cls);

        if (!g_on_vibration_method) {
            LOGE("Failed to find onVibration(III)V method on listener");
            env->DeleteGlobalRef(g_vibration_listener);
            g_vibration_listener = nullptr;
            return;
        }

        LOGI("Vibration listener registered");

        // Register native callback with InputManager
        get_input_manager().set_vibration_callback(
            [](u32 player, u16 left_motor, u16 right_motor) {
                if (!g_jvm || !g_vibration_listener || !g_on_vibration_method) return;

                JNIEnv* cb_env = nullptr;
                bool attached = false;
                jint result = g_jvm->GetEnv(reinterpret_cast<void**>(&cb_env), JNI_VERSION_1_6);

                if (result == JNI_EDETACHED) {
                    if (g_jvm->AttachCurrentThread(&cb_env, nullptr) == JNI_OK) {
                        attached = true;
                    } else {
                        return;
                    }
                } else if (result != JNI_OK) {
                    return;
                }

                cb_env->CallVoidMethod(g_vibration_listener, g_on_vibration_method,
                    static_cast<jint>(player),
                    static_cast<jint>(left_motor),
                    static_cast<jint>(right_motor));

                if (cb_env->ExceptionCheck()) {
                    cb_env->ExceptionClear();
                }

                if (attached) {
                    g_jvm->DetachCurrentThread();
                }
            }
        );
    } else {
        get_input_manager().set_vibration_callback(nullptr);
        LOGI("Vibration listener cleared");
    }
}

// ============================================================================
// Game Info
// ============================================================================

// Helper: serialize XEX module info as pipe-delimited string matching Kotlin GameInfo parser
// Format: titleId|mediaId|version|baseVersion|discNumber|discCount|platform|executableType|
//         savegameId|gameRegion|baseAddress|entryPoint|imageSize|stackSize|heapSize|
//         moduleName|sectionCount|importLibs|totalImports|resolvedImports
static std::string serialize_xex_module_info(
    const XexModule& mod,
    const Emulator* emulator)
{
    std::ostringstream ss;

    auto& ei = mod.execution_info;
    auto& si = mod.security_info;

    // [0] titleId
    ss << std::hex << ei.title_id << "|";
    // [1] mediaId
    ss << std::hex << ei.media_id << "|";
    // [2] version
    ss << std::dec << ((ei.version >> 24) & 0xFF) << "."
       << ((ei.version >> 16) & 0xFF) << "."
       << ((ei.version >> 8) & 0xFF) << "."
       << (ei.version & 0xFF) << "|";
    // [3] baseVersion
    ss << std::dec << ((ei.base_version >> 24) & 0xFF) << "."
       << ((ei.base_version >> 16) & 0xFF) << "."
       << ((ei.base_version >> 8) & 0xFF) << "."
       << (ei.base_version & 0xFF) << "|";
    // [4] discNumber
    ss << static_cast<int>(ei.disc_number) << "|";
    // [5] discCount
    ss << static_cast<int>(ei.disc_count) << "|";
    // [6] platform
    ss << static_cast<int>(ei.platform) << "|";
    // [7] executableType
    ss << static_cast<int>(ei.executable_type) << "|";
    // [8] savegameId
    ss << std::hex << ei.savegame_id << "|";
    // [9] gameRegion
    ss << region_to_string(si.game_region) << "|";
    // [10] baseAddress
    ss << std::hex << mod.base_address << "|";
    // [11] entryPoint
    ss << std::hex << mod.entry_point << "|";
    // [12] imageSize
    ss << std::dec << mod.image_size << "|";
    // [13] stackSize
    ss << std::dec << mod.default_stack_size << "|";
    // [14] heapSize
    ss << std::dec << mod.default_heap_size << "|";
    // [15] moduleName
    ss << mod.name << "|";
    // [16] sectionCount
    ss << mod.sections.size() << "|";

    // [17] importLibraries: comma-separated name:count pairs
    bool first = true;
    u32 total_imports = 0;
    for (const auto& lib : mod.imports) {
        if (!first) ss << ",";
        ss << lib.name << ":" << lib.imports.size();
        total_imports += static_cast<u32>(lib.imports.size());
        first = false;
    }
    ss << "|";

    // [18] totalImports
    ss << total_imports << "|";

    // [19] resolvedImports — check against HLE table if emulator is available
    u32 resolved = 0;
    if (emulator && emulator->get_game_info()) {
        resolved = emulator->get_game_info()->total_implemented;
    }
    ss << resolved;

    return ss.str();
}

JNIEXPORT jstring JNICALL
Java_com_x360mu_core_NativeEmulator_nativeGetGameInfo(
    JNIEnv* env, jobject /* this */, jlong handle, jstring path) {

    auto* emulator = reinterpret_cast<Emulator*>(handle);

    // First try: if the game is already loaded, use cached GameInfo
    if (emulator && emulator->get_game_info()) {
        // Use the loaded module from kernel
        const Kernel* kernel = emulator->kernel();
        if (kernel) {
            // We need the XexModule — re-parse from the path to get full metadata
            // But if we have a loaded GameInfo, construct from that
            const GameInfo* gi = emulator->get_game_info();

            std::ostringstream ss;
            ss << std::hex << gi->title_id << "|";
            ss << std::hex << gi->media_id << "|";
            ss << std::dec << ((gi->version >> 24) & 0xFF) << "."
               << ((gi->version >> 16) & 0xFF) << "."
               << ((gi->version >> 8) & 0xFF) << "."
               << (gi->version & 0xFF) << "|";
            ss << std::dec << ((gi->base_version >> 24) & 0xFF) << "."
               << ((gi->base_version >> 16) & 0xFF) << "."
               << ((gi->base_version >> 8) & 0xFF) << "."
               << (gi->base_version & 0xFF) << "|";
            ss << static_cast<int>(gi->disc_number) << "|";
            ss << static_cast<int>(gi->disc_count) << "|";
            ss << "0|0|0|";  // platform, executableType, savegameId — from GameInfo
            ss << region_to_string(gi->game_region) << "|";
            ss << std::hex << gi->base_address << "|";
            ss << std::hex << gi->entry_point << "|";
            ss << std::dec << gi->image_size << "|";
            ss << std::dec << gi->default_stack_size << "|";
            ss << std::dec << gi->default_heap_size << "|";
            ss << gi->module_name << "|";
            ss << "0|";  // sectionCount not stored in GameInfo

            // Import libraries
            bool first = true;
            for (const auto& lib : gi->import_libraries) {
                if (!first) ss << ",";
                ss << lib.library_name << ":" << lib.total_imports;
                first = false;
            }
            ss << "|";
            ss << gi->total_imports << "|";
            ss << gi->total_implemented;

            return env->NewStringUTF(ss.str().c_str());
        }
    }

    // Fallback: parse XEX from path
    std::string filePath = jstring_to_string(env, path);
    if (filePath.empty()) {
        return env->NewStringUTF("");
    }

    XexLoader loader;
    Status status = loader.load_file(filePath, nullptr);
    if (status != Status::Ok) {
        return env->NewStringUTF("");
    }

    const XexModule* mod = loader.get_module();
    if (!mod) {
        return env->NewStringUTF("");
    }

    return env->NewStringUTF(serialize_xex_module_info(*mod, emulator).c_str());
}

// ============================================================================
// Crash handler and log buffer JNI methods
// ============================================================================

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeInstallCrashHandler(
        JNIEnv* env, jclass /* clazz */, jlong handle, jstring crashDir) {
    auto* emulator = reinterpret_cast<Emulator*>(handle);
    std::string dir = jstring_to_string(env, crashDir);
    install_crash_handler(dir, emulator);
    LOGI("Crash handler installed: %s", dir.c_str());
}

JNIEXPORT jint JNICALL
Java_com_x360mu_core_NativeEmulator_nativeGetLogCount(
        JNIEnv* /* env */, jclass /* clazz */) {
    return static_cast<jint>(LogBuffer::instance().total_entries());
}

JNIEXPORT jstring JNICALL
Java_com_x360mu_core_NativeEmulator_nativeGetLogs(
        JNIEnv* env, jclass /* clazz */, jint severityMin, jint component) {
    auto severity = static_cast<LogSeverity>(severityMin);
    auto entries = LogBuffer::instance().get_filtered(severity, component);

    // Format as pipe-separated lines: timestamp|severity|component|message
    std::string result;
    result.reserve(entries.size() * 80);
    for (const auto& e : entries) {
        result += std::to_string(e.timestamp_ms);
        result += '|';
        result += std::to_string(static_cast<int>(e.severity));
        result += '|';
        result += std::to_string(static_cast<int>(e.component));
        result += '|';
        result += e.message;
        result += '\n';
    }
    return env->NewStringUTF(result.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_x360mu_core_NativeEmulator_nativeExportLogs(
        JNIEnv* env, jclass /* clazz */) {
    std::string text = LogBuffer::instance().export_text();
    return env->NewStringUTF(text.c_str());
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeClearLogs(
        JNIEnv* /* env */, jclass /* clazz */) {
    LogBuffer::instance().clear();
}

JNIEXPORT jobjectArray JNICALL
Java_com_x360mu_core_NativeEmulator_nativeListCrashLogs(
        JNIEnv* env, jclass /* clazz */, jstring crashDir) {
    std::string dir = jstring_to_string(env, crashDir);
    auto logs = list_crash_logs(dir);

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray(logs.size(), stringClass, nullptr);
    for (size_t i = 0; i < logs.size(); i++) {
        env->SetObjectArrayElement(result, i, env->NewStringUTF(logs[i].c_str()));
    }
    return result;
}

JNIEXPORT jstring JNICALL
Java_com_x360mu_core_NativeEmulator_nativeReadCrashLog(
        JNIEnv* env, jclass /* clazz */, jstring path) {
    std::string p = jstring_to_string(env, path);
    std::string content = read_crash_log(p);
    return env->NewStringUTF(content.c_str());
}

} // extern "C"

