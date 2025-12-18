package com.x360mu.util

import android.content.Context
import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import android.util.Log

private const val TAG = "360mu-PathUtils"

/**
 * Utility functions for converting content URIs to real file paths
 */
object PathUtils {
    
    /**
     * Convert a content URI to a real file system path
     * Returns null if the conversion is not possible
     */
    fun getPathFromUri(context: Context, uri: Uri): String? {
        Log.d(TAG, "getPathFromUri: $uri")
        
        // Already a file URI or path
        if (uri.scheme == "file") {
            return uri.path
        }
        
        // Content URI - try to extract real path
        if (uri.scheme == "content") {
            return getPathFromContentUri(context, uri)
        }
        
        // Assume it's already a path
        return uri.toString()
    }
    
    /**
     * Extract real path from a content:// URI
     */
    private fun getPathFromContentUri(context: Context, uri: Uri): String? {
        try {
            // Handle Documents provider
            if (DocumentsContract.isDocumentUri(context, uri)) {
                val docId = DocumentsContract.getDocumentId(uri)
                Log.d(TAG, "Document ID: $docId")
                
                when {
                    // External Storage Provider
                    isExternalStorageDocument(uri) -> {
                        val split = docId.split(":")
                        val type = split[0]
                        val relativePath = if (split.size > 1) split[1] else ""
                        
                        return when {
                            "primary".equals(type, ignoreCase = true) -> {
                                val path = "${Environment.getExternalStorageDirectory()}/$relativePath"
                                Log.d(TAG, "External storage path: $path")
                                path
                            }
                            else -> {
                                // Try secondary storage
                                val storagePath = "/storage/$type/$relativePath"
                                Log.d(TAG, "Secondary storage path: $storagePath")
                                storagePath
                            }
                        }
                    }
                    
                    // Downloads provider
                    isDownloadsDocument(uri) -> {
                        // Try to get the path directly
                        val path = getDataColumn(context, uri, null, null)
                        if (path != null) {
                            Log.d(TAG, "Downloads path: $path")
                            return path
                        }
                        
                        // Fallback for raw document IDs
                        if (docId.startsWith("raw:")) {
                            return docId.substring(4)
                        }
                        
                        // Try common download paths
                        val fileName = docId.substringAfterLast("/")
                        val downloadPath = "${Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)}/$fileName"
                        Log.d(TAG, "Downloads fallback path: $downloadPath")
                        return downloadPath
                    }
                    
                    // Media provider
                    isMediaDocument(uri) -> {
                        val split = docId.split(":")
                        val type = split[0]
                        val id = if (split.size > 1) split[1] else ""
                        
                        val contentUri = when (type) {
                            "image" -> android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI
                            "video" -> android.provider.MediaStore.Video.Media.EXTERNAL_CONTENT_URI
                            "audio" -> android.provider.MediaStore.Audio.Media.EXTERNAL_CONTENT_URI
                            else -> return null
                        }
                        
                        return getDataColumn(context, contentUri, "_id=?", arrayOf(id))
                    }
                }
            }
            
            // Generic content provider - try to get data column
            return getDataColumn(context, uri, null, null)
            
        } catch (e: Exception) {
            Log.e(TAG, "Error getting path from URI: ${e.message}", e)
            return null
        }
    }
    
    /**
     * Query for the _data column in a content provider
     */
    private fun getDataColumn(
        context: Context,
        uri: Uri,
        selection: String?,
        selectionArgs: Array<String>?
    ): String? {
        val column = "_data"
        val projection = arrayOf(column)
        
        return try {
            context.contentResolver.query(uri, projection, selection, selectionArgs, null)?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val columnIndex = cursor.getColumnIndexOrThrow(column)
                    cursor.getString(columnIndex)
                } else {
                    null
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error querying data column: ${e.message}")
            null
        }
    }
    
    private fun isExternalStorageDocument(uri: Uri): Boolean {
        return "com.android.externalstorage.documents" == uri.authority
    }
    
    private fun isDownloadsDocument(uri: Uri): Boolean {
        return "com.android.providers.downloads.documents" == uri.authority
    }
    
    private fun isMediaDocument(uri: Uri): Boolean {
        return "com.android.providers.media.documents" == uri.authority
    }
    
    /**
     * Convert a string that may be a content URI to a file path
     */
    fun resolveGamePath(context: Context, pathOrUri: String): String? {
        Log.d(TAG, "resolveGamePath: $pathOrUri")
        
        // Check if it's already a valid file path
        if (!pathOrUri.startsWith("content://")) {
            Log.d(TAG, "Already a file path: $pathOrUri")
            return pathOrUri
        }
        
        // Parse as URI and convert
        return try {
            val uri = Uri.parse(pathOrUri)
            val path = getPathFromUri(context, uri)
            Log.d(TAG, "Resolved path: $path")
            path
        } catch (e: Exception) {
            Log.e(TAG, "Error resolving path: ${e.message}", e)
            null
        }
    }
}
