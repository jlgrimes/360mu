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
    
    private external fun nativeSetButton(handle: Long, player: Int, button: Int, pressed: Boolean)
    private external fun nativeSetTrigger(handle: Long, player: Int, trigger: Int, value: Float)
    private external fun nativeSetStick(handle: Long, player: Int, stick: Int, x: Float, y: Float)
    
    private external fun nativeSaveState(handle: Long, path: String): Boolean
    private external fun nativeLoadState(handle: Long, path: String): Boolean
    
    private external fun nativeGetFps(handle: Long): Double
    private external fun nativeGetFrameTime(handle: Long): Double
    
    private external fun nativeSetResolutionScale(handle: Long, scale: Int)
    private external fun nativeSetVsync(handle: Long, enabled: Boolean)
}

