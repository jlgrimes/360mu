# Task: Android Build & UI

## Project Context
You are working on 360μ, an Xbox 360 emulator for Android. The UI uses Jetpack Compose and the native code is built with NDK/CMake.

## Your Assignment
Get the Android app building, implement the UI, and ensure native code loads correctly.

## Current State
- Android project at `android/`
- Gradle build files configured
- Basic Compose UI screens exist
- JNI bridge at `native/src/jni/jni_bridge.cpp`
- Native lib not yet tested on Android

## Files to Complete

### 1. `android/app/build.gradle.kts`
```kotlin
// Verify NDK configuration:
android {
    ndkVersion = "25.2.9519653"  // Or latest stable
    
    defaultConfig {
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++20"
                arguments += "-DANDROID_STL=c++_shared"
                arguments += "-DX360MU_USE_VULKAN=ON"
            }
        }
        ndk {
            abiFilters += listOf("arm64-v8a")  // ARM64 only for JIT
        }
    }
    
    externalNativeBuild {
        cmake {
            path = file("../native/CMakeLists.txt")
        }
    }
}
```

### 2. `android/app/src/main/java/com/x360mu/ui/screens/GameScreen.kt`
```kotlin
@Composable
fun GameScreen(
    gamePath: String,
    onBack: () -> Unit
) {
    val emulator = remember { NativeEmulator() }
    var isRunning by remember { mutableStateOf(false) }
    
    // Vulkan surface for rendering
    AndroidView(
        factory = { context ->
            SurfaceView(context).apply {
                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(holder: SurfaceHolder) {
                        emulator.setSurface(holder.surface)
                        emulator.loadGame(gamePath)
                        isRunning = true
                    }
                    override fun surfaceDestroyed(holder: SurfaceHolder) {
                        isRunning = false
                        emulator.stop()
                    }
                    override fun surfaceChanged(...) {}
                })
            }
        },
        modifier = Modifier.fillMaxSize()
    )
    
    // Touch controls overlay
    TouchControls(
        onButton = { button, pressed -> emulator.setButton(0, button, pressed) },
        onStick = { stick, x, y -> emulator.setStick(0, stick, x, y) },
        onTrigger = { trigger, value -> emulator.setTrigger(0, trigger, value) }
    )
}
```

### 3. `android/app/src/main/java/com/x360mu/ui/components/TouchControls.kt`
```kotlin
@Composable
fun TouchControls(
    onButton: (Int, Boolean) -> Unit,
    onStick: (Int, Float, Float) -> Unit,
    onTrigger: (Int, Float) -> Unit
) {
    Box(modifier = Modifier.fillMaxSize()) {
        // Left stick (bottom left)
        VirtualStick(
            modifier = Modifier.align(Alignment.BottomStart),
            onMove = { x, y -> onStick(0, x, y) }
        )
        
        // Right stick (bottom right)
        VirtualStick(
            modifier = Modifier.align(Alignment.BottomEnd),
            onMove = { x, y -> onStick(1, x, y) }
        )
        
        // ABXY buttons
        ButtonCluster(
            modifier = Modifier.align(Alignment.CenterEnd),
            onButton = onButton
        )
        
        // D-pad
        DPad(
            modifier = Modifier.align(Alignment.CenterStart),
            onButton = onButton
        )
        
        // Triggers/bumpers at top
        // Start/Back in middle
    }
}
```

### 4. `android/app/src/main/java/com/x360mu/core/NativeEmulator.kt`
```kotlin
class NativeEmulator {
    companion object {
        init {
            System.loadLibrary("x360mu_jni")
        }
    }
    
    private var nativePtr: Long = 0
    
    fun initialize(dataPath: String, savePath: String): Boolean {
        nativePtr = nativeCreate()
        return nativeInit(nativePtr, dataPath, savePath)
    }
    
    fun loadGame(path: String): Boolean = nativeLoadGame(nativePtr, path)
    fun start() = nativeStart(nativePtr)
    fun stop() = nativeStop(nativePtr)
    fun pause() = nativePause(nativePtr)
    fun resume() = nativeResume(nativePtr)
    
    fun setSurface(surface: Surface) = nativeSetSurface(nativePtr, surface)
    
    // Input
    fun setButton(player: Int, button: Int, pressed: Boolean) = 
        nativeSetButton(nativePtr, player, button, pressed)
    fun setStick(player: Int, stick: Int, x: Float, y: Float) = 
        nativeSetStick(nativePtr, player, stick, x, y)
    fun setTrigger(player: Int, trigger: Int, value: Float) = 
        nativeSetTrigger(nativePtr, player, trigger, value)
    
    // Native methods
    private external fun nativeCreate(): Long
    private external fun nativeInit(ptr: Long, dataPath: String, savePath: String): Boolean
    private external fun nativeLoadGame(ptr: Long, path: String): Boolean
    private external fun nativeStart(ptr: Long)
    private external fun nativeStop(ptr: Long)
    private external fun nativePause(ptr: Long)
    private external fun nativeResume(ptr: Long)
    private external fun nativeSetSurface(ptr: Long, surface: Surface)
    private external fun nativeSetButton(ptr: Long, player: Int, button: Int, pressed: Boolean)
    private external fun nativeSetStick(ptr: Long, player: Int, stick: Int, x: Float, y: Float)
    private external fun nativeSetTrigger(ptr: Long, player: Int, trigger: Int, value: Float)
}
```

### 5. `native/src/jni/jni_bridge.cpp`
```cpp
// Complete JNI implementation:
extern "C" {

JNIEXPORT jlong JNICALL
Java_com_x360mu_core_NativeEmulator_nativeCreate(JNIEnv* env, jobject thiz) {
    auto* emu = new x360mu::Emulator();
    return reinterpret_cast<jlong>(emu);
}

JNIEXPORT jboolean JNICALL
Java_com_x360mu_core_NativeEmulator_nativeInit(
    JNIEnv* env, jobject thiz, jlong ptr, jstring dataPath, jstring savePath) {
    auto* emu = reinterpret_cast<x360mu::Emulator*>(ptr);
    
    const char* data = env->GetStringUTFChars(dataPath, nullptr);
    const char* save = env->GetStringUTFChars(savePath, nullptr);
    
    x360mu::EmulatorConfig config;
    config.data_path = data;
    config.save_path = save;
    
    auto status = emu->initialize(config);
    
    env->ReleaseStringUTFChars(dataPath, data);
    env->ReleaseStringUTFChars(savePath, save);
    
    return status == x360mu::Status::Ok;
}

JNIEXPORT void JNICALL
Java_com_x360mu_core_NativeEmulator_nativeSetSurface(
    JNIEnv* env, jobject thiz, jlong ptr, jobject surface) {
    auto* emu = reinterpret_cast<x360mu::Emulator*>(ptr);
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    emu->set_surface(window);
}

// ... implement all other native methods

}
```

## Build & Test
```bash
# Build Android APK:
cd android
./gradlew assembleDebug

# Install on device:
adb install app/build/outputs/apk/debug/app-debug.apk

# View logs:
adb logcat -s 360mu

# If native crash, get symbols:
ndk-stack -sym app/build/intermediates/cmake/debug/obj/arm64-v8a < crash.txt
```

## UI Theme
Use a gaming-focused dark theme:
- Primary: Deep purple (#6B4EE6)
- Background: Near black (#0D0D0D)
- Surface: Dark gray (#1A1A1A)
- Accent: Electric blue (#00D9FF)

## Success Criteria
1. App builds and installs on Android device
2. Native library loads without crash
3. Can browse and select game files
4. Touch controls render and send input
5. Vulkan surface created successfully

