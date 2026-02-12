package com.x360mu

import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.x360mu.core.NativeEmulator
import com.x360mu.ui.App
import com.x360mu.ui.theme.X360muTheme

private const val TAG = "360mu-MainActivity"

class MainActivity : ComponentActivity() {

    // Hold a reference so lifecycle callbacks can pause/resume
    private var emulatorRef: NativeEmulator? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i(TAG, "=== onCreate started ===")

        try { enableEdgeToEdge() }
        catch (e: Exception) { Log.e(TAG, "enableEdgeToEdge failed: ${e.message}", e) }

        // Observe lifecycle for pause/resume
        lifecycle.addObserver(LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_PAUSE -> {
                    Log.i(TAG, "Lifecycle ON_PAUSE - pausing emulator")
                    emulatorRef?.let {
                        if (it.isRunning) it.pause()
                    }
                }
                Lifecycle.Event.ON_RESUME -> {
                    Log.i(TAG, "Lifecycle ON_RESUME - resuming emulator")
                    emulatorRef?.let {
                        if (it.isPaused) it.run()
                    }
                }
                Lifecycle.Event.ON_DESTROY -> {
                    Log.i(TAG, "Lifecycle ON_DESTROY - stopping emulator")
                    emulatorRef?.let {
                        if (it.isRunning || it.isPaused) it.stop()
                    }
                }
                else -> {}
            }
        })

        setContent {
            X360muTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    App(onEmulatorCreated = { emulatorRef = it })
                }
            }
        }

        Log.i(TAG, "=== onCreate completed ===")
    }
}
