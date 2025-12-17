package com.x360mu.core

import android.view.Surface

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
        nativeHandle = nativeCreate()
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
        return nativeInitialize(nativeHandle, dataPath, cachePath, savePath, enableJit, enableVulkan)
    }
    
    /**
     * Load a game file
     */
    fun loadGame(path: String): Boolean {
        return nativeLoadGame(nativeHandle, path)
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
        return nativeRun(nativeHandle)
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
        nativeSetSurface(nativeHandle, surface)
    }
    
    /**
     * Notify of surface size change
     */
    fun resizeSurface(width: Int, height: Int) {
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
        init {
            System.loadLibrary("x360mu_jni")
        }
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

