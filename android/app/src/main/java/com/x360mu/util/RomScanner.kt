package com.x360mu.util

import android.content.Context
import android.net.Uri
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import java.io.File

private const val TAG = "360mu-RomScanner"

/**
 * Data class representing a discovered ROM file
 */
data class RomFile(
    val uri: Uri,
    val name: String,
    val displayName: String,
    val extension: String,
    val size: Long,
    val realPath: String? // Resolved file system path if available
)

/**
 * Scans folders for Xbox 360 ROM files
 */
object RomScanner {
    
    // Supported ROM extensions
    private val SUPPORTED_EXTENSIONS = setOf("iso", "xex")
    
    /**
     * Scan a folder URI for ROM files
     */
    fun scanFolder(context: Context, folderUri: Uri): List<RomFile> {
        Log.i(TAG, "Scanning folder: $folderUri")
        
        val roms = mutableListOf<RomFile>()
        
        try {
            val documentFile = DocumentFile.fromTreeUri(context, folderUri)
            if (documentFile == null || !documentFile.exists()) {
                Log.e(TAG, "Folder does not exist or cannot be accessed")
                return emptyList()
            }
            
            scanDirectory(context, documentFile, roms)
            
        } catch (e: Exception) {
            Log.e(TAG, "Error scanning folder: ${e.message}", e)
        }
        
        Log.i(TAG, "Found ${roms.size} ROM files")
        return roms.sortedBy { it.displayName.lowercase() }
    }
    
    /**
     * Recursively scan a directory for ROM files
     */
    private fun scanDirectory(context: Context, directory: DocumentFile, roms: MutableList<RomFile>) {
        val files = directory.listFiles()
        
        for (file in files) {
            if (file.isDirectory) {
                // Recursively scan subdirectories
                scanDirectory(context, file, roms)
            } else if (file.isFile) {
                val name = file.name ?: continue
                val extension = name.substringAfterLast('.', "").lowercase()
                
                if (extension in SUPPORTED_EXTENSIONS) {
                    val uri = file.uri
                    val displayName = name.substringBeforeLast('.')
                    val size = file.length()
                    
                    // Try to get the real file path
                    val realPath = getRealPathFromUri(context, uri)
                    
                    Log.d(TAG, "Found ROM: $name (path: $realPath)")
                    
                    roms.add(RomFile(
                        uri = uri,
                        name = name,
                        displayName = displayName,
                        extension = extension.uppercase(),
                        size = size,
                        realPath = realPath
                    ))
                }
            }
        }
    }
    
    /**
     * Get the real file system path from a DocumentFile URI
     */
    private fun getRealPathFromUri(context: Context, uri: Uri): String? {
        // For tree URIs from the document picker, we can extract the path
        val uriString = uri.toString()
        
        // Handle document URIs from external storage
        if (uriString.contains("com.android.externalstorage.documents")) {
            try {
                // Extract the path from the URI
                // Format: content://com.android.externalstorage.documents/tree/primary%3AROMS/document/primary%3AROMS%2Fgame.iso
                val docId = uri.lastPathSegment ?: return null
                
                // docId format: "primary:path/to/file" or "XXXX-XXXX:path/to/file"
                val split = docId.split(":")
                if (split.size >= 2) {
                    val storageType = split[0]
                    val relativePath = split.subList(1, split.size).joinToString(":")
                    
                    val basePath = when {
                        storageType.equals("primary", ignoreCase = true) -> {
                            "/storage/emulated/0"
                        }
                        else -> {
                            // External SD card or USB storage
                            "/storage/$storageType"
                        }
                    }
                    
                    val fullPath = "$basePath/$relativePath"
                    
                    // Verify the file exists
                    if (File(fullPath).exists()) {
                        return fullPath
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error extracting path from URI: ${e.message}")
            }
        }
        
        return null
    }
    
    /**
     * Get the real file path for a ROM, required by the native emulator
     */
    fun getRomRealPath(context: Context, rom: RomFile): String? {
        // Use cached path if available
        if (rom.realPath != null && File(rom.realPath).exists()) {
            return rom.realPath
        }
        
        // Try to resolve again
        return getRealPathFromUri(context, rom.uri)
    }
    
    /**
     * Format file size for display
     */
    fun formatFileSize(bytes: Long): String {
        return when {
            bytes >= 1_000_000_000 -> String.format("%.1f GB", bytes / 1_000_000_000.0)
            bytes >= 1_000_000 -> String.format("%.1f MB", bytes / 1_000_000.0)
            bytes >= 1_000 -> String.format("%.1f KB", bytes / 1_000.0)
            else -> "$bytes B"
        }
    }
}
