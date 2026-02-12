package com.x360mu.core

import android.content.Context
import android.util.Log
import java.io.File

private const val TAG = "360mu-ProfileMgr"
private const val PROFILES_DIR = "controller_profiles"
private const val PROFILE_EXT = ".json"

/**
 * Manages controller profile persistence — save/load/list/delete profiles as JSON files.
 */
class ControllerProfileManager(private val context: Context) {

    private val profilesDir: File = File(context.filesDir, PROFILES_DIR).also {
        if (!it.exists()) it.mkdirs()
    }

    /**
     * List all saved profile names
     */
    fun listProfiles(): List<String> {
        return profilesDir.listFiles { f -> f.extension == "json" }
            ?.map { it.nameWithoutExtension }
            ?.sorted()
            ?: emptyList()
    }

    /**
     * Save a profile. Overwrites if name already exists.
     */
    fun saveProfile(profile: ControllerProfile): Boolean {
        return try {
            val file = File(profilesDir, sanitizeFileName(profile.name) + PROFILE_EXT)
            file.writeText(profile.toJsonString())
            Log.i(TAG, "Saved profile: ${profile.name} -> ${file.absolutePath}")
            true
        } catch (e: Exception) {
            Log.e(TAG, "Failed to save profile ${profile.name}: ${e.message}", e)
            false
        }
    }

    /**
     * Load a profile by name
     */
    fun loadProfile(name: String): ControllerProfile? {
        return try {
            val file = File(profilesDir, sanitizeFileName(name) + PROFILE_EXT)
            if (!file.exists()) {
                Log.w(TAG, "Profile not found: $name")
                return null
            }
            val json = file.readText()
            ControllerProfile.fromJsonString(json).also {
                Log.i(TAG, "Loaded profile: $name")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to load profile $name: ${e.message}", e)
            null
        }
    }

    /**
     * Delete a profile by name
     */
    fun deleteProfile(name: String): Boolean {
        val file = File(profilesDir, sanitizeFileName(name) + PROFILE_EXT)
        return if (file.exists()) {
            file.delete().also { Log.i(TAG, "Deleted profile: $name") }
        } else false
    }

    /**
     * Rename a profile
     */
    fun renameProfile(oldName: String, newName: String): Boolean {
        val profile = loadProfile(oldName) ?: return false
        val renamed = profile.copy(name = newName)
        if (!saveProfile(renamed)) return false
        if (oldName != newName) deleteProfile(oldName)
        return true
    }

    /**
     * Export a profile to a string (for sharing)
     */
    fun exportProfile(name: String): String? {
        return loadProfile(name)?.toJsonString()
    }

    /**
     * Import a profile from a JSON string
     */
    fun importProfile(json: String): ControllerProfile? {
        return try {
            val profile = ControllerProfile.fromJsonString(json)
            saveProfile(profile)
            profile
        } catch (e: Exception) {
            Log.e(TAG, "Failed to import profile: ${e.message}", e)
            null
        }
    }

    /**
     * Get the default profile (creates one if none exists)
     */
    fun getDefaultProfile(): ControllerProfile {
        return loadProfile("Default") ?: ControllerProfile(name = "Default").also {
            saveProfile(it)
        }
    }

    /**
     * Load a per-game profile override, falling back to the active global profile
     */
    fun loadGameProfile(titleId: String, fallbackName: String): ControllerProfile {
        val gameProfile = loadProfile("game_$titleId")
        if (gameProfile != null) return gameProfile
        return loadProfile(fallbackName) ?: getDefaultProfile()
    }

    /**
     * Save a per-game profile override
     */
    fun saveGameProfile(titleId: String, profile: ControllerProfile): Boolean {
        return saveProfile(profile.copy(name = "game_$titleId"))
    }

    private fun sanitizeFileName(name: String): String {
        return name.replace(Regex("[^a-zA-Z0-9_\\-]"), "_").take(64)
    }
}
