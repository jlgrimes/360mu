package com.x360mu.util

import android.content.Context
import android.content.SharedPreferences
import android.net.Uri
import android.util.Log
import androidx.core.content.edit

private const val TAG = "360mu-Prefs"
private const val PREFS_NAME = "x360mu_preferences"

/**
 * Manages app preferences including the ROMs folder path
 */
class PreferencesManager(context: Context) {

    private val prefs: SharedPreferences = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    companion object {
        private const val KEY_ROMS_FOLDER_URI = "roms_folder_uri"
        private const val KEY_ENABLE_JIT = "enable_jit"
        private const val KEY_ENABLE_VULKAN = "enable_vulkan"
        private const val KEY_RESOLUTION_SCALE = "resolution_scale"
        private const val KEY_VSYNC = "vsync_enabled"
        private const val KEY_ENABLE_AUDIO = "enable_audio"
        private const val KEY_AUDIO_BUFFER_SIZE = "audio_buffer_size"
        private const val KEY_FRAME_SKIP = "frame_skip"
        private const val KEY_SHOW_FPS = "show_fps"
        private const val KEY_SHOW_PERF_OVERLAY = "show_perf_overlay"
        private const val KEY_CONTROL_OPACITY = "control_opacity"
        private const val KEY_CONTROL_LAYOUT_LOCKED = "control_layout_locked"
        private const val KEY_INTERPRETER_FALLBACK = "interpreter_fallback"
        private const val KEY_CACHE_DIR = "cache_directory"
        private const val KEY_ACTIVE_PROFILE = "active_controller_profile"
        private const val KEY_HAPTIC_ENABLED = "haptic_enabled"
        private const val KEY_BUTTON_REMAPS = "button_remaps"

        @Volatile
        private var instance: PreferencesManager? = null

        fun getInstance(context: Context): PreferencesManager {
            return instance ?: synchronized(this) {
                instance ?: PreferencesManager(context.applicationContext).also { instance = it }
            }
        }
    }
    
    /**
     * Get the saved ROMs folder URI
     */
    var romsFolderUri: Uri?
        get() {
            val uriString = prefs.getString(KEY_ROMS_FOLDER_URI, null)
            return if (uriString != null) {
                try {
                    Uri.parse(uriString)
                } catch (e: Exception) {
                    Log.e(TAG, "Error parsing ROMs folder URI: ${e.message}")
                    null
                }
            } else {
                null
            }
        }
        set(value) {
            Log.i(TAG, "Setting ROMs folder URI: $value")
            prefs.edit {
                if (value != null) {
                    putString(KEY_ROMS_FOLDER_URI, value.toString())
                } else {
                    remove(KEY_ROMS_FOLDER_URI)
                }
            }
        }
    
    /**
     * Check if ROMs folder is configured
     */
    val isRomsFolderConfigured: Boolean
        get() = romsFolderUri != null
    
    /**
     * JIT compilation enabled
     */
    var enableJit: Boolean
        get() = prefs.getBoolean(KEY_ENABLE_JIT, true)
        set(value) = prefs.edit { putBoolean(KEY_ENABLE_JIT, value) }
    
    /**
     * Vulkan rendering enabled
     */
    var enableVulkan: Boolean
        get() = prefs.getBoolean(KEY_ENABLE_VULKAN, true)
        set(value) = prefs.edit { putBoolean(KEY_ENABLE_VULKAN, value) }
    
    /**
     * Resolution scale (1 = native, 2 = 2x, etc.)
     */
    var resolutionScale: Int
        get() = prefs.getInt(KEY_RESOLUTION_SCALE, 1)
        set(value) = prefs.edit { putInt(KEY_RESOLUTION_SCALE, value) }
    
    /**
     * VSync enabled
     */
    var vsyncEnabled: Boolean
        get() = prefs.getBoolean(KEY_VSYNC, true)
        set(value) = prefs.edit { putBoolean(KEY_VSYNC, value) }

    /**
     * Audio enabled
     */
    var enableAudio: Boolean
        get() = prefs.getBoolean(KEY_ENABLE_AUDIO, true)
        set(value) = prefs.edit { putBoolean(KEY_ENABLE_AUDIO, value) }

    /**
     * Audio buffer size: 0=low(256), 1=medium(512), 2=high(1024)
     */
    var audioBufferSize: Int
        get() = prefs.getInt(KEY_AUDIO_BUFFER_SIZE, 1)
        set(value) = prefs.edit { putInt(KEY_AUDIO_BUFFER_SIZE, value) }

    /**
     * Frame skip (0=disabled, 1-3)
     */
    var frameSkip: Int
        get() = prefs.getInt(KEY_FRAME_SKIP, 0)
        set(value) = prefs.edit { putInt(KEY_FRAME_SKIP, value) }

    /**
     * Show FPS counter
     */
    var showFps: Boolean
        get() = prefs.getBoolean(KEY_SHOW_FPS, true)
        set(value) = prefs.edit { putBoolean(KEY_SHOW_FPS, value) }

    /**
     * Show performance overlay (frame time, state)
     */
    var showPerfOverlay: Boolean
        get() = prefs.getBoolean(KEY_SHOW_PERF_OVERLAY, false)
        set(value) = prefs.edit { putBoolean(KEY_SHOW_PERF_OVERLAY, value) }

    /**
     * Touch control opacity (0.0 - 1.0)
     */
    var controlOpacity: Float
        get() = prefs.getFloat(KEY_CONTROL_OPACITY, 0.5f)
        set(value) = prefs.edit { putFloat(KEY_CONTROL_OPACITY, value) }

    /**
     * Touch control layout locked
     */
    var controlLayoutLocked: Boolean
        get() = prefs.getBoolean(KEY_CONTROL_LAYOUT_LOCKED, true)
        set(value) = prefs.edit { putBoolean(KEY_CONTROL_LAYOUT_LOCKED, value) }

    /**
     * Interpreter fallback when JIT fails
     */
    var interpreterFallback: Boolean
        get() = prefs.getBoolean(KEY_INTERPRETER_FALLBACK, true)
        set(value) = prefs.edit { putBoolean(KEY_INTERPRETER_FALLBACK, value) }

    /**
     * Custom cache directory path
     */
    var cacheDirectory: String?
        get() = prefs.getString(KEY_CACHE_DIR, null)
        set(value) = prefs.edit {
            if (value != null) putString(KEY_CACHE_DIR, value)
            else remove(KEY_CACHE_DIR)
        }

    /**
     * Active controller profile name
     */
    var activeProfile: String
        get() = prefs.getString(KEY_ACTIVE_PROFILE, "Default") ?: "Default"
        set(value) = prefs.edit { putString(KEY_ACTIVE_PROFILE, value) }

    /**
     * Haptic feedback enabled for touch controls
     */
    var hapticEnabled: Boolean
        get() = prefs.getBoolean(KEY_HAPTIC_ENABLED, true)
        set(value) = prefs.edit { putBoolean(KEY_HAPTIC_ENABLED, value) }

    /**
     * Button remaps stored as JSON string
     */
    var buttonRemapsJson: String?
        get() = prefs.getString(KEY_BUTTON_REMAPS, null)
        set(value) = prefs.edit {
            if (value != null) putString(KEY_BUTTON_REMAPS, value)
            else remove(KEY_BUTTON_REMAPS)
        }

    /**
     * Clear all preferences
     */
    fun clearAll() {
        prefs.edit { clear() }
    }
}
