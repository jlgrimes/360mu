package com.x360mu.ui.screens

import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.x360mu.core.NativeEmulator
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private const val TAG = "360mu-TestRender"

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TestRenderScreen(
    emulator: NativeEmulator,
    onBack: () -> Unit
) {
    Log.i(TAG, "TestRenderScreen composable rendering")

    var surfaceReady by remember { mutableStateOf(false) }
    var testExecuted by remember { mutableStateOf(false) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Test Render") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                    }
                }
            )
        }
    ) { paddingValues ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(paddingValues)
        ) {
            // Render surface
            AndroidView(
                factory = { context ->
                    Log.i(TAG, "Creating SurfaceView for test render")
                    SurfaceView(context).apply {
                        holder.addCallback(object : SurfaceHolder.Callback {
                            override fun surfaceCreated(holder: SurfaceHolder) {
                                Log.i(TAG, "SurfaceHolder.Callback.surfaceCreated()")
                                try {
                                    emulator.setSurface(holder.surface)
                                    surfaceReady = true
                                    Log.i(TAG, "Surface set successfully, ready for test render")
                                } catch (e: Exception) {
                                    Log.e(TAG, "Error setting surface: ${e.message}", e)
                                }
                            }

                            override fun surfaceChanged(
                                holder: SurfaceHolder,
                                format: Int,
                                width: Int,
                                height: Int
                            ) {
                                Log.i(TAG, "SurfaceHolder.Callback.surfaceChanged() - ${width}x${height}")
                                try {
                                    emulator.resizeSurface(width, height)
                                } catch (e: Exception) {
                                    Log.e(TAG, "Error resizing surface: ${e.message}", e)
                                }
                            }

                            override fun surfaceDestroyed(holder: SurfaceHolder) {
                                Log.i(TAG, "SurfaceHolder.Callback.surfaceDestroyed()")
                                surfaceReady = false
                                try {
                                    emulator.setSurface(null)
                                } catch (e: Exception) {
                                    Log.e(TAG, "Error clearing surface: ${e.message}", e)
                                }
                            }
                        })
                    }
                },
                modifier = Modifier.fillMaxSize()
            )

            // Execute test render when surface is ready
            LaunchedEffect(surfaceReady) {
                if (surfaceReady && !testExecuted) {
                    Log.i(TAG, "Surface ready, executing test render on background thread...")
                    try {
                        // Move test render to background thread to avoid ANR
                        withContext(Dispatchers.Default) {
                            emulator.testRender()
                        }
                        testExecuted = true
                        Log.i(TAG, "Test render executed successfully")
                    } catch (e: Exception) {
                        Log.e(TAG, "Test render failed: ${e.message}", e)
                    }
                }
            }

            // Info overlay
            Column(
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(16.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Card {
                    Column(
                        modifier = Modifier.padding(16.dp),
                        horizontalAlignment = Alignment.CenterHorizontally
                    ) {
                        Text(
                            text = if (surfaceReady) "✓ Surface Ready" else "⏳ Initializing Surface...",
                            style = MaterialTheme.typography.bodyLarge
                        )
                        if (testExecuted) {
                            Spacer(modifier = Modifier.height(8.dp))
                            Text(
                                text = "✓ Test Render Executed",
                                style = MaterialTheme.typography.bodyMedium
                            )
                            Spacer(modifier = Modifier.height(8.dp))
                            Text(
                                text = "You should see a CYAN screen",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.primary
                            )
                        }
                    }
                }
            }
        }
    }
}
