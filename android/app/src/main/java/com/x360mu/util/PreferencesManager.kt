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
     * Clear all preferences
     */
    fun clearAll() {
        prefs.edit { clear() }
    }
}
