package com.x360mu.ui.screens

import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.x360mu.core.NativeEmulator
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import kotlin.math.sqrt

private const val TAG = "360mu-GameScreen"

@Composable
fun GameScreen(
    emulator: NativeEmulator,
    gamePath: String,
    onBack: () -> Unit
) {
    Log.i(TAG, "GameScreen composable rendering, path: $gamePath")
    
    var isLoading by remember { mutableStateOf(true) }
    var showControls by remember { mutableStateOf(true) }
    var showMenu by remember { mutableStateOf(false) }
    var fps by remember { mutableStateOf(0.0) }
    var loadError by remember { mutableStateOf<String?>(null) }
    var emulatorState by remember { mutableStateOf("Unknown") }
    
    // Load game and start emulation
    // gamePath is now the real file system path (e.g., /storage/emulated/0/ROMS/game.iso)
    LaunchedEffect(gamePath) {
        Log.i(TAG, "GameScreen LaunchedEffect - loading game: $gamePath")
        isLoading = true
        loadError = null
        
        try {
            // Load game on IO thread to avoid blocking UI
            val loaded = withContext(Dispatchers.IO) {
                Log.i(TAG, "Calling emulator.loadGame() on IO thread...")
                emulator.loadGame(gamePath)
            }
            Log.i(TAG, "loadGame returned: $loaded")
            
            if (loaded) {
                Log.i(TAG, "Game loaded successfully, calling emulator.run()...")
                // run() starts the emulation thread internally, so it's quick
                val running = emulator.run()
                Log.i(TAG, "run() returned: $running")
                emulatorState = emulator.state.name
            } else {
                loadError = "Failed to load game"
                Log.e(TAG, "Failed to load game: $gamePath")
            }
        } catch (e: Exception) {
            loadError = "Exception: ${e.message}"
            Log.e(TAG, "Exception loading game: ${e.message}", e)
        }
        
        isLoading = false
        Log.i(TAG, "GameScreen LaunchedEffect complete, isLoading=$isLoading, error=$loadError")
    }
    
    // Update FPS display and state
    LaunchedEffect(Unit) {
        while (true) {
            fps = emulator.fps
            emulatorState = emulator.state.name
            delay(500)
        }
    }
    
    // Auto-hide controls after inactivity
    LaunchedEffect(showControls) {
        if (showControls && !showMenu) {
            delay(5000)
            showControls = false
        }
    }
    
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .pointerInput(Unit) {
                detectTapGestures(
                    onTap = { showControls = !showControls }
                )
            }
    ) {
        // Emulator surface
        EmulatorSurface(
            emulator = emulator,
            modifier = Modifier.fillMaxSize()
        )
        
        // Touch controller overlay
        TouchControllerOverlay(
            emulator = emulator,
            modifier = Modifier.fillMaxSize()
        )
        
        // Top bar with FPS and menu button
        AnimatedVisibility(
            visible = showControls,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier.align(Alignment.TopCenter)
        ) {
            TopControlBar(
                fps = fps,
                isPaused = emulator.isPaused,
                onMenuClick = { showMenu = true },
                onBackClick = onBack
            )
        }
        
        // Loading overlay or error display
        if (isLoading || loadError != null) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = 0.9f)),
                contentAlignment = Alignment.Center
            ) {
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    modifier = Modifier
                        .border(2.dp, if (loadError != null) Color.Red else Color.Green, RoundedCornerShape(8.dp))
                        .padding(24.dp)
                ) {
                    if (loadError != null) {
                        Icon(
                            Icons.Default.Error,
                            contentDescription = null,
                            tint = Color.Red,
                            modifier = Modifier.size(48.dp)
                        )
                        Spacer(modifier = Modifier.height(16.dp))
                        Text(
                            text = "Load Error",
                            color = Color.Red,
                            style = MaterialTheme.typography.titleLarge
                        )
                        Spacer(modifier = Modifier.height(8.dp))
                        Text(
                            text = loadError!!,
                            color = Color.White
                        )
                    } else {
                        CircularProgressIndicator(
                            color = MaterialTheme.colorScheme.primary
                        )
                        Spacer(modifier = Modifier.height(16.dp))
                        Text(
                            text = "Loading...",
                            color = Color.White,
                            style = MaterialTheme.typography.titleMedium
                        )
                    }
                    
                    Spacer(modifier = Modifier.height(16.dp))
                    Text(
                        text = "Path: ${gamePath.takeLast(40)}",
                        color = Color.Gray,
                        style = MaterialTheme.typography.bodySmall
                    )
                    Text(
                        text = "State: $emulatorState",
                        color = Color.Gray,
                        style = MaterialTheme.typography.bodySmall
                    )
                }
            }
        }
        
        // Debug info overlay (always visible)
        Box(
            modifier = Modifier
                .align(Alignment.BottomStart)
                .padding(8.dp)
                .background(Color.Black.copy(alpha = 0.7f), RoundedCornerShape(4.dp))
                .padding(4.dp)
        ) {
            Column {
                Text(
                    text = "DEBUG: GameScreen Active",
                    color = Color.Green,
                    style = MaterialTheme.typography.labelSmall
                )
                Text(
                    text = "State: $emulatorState",
                    color = Color.Cyan,
                    style = MaterialTheme.typography.labelSmall
                )
                Text(
                    text = "FPS: %.1f".format(fps),
                    color = if (fps > 0) Color.Green else Color.Red,
                    style = MaterialTheme.typography.labelSmall
                )
            }
        }
        
        // In-game menu
        if (showMenu) {
            InGameMenu(
                emulator = emulator,
                onDismiss = { showMenu = false },
                onExit = onBack
            )
        }
    }
}

@Composable
private fun EmulatorSurface(
    emulator: NativeEmulator,
    modifier: Modifier = Modifier
) {
    Log.i(TAG, "EmulatorSurface composable rendering")
    
    AndroidView(
        factory = { context ->
            Log.i(TAG, "Creating SurfaceView")
            SurfaceView(context).apply {
                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(holder: SurfaceHolder) {
                        Log.i(TAG, "SurfaceHolder.Callback.surfaceCreated()")
                        try {
                            emulator.setSurface(holder.surface)
                            Log.i(TAG, "Surface set successfully")
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
                        Log.i(TAG, "SurfaceHolder.Callback.surfaceChanged() - ${width}x${height}, format=$format")
                        try {
                            emulator.resizeSurface(width, height)
                        } catch (e: Exception) {
                            Log.e(TAG, "Error resizing surface: ${e.message}", e)
                        }
                    }
                    
                    override fun surfaceDestroyed(holder: SurfaceHolder) {
                        Log.i(TAG, "SurfaceHolder.Callback.surfaceDestroyed()")
                        try {
                            emulator.setSurface(null)
                        } catch (e: Exception) {
                            Log.e(TAG, "Error clearing surface: ${e.message}", e)
                        }
                    }
                })
            }
        },
        modifier = modifier
    )
}

@Composable
private fun TopControlBar(
    fps: Double,
    isPaused: Boolean,
    onMenuClick: () -> Unit,
    onBackClick: () -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(
                Color.Black.copy(alpha = 0.6f)
            )
            .padding(horizontal = 8.dp, vertical = 4.dp)
            .statusBarsPadding(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        IconButton(onClick = onBackClick) {
            Icon(
                Icons.Default.ArrowBack,
                contentDescription = "Back",
                tint = Color.White
            )
        }
        
        // FPS counter
        Surface(
            shape = RoundedCornerShape(4.dp),
            color = Color.Black.copy(alpha = 0.5f)
        ) {
            Text(
                text = "%.1f FPS".format(fps),
                color = when {
                    fps >= 55 -> Color(0xFF4CAF50) // Green
                    fps >= 25 -> Color(0xFFFFC107) // Yellow
                    else -> Color(0xFFF44336) // Red
                },
                style = MaterialTheme.typography.labelMedium,
                modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp)
            )
        }
        
        Row {
            if (isPaused) {
                Icon(
                    Icons.Default.Pause,
                    contentDescription = "Paused",
                    tint = Color.Yellow,
                    modifier = Modifier.padding(8.dp)
                )
            }
            
            IconButton(onClick = onMenuClick) {
                Icon(
                    Icons.Default.Menu,
                    contentDescription = "Menu",
                    tint = Color.White
                )
            }
        }
    }
}

@Composable
private fun TouchControllerOverlay(
    emulator: NativeEmulator,
    modifier: Modifier = Modifier
) {
    Box(modifier = modifier) {
        // Left side - D-Pad and Left Stick
        Column(
            modifier = Modifier
                .align(Alignment.BottomStart)
                .padding(16.dp)
        ) {
            // Left stick
            JoystickControl(
                onMove = { x, y ->
                    emulator.setStick(0, NativeEmulator.Stick.LEFT, x, y)
                },
                modifier = Modifier.size(120.dp)
            )
        }
        
        // Right side - ABXY buttons and Right Stick
        Column(
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .padding(16.dp),
            horizontalAlignment = Alignment.End
        ) {
            // ABXY buttons
            ABXYButtons(
                onButtonEvent = { button, pressed ->
                    emulator.setButton(0, button, pressed)
                }
            )
            
            Spacer(modifier = Modifier.height(16.dp))
            
            // Right stick
            JoystickControl(
                onMove = { x, y ->
                    emulator.setStick(0, NativeEmulator.Stick.RIGHT, x, y)
                },
                modifier = Modifier.size(100.dp)
            )
        }
        
        // Center - Start/Back/Bumpers/Triggers
        Row(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(bottom = 32.dp),
            horizontalArrangement = Arrangement.spacedBy(24.dp)
        ) {
            // Back button
            ControlButton(
                icon = Icons.Default.ArrowBack,
                label = "BACK",
                onPress = { emulator.setButton(0, NativeEmulator.Button.BACK, true) },
                onRelease = { emulator.setButton(0, NativeEmulator.Button.BACK, false) }
            )
            
            // Start button
            ControlButton(
                icon = Icons.Default.PlayArrow,
                label = "START",
                onPress = { emulator.setButton(0, NativeEmulator.Button.START, true) },
                onRelease = { emulator.setButton(0, NativeEmulator.Button.START, false) }
            )
        }
        
        // Bumpers at top corners
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 80.dp),
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            // LB + LT
            Column {
                TriggerButton(
                    label = "LT",
                    onValueChange = { emulator.setTrigger(0, NativeEmulator.Trigger.LEFT, it) }
                )
                BumperButton(
                    label = "LB",
                    onPress = { emulator.setButton(0, NativeEmulator.Button.LEFT_BUMPER, true) },
                    onRelease = { emulator.setButton(0, NativeEmulator.Button.LEFT_BUMPER, false) }
                )
            }
            
            // RB + RT
            Column(horizontalAlignment = Alignment.End) {
                TriggerButton(
                    label = "RT",
                    onValueChange = { emulator.setTrigger(0, NativeEmulator.Trigger.RIGHT, it) }
                )
                BumperButton(
                    label = "RB",
                    onPress = { emulator.setButton(0, NativeEmulator.Button.RIGHT_BUMPER, true) },
                    onRelease = { emulator.setButton(0, NativeEmulator.Button.RIGHT_BUMPER, false) }
                )
            }
        }
    }
}

@Composable
private fun JoystickControl(
    onMove: (Float, Float) -> Unit,
    modifier: Modifier = Modifier
) {
    var offsetX by remember { mutableStateOf(0f) }
    var offsetY by remember { mutableStateOf(0f) }
    
    Box(
        modifier = modifier
            .clip(CircleShape)
            .background(Color.White.copy(alpha = 0.2f))
            .pointerInput(Unit) {
                detectDragGestures(
                    onDragEnd = {
                        offsetX = 0f
                        offsetY = 0f
                        onMove(0f, 0f)
                    },
                    onDragCancel = {
                        offsetX = 0f
                        offsetY = 0f
                        onMove(0f, 0f)
                    }
                ) { change, dragAmount ->
                    change.consume()
                    
                    val maxRadius = size.width / 2f
                    offsetX = (offsetX + dragAmount.x).coerceIn(-maxRadius, maxRadius)
                    offsetY = (offsetY + dragAmount.y).coerceIn(-maxRadius, maxRadius)
                    
                    // Normalize to -1..1
                    val normalizedX = offsetX / maxRadius
                    val normalizedY = -offsetY / maxRadius // Invert Y
                    
                    onMove(normalizedX, normalizedY)
                }
            },
        contentAlignment = Alignment.Center
    ) {
        // Stick knob
        Box(
            modifier = Modifier
                .offset(
                    x = (offsetX / 2).dp,
                    y = (offsetY / 2).dp
                )
                .size(48.dp)
                .clip(CircleShape)
                .background(Color.White.copy(alpha = 0.5f))
        )
    }
}

@Composable
private fun ABXYButtons(
    onButtonEvent: (Int, Boolean) -> Unit
) {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        // Y button (top)
        FaceButton(
            label = "Y",
            color = Color(0xFFFFEB3B),
            onPress = { onButtonEvent(NativeEmulator.Button.Y, true) },
            onRelease = { onButtonEvent(NativeEmulator.Button.Y, false) }
        )
        
        Row {
            // X button (left)
            FaceButton(
                label = "X",
                color = Color(0xFF2196F3),
                onPress = { onButtonEvent(NativeEmulator.Button.X, true) },
                onRelease = { onButtonEvent(NativeEmulator.Button.X, false) }
            )
            
            Spacer(modifier = Modifier.width(32.dp))
            
            // B button (right)
            FaceButton(
                label = "B",
                color = Color(0xFFF44336),
                onPress = { onButtonEvent(NativeEmulator.Button.B, true) },
                onRelease = { onButtonEvent(NativeEmulator.Button.B, false) }
            )
        }
        
        // A button (bottom)
        FaceButton(
            label = "A",
            color = Color(0xFF4CAF50),
            onPress = { onButtonEvent(NativeEmulator.Button.A, true) },
            onRelease = { onButtonEvent(NativeEmulator.Button.A, false) }
        )
    }
}

@Composable
private fun FaceButton(
    label: String,
    color: Color,
    onPress: () -> Unit,
    onRelease: () -> Unit
) {
    var isPressed by remember { mutableStateOf(false) }
    
    Box(
        modifier = Modifier
            .size(48.dp)
            .clip(CircleShape)
            .background(
                if (isPressed) color else color.copy(alpha = 0.6f)
            )
            .pointerInput(Unit) {
                detectTapGestures(
                    onPress = {
                        isPressed = true
                        onPress()
                        tryAwaitRelease()
                        isPressed = false
                        onRelease()
                    }
                )
            },
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = label,
            color = Color.White,
            style = MaterialTheme.typography.titleMedium
        )
    }
}

@Composable
private fun ControlButton(
    icon: ImageVector,
    label: String,
    onPress: () -> Unit,
    onRelease: () -> Unit
) {
    var isPressed by remember { mutableStateOf(false) }
    
    Surface(
        modifier = Modifier
            .pointerInput(Unit) {
                detectTapGestures(
                    onPress = {
                        isPressed = true
                        onPress()
                        tryAwaitRelease()
                        isPressed = false
                        onRelease()
                    }
                )
            },
        shape = RoundedCornerShape(8.dp),
        color = if (isPressed) Color.White.copy(alpha = 0.4f) else Color.White.copy(alpha = 0.2f)
    ) {
        Column(
            modifier = Modifier.padding(8.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Icon(
                icon,
                contentDescription = label,
                tint = Color.White,
                modifier = Modifier.size(20.dp)
            )
            Text(
                text = label,
                color = Color.White,
                style = MaterialTheme.typography.labelSmall
            )
        }
    }
}

@Composable
private fun BumperButton(
    label: String,
    onPress: () -> Unit,
    onRelease: () -> Unit
) {
    var isPressed by remember { mutableStateOf(false) }
    
    Surface(
        modifier = Modifier
            .width(60.dp)
            .height(28.dp)
            .pointerInput(Unit) {
                detectTapGestures(
                    onPress = {
                        isPressed = true
                        onPress()
                        tryAwaitRelease()
                        isPressed = false
                        onRelease()
                    }
                )
            },
        shape = RoundedCornerShape(4.dp),
        color = if (isPressed) Color.White.copy(alpha = 0.4f) else Color.White.copy(alpha = 0.2f)
    ) {
        Box(contentAlignment = Alignment.Center) {
            Text(
                text = label,
                color = Color.White,
                style = MaterialTheme.typography.labelSmall
            )
        }
    }
}

@Composable
private fun TriggerButton(
    label: String,
    onValueChange: (Float) -> Unit
) {
    var value by remember { mutableStateOf(0f) }
    
    Surface(
        modifier = Modifier
            .width(60.dp)
            .height(40.dp)
            .pointerInput(Unit) {
                detectDragGestures(
                    onDragEnd = {
                        value = 0f
                        onValueChange(0f)
                    }
                ) { change, dragAmount ->
                    change.consume()
                    value = (value - dragAmount.y / 100f).coerceIn(0f, 1f)
                    onValueChange(value)
                }
            }
            .pointerInput(Unit) {
                detectTapGestures(
                    onPress = {
                        value = 1f
                        onValueChange(1f)
                        tryAwaitRelease()
                        value = 0f
                        onValueChange(0f)
                    }
                )
            },
        shape = RoundedCornerShape(4.dp),
        color = Color.White.copy(alpha = 0.2f + value * 0.4f)
    ) {
        Box(contentAlignment = Alignment.Center) {
            Text(
                text = label,
                color = Color.White,
                style = MaterialTheme.typography.labelSmall
            )
        }
    }
}

@Composable
private fun InGameMenu(
    emulator: NativeEmulator,
    onDismiss: () -> Unit,
    onExit: () -> Unit
) {
    var showExitConfirm by remember { mutableStateOf(false) }
    
    // Pause when menu opens
    LaunchedEffect(Unit) {
        emulator.pause()
    }
    
    AlertDialog(
        onDismissRequest = {
            emulator.run()
            onDismiss()
        },
        title = { Text("Game Menu") },
        text = {
            Column(
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                // Resume
                TextButton(
                    onClick = {
                        emulator.run()
                        onDismiss()
                    },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(Icons.Default.PlayArrow, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text("Resume")
                }
                
                // Reset
                TextButton(
                    onClick = {
                        emulator.reset()
                        onDismiss()
                    },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(Icons.Default.Refresh, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text("Reset")
                }
                
                Divider()
                
                // Save state
                TextButton(
                    onClick = { /* TODO */ },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(Icons.Default.Save, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text("Save State")
                }
                
                // Load state
                TextButton(
                    onClick = { /* TODO */ },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(Icons.Default.FolderOpen, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text("Load State")
                }
                
                Divider()
                
                // Exit
                TextButton(
                    onClick = { showExitConfirm = true },
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.textButtonColors(
                        contentColor = MaterialTheme.colorScheme.error
                    )
                ) {
                    Icon(Icons.Default.ExitToApp, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text("Exit Game")
                }
            }
        },
        confirmButton = {}
    )
    
    // Exit confirmation
    if (showExitConfirm) {
        AlertDialog(
            onDismissRequest = { showExitConfirm = false },
            title = { Text("Exit Game?") },
            text = { Text("Are you sure you want to exit? Unsaved progress will be lost.") },
            confirmButton = {
                TextButton(onClick = onExit) {
                    Text("Exit")
                }
            },
            dismissButton = {
                TextButton(onClick = { showExitConfirm = false }) {
                    Text("Cancel")
                }
            }
        )
    }
}

