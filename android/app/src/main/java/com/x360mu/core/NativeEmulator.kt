package com.x360mu.core

import android.util.Log
import android.view.Surface

private const val TAG = "360mu-NativeEmulator"

/**
 * Native emulator interface
 * 
 * This class wraps the native C++ emulator core via JNI.
 */
class NativeEmulator : AutoCloseable {
    
    private var nativeHandle: Long = 0
    
    /**
     * Emulator states matching native EmulatorState enum
     */
    enum class State(val value: Int) {
        UNINITIALIZED(0),
        READY(1),
        LOADED(2),
        RUNNING(3),
        PAUSED(4),
        STOPPED(5),
        ERROR(6);
        
        companion object {
            fun fromInt(value: Int) = entries.find { it.value == value } ?: UNINITIALIZED
        }
    }
    
    /**
     * Xbox 360 controller buttons
     */
    object Button {
        const val A = 0
        const val B = 1
        const val X = 2
        const val Y = 3
        const val DPAD_UP = 4
        const val DPAD_DOWN = 5
        const val DPAD_LEFT = 6
        const val DPAD_RIGHT = 7
        const val START = 8
        const val BACK = 9
        const val LEFT_BUMPER = 10
        const val RIGHT_BUMPER = 11
        const val LEFT_STICK = 12
        const val RIGHT_STICK = 13
        const val GUIDE = 14
    }
    
    object Trigger {
        const val LEFT = 0
        const val RIGHT = 1
    }
    
    object Stick {
        const val LEFT = 0
        const val RIGHT = 1
    }
    
    init {
        Log.i(TAG, "NativeEmulator init block - creating native instance")
        if (!libraryLoaded) {
            Log.e(TAG, "Native library not loaded! Error: $libraryError")
            throw RuntimeException("Native library not loaded: $libraryError")
        }
        try {
            nativeHandle = nativeCreate()
            Log.i(TAG, "Native emulator created, handle=$nativeHandle")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to create native emulator: ${e.message}", e)
            throw e
        }
    }
    
    /**
     * Initialize the emulator with configuration
     */
    fun initialize(
        dataPath: String,
        cachePath: String,
        savePath: String,
        enableJit: Boolean = true,
        enableVulkan: Boolean = true
    ): Boolean {
        Log.i(TAG, "initialize() called")
        Log.i(TAG, "  dataPath: $dataPath")
        Log.i(TAG, "  cachePath: $cachePath")
        Log.i(TAG, "  savePath: $savePath")
        Log.i(TAG, "  enableJit: $enableJit")
        Log.i(TAG, "  enableVulkan: $enableVulkan")
        
        val result = nativeInitialize(nativeHandle, dataPath, cachePath, savePath, enableJit, enableVulkan)
        Log.i(TAG, "initialize() returned: $result")
        return result
    }
    
    /**
     * Load a game file
     */
    fun loadGame(path: String): Boolean {
        Log.i(TAG, "loadGame() called with path: $path")
        val result = nativeLoadGame(nativeHandle, path)
        Log.i(TAG, "loadGame() returned: $result, state: $state")
        return result
    }
    
    /**
     * Unload the current game
     */
    fun unloadGame() {
        nativeUnloadGame(nativeHandle)
    }
    
    /**
     * Start/resume emulation
     */
    fun run(): Boolean {
        Log.i(TAG, "run() called, current state: $state")
        val result = nativeRun(nativeHandle)
        Log.i(TAG, "run() returned: $result, new state: $state")
        return result
    }
    
    /**
     * Pause emulation
     */
    fun pause() {
        nativePause(nativeHandle)
    }
    
    /**
     * Stop emulation
     */
    fun stop() {
        nativeStop(nativeHandle)
    }
    
    /**
     * Reset the emulated system
     */
    fun reset() {
        nativeReset(nativeHandle)
    }
    
    /**
     * Get current emulator state
     */
    val state: State
        get() = State.fromInt(nativeGetState(nativeHandle))
    
    val isRunning: Boolean
        get() = state == State.RUNNING
    
    val isPaused: Boolean
        get() = state == State.PAUSED
    
    val isGameLoaded: Boolean
        get() = state.value >= State.LOADED.value
    
    /**
     * Set the rendering surface
     */
    fun setSurface(surface: Surface?) {
        Log.i(TAG, "setSurface() called with surface: ${surface?.toString() ?: "null"}")
        nativeSetSurface(nativeHandle, surface)
        Log.i(TAG, "setSurface() completed")
    }
    
    /**
     * Notify of surface size change
     */
    fun resizeSurface(width: Int, height: Int) {
        Log.i(TAG, "resizeSurface() called with ${width}x${height}")
        nativeResizeSurface(nativeHandle, width, height)
    }

    /**
     * Test render - draws a cyan screen to verify rendering pipeline works
     */
    fun testRender() {
        Log.i(TAG, "testRender() called")
        nativeTestRender(nativeHandle)
        Log.i(TAG, "testRender() completed")
    }
    
    // Input methods
    fun setButton(player: Int, button: Int, pressed: Boolean) {
        nativeSetButton(nativeHandle, player, button, pressed)
    }

    fun setTrigger(player: Int, trigger: Int, value: Float) {
        nativeSetTrigger(nativeHandle, player, trigger, value)
    }

    fun setStick(player: Int, stick: Int, x: Float, y: Float) {
        nativeSetStick(nativeHandle, player, stick, x, y)
    }

    // Touch input (passes raw touch coordinates to native touch-to-controller mapping)
    fun onTouchDown(player: Int, pointerId: Int, x: Float, y: Float, screenWidth: Float, screenHeight: Float) {
        nativeOnTouchDown(nativeHandle, player, pointerId, x, y, screenWidth, screenHeight)
    }

    fun onTouchMove(player: Int, pointerId: Int, x: Float, y: Float, screenWidth: Float, screenHeight: Float) {
        nativeOnTouchMove(nativeHandle, player, pointerId, x, y, screenWidth, screenHeight)
    }

    fun onTouchUp(player: Int, pointerId: Int) {
        nativeOnTouchUp(nativeHandle, player, pointerId)
    }

    // Physical controller connection
    fun setControllerConnected(player: Int, connected: Boolean) {
        nativeSetControllerConnected(nativeHandle, player, connected)
    }

    // Vibration feedback (read by polling from game thread)
    fun getVibrationLeft(player: Int): Int = nativeGetVibrationLeft(nativeHandle, player)
    fun getVibrationRight(player: Int): Int = nativeGetVibrationRight(nativeHandle, player)

    /**
     * Register a vibration listener that receives callbacks from native XInputSetState.
     * Pass null to unregister.
     */
    fun setVibrationListener(listener: VibrationManager.VibrationListener?) {
        Log.i(TAG, "setVibrationListener: ${if (listener != null) "registering" else "clearing"}")
        nativeSetVibrationListener(nativeHandle, listener)
    }

    // Dead zone configuration
    fun setStickDeadZone(stickId: Int, inner: Float, outer: Float = 0.95f) {
        nativeSetStickDeadZone(nativeHandle, stickId, inner, outer)
    }

    fun setTriggerDeadZone(threshold: Float) {
        nativeSetTriggerDeadZone(nativeHandle, threshold)
    }

    /**
     * Map Android KeyEvent keyCode to Xbox 360 button index.
     * Returns -1 if unmapped.
     */
    fun mapKeyCodeToButton(keyCode: Int): Int {
        return when (keyCode) {
            96 /* KEYCODE_BUTTON_A */ -> Button.A
            97 /* KEYCODE_BUTTON_B */ -> Button.B
            99 /* KEYCODE_BUTTON_X */ -> Button.X
            100 /* KEYCODE_BUTTON_Y */ -> Button.Y
            19 /* KEYCODE_DPAD_UP */ -> Button.DPAD_UP
            20 /* KEYCODE_DPAD_DOWN */ -> Button.DPAD_DOWN
            21 /* KEYCODE_DPAD_LEFT */ -> Button.DPAD_LEFT
            22 /* KEYCODE_DPAD_RIGHT */ -> Button.DPAD_RIGHT
            108 /* KEYCODE_BUTTON_START */ -> Button.START
            4 /* KEYCODE_BACK */, 109 /* KEYCODE_BUTTON_SELECT */ -> Button.BACK
            102 /* KEYCODE_BUTTON_L1 */ -> Button.LEFT_BUMPER
            103 /* KEYCODE_BUTTON_R1 */ -> Button.RIGHT_BUMPER
            106 /* KEYCODE_BUTTON_THUMBL */ -> Button.LEFT_STICK
            107 /* KEYCODE_BUTTON_THUMBR */ -> Button.RIGHT_STICK
            110 /* KEYCODE_BUTTON_MODE */ -> Button.GUIDE
            else -> -1
        }
    }
    
    // Game info / compatibility
    data class GameInfo(
        val titleId: String,
        val mediaId: String,
        val version: String,
        val baseVersion: String,
        val discNumber: Int,
        val discCount: Int,
        val platform: Int,
        val executableType: Int,
        val savegameId: String,
        val gameRegion: String,
        val baseAddress: String,
        val entryPoint: String,
        val imageSize: Long,
        val stackSize: Long,
        val heapSize: Long,
        val moduleName: String,
        val sectionCount: Int,
        val importLibraries: Map<String, Int>,  // lib name -> import count
        val totalImports: Int,
        val resolvedImports: Int
    ) {
        val compatibilityPercent: Int
            get() = if (totalImports > 0) (resolvedImports * 100 / totalImports) else 0

        val compatibilityRating: String
            get() = when {
                compatibilityPercent >= 90 -> "Playable"
                compatibilityPercent >= 70 -> "In-Game"
                compatibilityPercent >= 50 -> "Menu"
                compatibilityPercent >= 30 -> "Boots"
                else -> "Untested"
            }
    }

    fun getGameInfo(path: String): GameInfo? {
        val raw = nativeGetGameInfo(nativeHandle, path)
        if (raw.isNullOrEmpty()) return null

        val parts = raw.split('|')
        if (parts.size < 20) return null

        val importLibs = mutableMapOf<String, Int>()
        if (parts[17].isNotEmpty()) {
            parts[17].split(',').forEach { entry ->
                val kv = entry.split(':')
                if (kv.size == 2) {
                    importLibs[kv[0]] = kv[1].toIntOrNull() ?: 0
                }
            }
        }

        return GameInfo(
            titleId = parts[0],
            mediaId = parts[1],
            version = parts[2],
            baseVersion = parts[3],
            discNumber = parts[4].toIntOrNull() ?: 0,
            discCount = parts[5].toIntOrNull() ?: 0,
            platform = parts[6].toIntOrNull() ?: 0,
            executableType = parts[7].toIntOrNull() ?: 0,
            savegameId = parts[8],
            gameRegion = parts[9],
            baseAddress = parts[10],
            entryPoint = parts[11],
            imageSize = parts[12].toLongOrNull() ?: 0,
            stackSize = parts[13].toLongOrNull() ?: 0,
            heapSize = parts[14].toLongOrNull() ?: 0,
            moduleName = parts[15],
            sectionCount = parts[16].toIntOrNull() ?: 0,
            importLibraries = importLibs,
            totalImports = parts[18].toIntOrNull() ?: 0,
            resolvedImports = parts[19].toIntOrNull() ?: 0
        )
    }

    // Save states
    fun saveState(path: String): Boolean {
        return nativeSaveState(nativeHandle, path)
    }
    
    fun loadState(path: String): Boolean {
        return nativeLoadState(nativeHandle, path)
    }
    
    // Performance stats
    val fps: Double
        get() = nativeGetFps(nativeHandle)
    
    val frameTime: Double
        get() = nativeGetFrameTime(nativeHandle)
    
    // Settings
    fun setResolutionScale(scale: Int) {
        nativeSetResolutionScale(nativeHandle, scale)
    }

    fun setVsync(enabled: Boolean) {
        nativeSetVsync(nativeHandle, enabled)
    }

    fun setFrameSkip(count: Int) {
        nativeSetFrameSkip(nativeHandle, count)
    }

    fun setTargetFps(fps: Int) {
        nativeSetTargetFps(nativeHandle, fps)
    }
    
    override fun close() {
        if (nativeHandle != 0L) {
            nativeDestroy(nativeHandle)
            nativeHandle = 0
        }
    }
    
    protected fun finalize() {
        close()
    }
    
    companion object {
        private var libraryLoaded = false
        private var libraryError: String? = null
        
        init {
            Log.i(TAG, "Loading native library x360mu_jni...")
            try {
                System.loadLibrary("x360mu_jni")
                libraryLoaded = true
                Log.i(TAG, "Native library loaded successfully!")
            } catch (e: UnsatisfiedLinkError) {
                libraryError = e.message
                Log.e(TAG, "Failed to load native library: ${e.message}", e)
            } catch (e: Exception) {
                libraryError = e.message
                Log.e(TAG, "Exception loading native library: ${e.message}", e)
            }
        }
        
        fun isLibraryLoaded(): Boolean = libraryLoaded
        fun getLibraryError(): String? = libraryError
    }
    
    // Native methods
    private external fun nativeCreate(): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeInitialize(
        handle: Long,
        dataPath: String,
        cachePath: String,
        savePath: String,
        enableJit: Boolean,
        enableVulkan: Boolean
    ): Boolean
    
    private external fun nativeLoadGame(handle: Long, path: String): Boolean
    private external fun nativeUnloadGame(handle: Long)
    
    private external fun nativeRun(handle: Long): Boolean
    private external fun nativePause(handle: Long)
    private external fun nativeStop(handle: Long)
    private external fun nativeReset(handle: Long)
    private external fun nativeGetState(handle: Long): Int
    
    private external fun nativeSetSurface(handle: Long, surface: Surface?)
    private external fun nativeResizeSurface(handle: Long, width: Int, height: Int)
    private external fun nativeTestRender(handle: Long)
    
    private external fun nativeSetButton(handle: Long, player: Int, button: Int, pressed: Boolean)
    private external fun nativeSetTrigger(handle: Long, player: Int, trigger: Int, value: Float)
    private external fun nativeSetStick(handle: Long, player: Int, stick: Int, x: Float, y: Float)

    // Touch input
    private external fun nativeOnTouchDown(handle: Long, player: Int, pointerId: Int, x: Float, y: Float, screenWidth: Float, screenHeight: Float)
    private external fun nativeOnTouchMove(handle: Long, player: Int, pointerId: Int, x: Float, y: Float, screenWidth: Float, screenHeight: Float)
    private external fun nativeOnTouchUp(handle: Long, player: Int, pointerId: Int)

    // Controller connection
    private external fun nativeSetControllerConnected(handle: Long, player: Int, connected: Boolean)

    // Vibration
    private external fun nativeGetVibrationLeft(handle: Long, player: Int): Int
    private external fun nativeGetVibrationRight(handle: Long, player: Int): Int
    private external fun nativeSetVibrationListener(handle: Long, listener: VibrationManager.VibrationListener?)

    // Dead zone configuration
    private external fun nativeSetStickDeadZone(handle: Long, stickId: Int, inner: Float, outer: Float)
    private external fun nativeSetTriggerDeadZone(handle: Long, threshold: Float)

    private external fun nativeGetGameInfo(handle: Long, path: String): String
    private external fun nativeSaveState(handle: Long, path: String): Boolean
    private external fun nativeLoadState(handle: Long, path: String): Boolean
    
    private external fun nativeGetFps(handle: Long): Double
    private external fun nativeGetFrameTime(handle: Long): Double
    
    private external fun nativeSetResolutionScale(handle: Long, scale: Int)
    private external fun nativeSetVsync(handle: Long, enabled: Boolean)
    private external fun nativeSetFrameSkip(handle: Long, count: Int)
    private external fun nativeSetTargetFps(handle: Long, fps: Int)
    
    // Feature flags for debugging
    private external fun nativeSetFeatureFlag(flagName: String, enabled: Boolean)
    private external fun nativeGetFeatureFlag(flagName: String): Boolean
    
    /**
     * Set a feature flag value
     * 
     * Available flags:
     * - jit_trace_memory: Trace all memory accesses
     * - jit_trace_mirror_access: Trace mirror range accesses
     * - jit_trace_boundary_access: Trace 512MB boundary accesses
     * - jit_trace_blocks: Trace block execution
     * - jit_trace_mmio: Trace MMIO operations
     * - gpu_trace_registers: Trace GPU register writes
     * - gpu_trace_shaders: Trace shader compilation
     * - gpu_trace_draws: Trace draw calls
     * - kernel_trace_syscalls: Trace syscalls
     * - kernel_trace_threads: Trace threading
     * - kernel_trace_files: Trace file I/O
     * - disable_fastmem: Use slow path for memory
     * - force_interpreter: Disable JIT
     */
    fun setFeatureFlag(name: String, enabled: Boolean) {
        Log.i(TAG, "Setting feature flag: $name = $enabled")
        nativeSetFeatureFlag(name, enabled)
    }
    
    fun getFeatureFlag(name: String): Boolean {
        return nativeGetFeatureFlag(name)
    }

    // ========================================================================
    // Crash handler and log buffer
    // ========================================================================

    fun installCrashHandler(crashDir: String) {
        Log.i(TAG, "Installing crash handler, dir: $crashDir")
        nativeInstallCrashHandler(nativeHandle, crashDir)
    }

    enum class LogSeverity(val value: Int) {
        Debug(0), Info(1), Warning(2), Error(3)
    }

    enum class LogComponent(val value: Int) {
        Core(0), CPU(1), GPU(2), APU(3), Kernel(4),
        Memory(5), Input(6), JIT(7), Loader(8);

        val displayName: String get() = name
    }

    data class LogEntry(
        val timestampMs: Long,
        val severity: LogSeverity,
        val component: LogComponent,
        val message: String
    )

    fun getLogCount(): Int = nativeGetLogCount()

    fun getLogs(
        severityMin: LogSeverity = LogSeverity.Debug,
        component: Int = -1
    ): List<LogEntry> {
        val raw = nativeGetLogs(severityMin.value, component)
        if (raw.isNullOrEmpty()) return emptyList()

        return raw.trimEnd('\n').split('\n').mapNotNull { line ->
            val parts = line.split('|', limit = 4)
            if (parts.size < 4) return@mapNotNull null
            LogEntry(
                timestampMs = parts[0].toLongOrNull() ?: 0,
                severity = LogSeverity.entries.find { it.value == (parts[1].toIntOrNull() ?: 0) } ?: LogSeverity.Debug,
                component = LogComponent.entries.find { it.value == (parts[2].toIntOrNull() ?: 0) } ?: LogComponent.Core,
                message = parts[3]
            )
        }
    }

    fun exportLogs(): String = nativeExportLogs() ?: ""

    fun clearLogs() = nativeClearLogs()

    fun listCrashLogs(crashDir: String): List<String> {
        return nativeListCrashLogs(crashDir)?.toList() ?: emptyList()
    }

    fun readCrashLog(path: String): String {
        return nativeReadCrashLog(path) ?: ""
    }

    // Crash handler and log buffer native methods (instance - need handle)
    private external fun nativeInstallCrashHandler(handle: Long, crashDir: String)

    // Static log/crash methods (operate on global singleton)
    private external fun nativeGetLogCount(): Int
    private external fun nativeGetLogs(severityMin: Int, component: Int): String
    private external fun nativeExportLogs(): String
    private external fun nativeClearLogs()
    private external fun nativeListCrashLogs(crashDir: String): Array<String>?
    private external fun nativeReadCrashLog(path: String): String
}

