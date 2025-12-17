package com.x360mu

import android.app.Application
import android.util.Log

class X360muApplication : Application() {
    
    override fun onCreate() {
        super.onCreate()
        
        Log.i(TAG, "360μ Application starting")
        
        // Initialize crash reporting in production
        if (!BuildConfig.DEBUG) {
            Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
                Log.e(TAG, "Uncaught exception in thread ${thread.name}", throwable)
                // Could add crash reporting here
            }
        }
        
        // Create necessary directories
        filesDir.resolve("games").mkdirs()
        filesDir.resolve("saves").mkdirs()
        cacheDir.resolve("shaders").mkdirs()
        cacheDir.resolve("textures").mkdirs()
        
        Log.i(TAG, "Data directory: ${filesDir.absolutePath}")
        Log.i(TAG, "Cache directory: ${cacheDir.absolutePath}")
    }
    
    companion object {
        private const val TAG = "360mu"
    }
}

