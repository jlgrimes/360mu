package com.x360mu.ui.screens

import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
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
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.PointerEventType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import kotlin.math.sqrt
import androidx.compose.ui.viewinterop.AndroidView
import com.x360mu.core.ButtonLayout
import com.x360mu.core.ControllerProfile
import com.x360mu.core.ControllerProfileManager
import com.x360mu.core.NativeEmulator
import com.x360mu.core.VibrationManager
import com.x360mu.util.PreferencesManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.math.roundToInt

private const val TAG = "360mu-GameScreen"

@Composable
fun GameScreen(
    emulator: NativeEmulator,
    gamePath: String,
    onBack: () -> Unit
) {
    Log.i(TAG, "GameScreen composable rendering, path: $gamePath")

    val context = LocalContext.current
    val prefs = remember { PreferencesManager.getInstance(context) }

    var isLoading by remember { mutableStateOf(true) }
    var showControls by remember { mutableStateOf(true) }
    var showMenu by remember { mutableStateOf(false) }
    var fps by remember { mutableStateOf(0.0) }
    var frameTime by remember { mutableStateOf(0.0) }
    var loadError by remember { mutableStateOf<String?>(null) }
    var emulatorState by remember { mutableStateOf("Unknown") }
    var controlOpacity by remember { mutableStateOf(prefs.controlOpacity) }
    var showFps by remember { mutableStateOf(prefs.showFps) }
    var showPerfOverlay by remember { mutableStateOf(prefs.showPerfOverlay) }
    var layoutLocked by remember { mutableStateOf(prefs.controlLayoutLocked) }
    var showOpacitySlider by remember { mutableStateOf(false) }
    var editLayoutMode by remember { mutableStateOf(false) }

    // Error overlay toasts
    var errorToasts by remember { mutableStateOf<List<String>>(emptyList()) }

    // Controller profile
    val profileManager = remember { ControllerProfileManager(context) }
    var activeProfile by remember {
        mutableStateOf(profileManager.loadProfile(prefs.activeProfile) ?: profileManager.getDefaultProfile())
    }
    var buttonPositions by remember { mutableStateOf(activeProfile.buttons.toMutableMap()) }

    // Vibration manager — register with native for XInputSetState callbacks
    val vibrationManager = remember { VibrationManager(context) }
    DisposableEffect(emulator) {
        emulator.setVibrationListener(vibrationManager.listener)
        onDispose {
            emulator.setVibrationListener(null)
            vibrationManager.release()
        }
    }

    // Load game
    LaunchedEffect(gamePath) {
        Log.i(TAG, "Loading game: $gamePath")
        isLoading = true
        loadError = null

        try {
            val loaded = withContext(Dispatchers.IO) {
                emulator.loadGame(gamePath)
            }
            if (loaded) {
                emulator.run()
                emulatorState = emulator.state.name
            } else {
                loadError = "Failed to load game"
            }
        } catch (e: Exception) {
            loadError = "Exception: ${e.message}"
            Log.e(TAG, "Exception loading game: ${e.message}", e)
        }

        isLoading = false
    }

    // Update FPS/stats + poll for recent errors
    LaunchedEffect(Unit) {
        var lastLogCount = emulator.getLogCount()
        while (true) {
            fps = emulator.fps
            frameTime = emulator.frameTime
            emulatorState = emulator.state.name

            // Check for new error log entries to show as toasts
            val newCount = emulator.getLogCount()
            if (newCount > lastLogCount) {
                val errors = withContext(Dispatchers.IO) {
                    emulator.getLogs(NativeEmulator.LogSeverity.Error)
                }
                val recent = errors.takeLast(3).map { it.message }
                if (recent.isNotEmpty()) {
                    errorToasts = recent
                }
            }
            lastLogCount = newCount

            delay(500)
        }
    }

    // Auto-dismiss error toasts
    LaunchedEffect(errorToasts) {
        if (errorToasts.isNotEmpty()) {
            delay(4000)
            errorToasts = emptyList()
        }
    }

    // Auto-hide controls
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
        EmulatorSurface(emulator = emulator, modifier = Modifier.fillMaxSize())

        // Touch controller overlay
        TouchControllerOverlay(
            emulator = emulator,
            opacity = controlOpacity,
            layoutLocked = layoutLocked,
            editMode = editLayoutMode,
            buttonPositions = buttonPositions,
            onButtonMoved = { key, layout ->
                buttonPositions = buttonPositions.toMutableMap().apply { put(key, layout) }
            },
            modifier = Modifier.fillMaxSize()
        )

        // Layout editor bar
        if (editLayoutMode) {
            LayoutEditorBar(
                onSave = {
                    val updated = activeProfile.copy(buttons = buttonPositions)
                    profileManager.saveProfile(updated)
                    activeProfile = updated
                    prefs.activeProfile = updated.name
                    editLayoutMode = false
                },
                onReset = {
                    buttonPositions = ControllerProfile.DEFAULT_LAYOUT.toMutableMap()
                },
                onCancel = {
                    buttonPositions = activeProfile.buttons.toMutableMap()
                    editLayoutMode = false
                },
                modifier = Modifier.align(Alignment.TopCenter)
            )
        }

        // Top bar
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

        // Performance overlay
        if (showFps || showPerfOverlay) {
            PerformanceOverlay(
                fps = fps,
                frameTime = frameTime,
                emulatorState = emulatorState,
                showFps = showFps,
                showDetailed = showPerfOverlay,
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .padding(top = 48.dp, end = 8.dp)
            )
        }

        // Error toasts overlay
        AnimatedVisibility(
            visible = errorToasts.isNotEmpty(),
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier
                .align(Alignment.TopStart)
                .padding(top = 48.dp, start = 8.dp)
        ) {
            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                errorToasts.forEach { msg ->
                    Surface(
                        shape = RoundedCornerShape(4.dp),
                        color = Color(0xCCB71C1C)
                    ) {
                        Text(
                            text = msg,
                            color = Color.White,
                            style = MaterialTheme.typography.labelSmall,
                            maxLines = 1,
                            modifier = Modifier.padding(horizontal = 8.dp, vertical = 3.dp)
                        )
                    }
                }
            }
        }

        // Loading/error overlay
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
                        .border(
                            2.dp,
                            if (loadError != null) Color.Red else Color.Green,
                            RoundedCornerShape(8.dp)
                        )
                        .padding(24.dp)
                ) {
                    if (loadError != null) {
                        Icon(
                            Icons.Default.Error, contentDescription = null,
                            tint = Color.Red, modifier = Modifier.size(48.dp)
                        )
                        Spacer(modifier = Modifier.height(16.dp))
                        Text("Load Error", color = Color.Red,
                            style = MaterialTheme.typography.titleLarge)
                        Spacer(modifier = Modifier.height(8.dp))
                        Text(loadError!!, color = Color.White)
                    } else {
                        CircularProgressIndicator(color = MaterialTheme.colorScheme.primary)
                        Spacer(modifier = Modifier.height(16.dp))
                        Text("Loading...", color = Color.White,
                            style = MaterialTheme.typography.titleMedium)
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

        // Opacity slider (toggled from menu)
        AnimatedVisibility(
            visible = showOpacitySlider,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(bottom = 200.dp)
        ) {
            Surface(
                shape = RoundedCornerShape(12.dp),
                color = Color.Black.copy(alpha = 0.8f),
                modifier = Modifier.padding(16.dp)
            ) {
                Column(
                    modifier = Modifier.padding(16.dp),
                    horizontalAlignment = Alignment.CenterHorizontally
                ) {
                    Text(
                        "Control Opacity: ${(controlOpacity * 100).toInt()}%",
                        color = Color.White,
                        style = MaterialTheme.typography.bodyMedium
                    )
                    Slider(
                        value = controlOpacity,
                        onValueChange = {
                            controlOpacity = it
                            prefs.controlOpacity = it
                        },
                        valueRange = 0.1f..1.0f,
                        modifier = Modifier.width(250.dp)
                    )
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        TextButton(onClick = {
                            layoutLocked = !layoutLocked
                            prefs.controlLayoutLocked = layoutLocked
                        }) {
                            Icon(
                                if (layoutLocked) Icons.Default.Lock else Icons.Default.LockOpen,
                                contentDescription = null, tint = Color.White
                            )
                            Spacer(modifier = Modifier.width(4.dp))
                            Text(
                                if (layoutLocked) "Locked" else "Unlocked",
                                color = Color.White
                            )
                        }
                        TextButton(onClick = { showOpacitySlider = false }) {
                            Text("Done", color = Color.White)
                        }
                    }
                }
            }
        }

        // In-game menu
        if (showMenu) {
            InGameMenu(
                emulator = emulator,
                onDismiss = { showMenu = false },
                onExit = onBack,
                onToggleOpacitySlider = {
                    showOpacitySlider = !showOpacitySlider
                    showMenu = false
                },
                onEditLayout = {
                    editLayoutMode = true
                    showMenu = false
                },
                profileManager = profileManager,
                activeProfileName = activeProfile.name,
                onProfileSelected = { name ->
                    profileManager.loadProfile(name)?.let { profile ->
                        activeProfile = profile
                        buttonPositions = profile.buttons.toMutableMap()
                        prefs.activeProfile = name
                    }
                },
                savePath = File(context.filesDir, "saves").absolutePath,
                gamePath = gamePath
            )
        }
    }
}

@Composable
private fun EmulatorSurface(emulator: NativeEmulator, modifier: Modifier = Modifier) {
    AndroidView(
        factory = { context ->
            SurfaceView(context).apply {
                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(holder: SurfaceHolder) {
                        try { emulator.setSurface(holder.surface) }
                        catch (e: Exception) { Log.e(TAG, "Error setting surface: ${e.message}", e) }
                    }
                    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                        try { emulator.resizeSurface(width, height) }
                        catch (e: Exception) { Log.e(TAG, "Error resizing surface: ${e.message}", e) }
                    }
                    override fun surfaceDestroyed(holder: SurfaceHolder) {
                        try { emulator.setSurface(null) }
                        catch (e: Exception) { Log.e(TAG, "Error clearing surface: ${e.message}", e) }
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
            .background(Color.Black.copy(alpha = 0.6f))
            .padding(horizontal = 8.dp, vertical = 4.dp)
            .statusBarsPadding(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        IconButton(onClick = onBackClick) {
            Icon(Icons.Default.ArrowBack, contentDescription = "Back", tint = Color.White)
        }

        Surface(shape = RoundedCornerShape(4.dp), color = Color.Black.copy(alpha = 0.5f)) {
            Text(
                text = "%.1f FPS".format(fps),
                color = when {
                    fps >= 55 -> Color(0xFF4CAF50)
                    fps >= 25 -> Color(0xFFFFC107)
                    else -> Color(0xFFF44336)
                },
                style = MaterialTheme.typography.labelMedium,
                modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp)
            )
        }

        Row {
            if (isPaused) {
                Icon(
                    Icons.Default.Pause, contentDescription = "Paused",
                    tint = Color.Yellow, modifier = Modifier.padding(8.dp)
                )
            }
            IconButton(onClick = onMenuClick) {
                Icon(Icons.Default.Menu, contentDescription = "Menu", tint = Color.White)
            }
        }
    }
}

@Composable
private fun PerformanceOverlay(
    fps: Double,
    frameTime: Double,
    emulatorState: String,
    showFps: Boolean,
    showDetailed: Boolean,
    modifier: Modifier = Modifier
) {
    Surface(
        modifier = modifier,
        shape = RoundedCornerShape(4.dp),
        color = Color.Black.copy(alpha = 0.7f)
    ) {
        Column(modifier = Modifier.padding(6.dp)) {
            if (showFps) {
                Text(
                    text = "FPS: %.1f".format(fps),
                    color = when {
                        fps >= 55 -> Color(0xFF4CAF50)
                        fps >= 25 -> Color(0xFFFFC107)
                        else -> Color(0xFFF44336)
                    },
                    style = MaterialTheme.typography.labelSmall
                )
            }
            if (showDetailed) {
                Text(
                    text = "Frame: %.2f ms".format(frameTime),
                    color = Color.Cyan,
                    style = MaterialTheme.typography.labelSmall
                )
                Text(
                    text = "State: $emulatorState",
                    color = Color.White,
                    style = MaterialTheme.typography.labelSmall
                )
            }
        }
    }
}

/**
 * Draggable wrapper for layout editor mode.
 * In edit mode: shows border, allows drag-to-reposition.
 * In play mode: passes through to child.
 */
@Composable
private fun EditableButton(
    buttonKey: String,
    layout: ButtonLayout,
    editMode: Boolean,
    onMoved: (String, ButtonLayout) -> Unit,
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit
) {
    if (!layout.visible) return

    BoxWithConstraints(modifier = modifier) {
        val parentWidth = constraints.maxWidth.toFloat()
        val parentHeight = constraints.maxHeight.toFloat()
        val btnW = (layout.width * parentWidth).roundToInt()
        val btnH = (layout.height * parentHeight).roundToInt()
        val posX = ((layout.x - layout.width / 2) * parentWidth).roundToInt()
        val posY = ((layout.y - layout.height / 2) * parentHeight).roundToInt()

        var offsetX by remember(layout) { mutableStateOf(posX.toFloat()) }
        var offsetY by remember(layout) { mutableStateOf(posY.toFloat()) }

        Box(
            modifier = Modifier
                .offset { IntOffset(offsetX.roundToInt(), offsetY.roundToInt()) }
                .size(btnW.dp.coerceAtLeast(24.dp), btnH.dp.coerceAtLeast(24.dp))
                .then(
                    if (editMode) {
                        Modifier
                            .border(2.dp, Color.Cyan, RoundedCornerShape(4.dp))
                            .pointerInput(Unit) {
                                detectDragGestures(
                                    onDragEnd = {
                                        val newCenterX = (offsetX + btnW / 2) / parentWidth
                                        val newCenterY = (offsetY + btnH / 2) / parentHeight
                                        onMoved(
                                            buttonKey,
                                            layout.copy(
                                                x = newCenterX.coerceIn(0f, 1f),
                                                y = newCenterY.coerceIn(0f, 1f)
                                            )
                                        )
                                    }
                                ) { change, dragAmount ->
                                    change.consume()
                                    offsetX = (offsetX + dragAmount.x).coerceIn(0f, parentWidth - btnW)
                                    offsetY = (offsetY + dragAmount.y).coerceIn(0f, parentHeight - btnH)
                                }
                            }
                    } else Modifier
                )
        ) {
            content()
        }
    }
}

@Composable
private fun TouchControllerOverlay(
    emulator: NativeEmulator,
    opacity: Float,
    layoutLocked: Boolean,
    editMode: Boolean = false,
    buttonPositions: Map<String, ButtonLayout> = ControllerProfile.DEFAULT_LAYOUT,
    onButtonMoved: (String, ButtonLayout) -> Unit = { _, _ -> },
    modifier: Modifier = Modifier
) {
    Box(modifier = modifier.alpha(if (editMode) 1f else opacity)) {
        // Left side - D-Pad + Left Stick
        Column(
            modifier = Modifier
                .align(Alignment.BottomStart)
                .padding(16.dp)
        ) {
            // D-Pad
            DPadControl(
                onButtonEvent = { button, pressed ->
                    if (!editMode) emulator.setButton(0, button, pressed)
                },
                modifier = Modifier.padding(bottom = 12.dp)
            )

            // Left stick
            JoystickControl(
                onMove = { x, y ->
                    if (!editMode) emulator.setStick(0, NativeEmulator.Stick.LEFT, x, y)
                },
                modifier = Modifier.size(120.dp)
            )
        }

        // Right side - ABXY + Right Stick
        Column(
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .padding(16.dp),
            horizontalAlignment = Alignment.End
        ) {
            ABXYButtons(
                onButtonEvent = { button, pressed ->
                    if (!editMode) emulator.setButton(0, button, pressed)
                }
            )
            Spacer(modifier = Modifier.height(16.dp))
            JoystickControl(
                onMove = { x, y ->
                    if (!editMode) emulator.setStick(0, NativeEmulator.Stick.RIGHT, x, y)
                },
                modifier = Modifier.size(100.dp)
            )
        }

        // Center - Start/Back
        Row(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(bottom = 32.dp),
            horizontalArrangement = Arrangement.spacedBy(24.dp)
        ) {
            ControlButton(
                icon = Icons.Default.ArrowBack, label = "BACK",
                onPress = { if (!editMode) emulator.setButton(0, NativeEmulator.Button.BACK, true) },
                onRelease = { if (!editMode) emulator.setButton(0, NativeEmulator.Button.BACK, false) }
            )
            ControlButton(
                icon = Icons.Default.PlayArrow, label = "START",
                onPress = { if (!editMode) emulator.setButton(0, NativeEmulator.Button.START, true) },
                onRelease = { if (!editMode) emulator.setButton(0, NativeEmulator.Button.START, false) }
            )
        }

        // Bumpers/Triggers at top corners
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 80.dp),
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column {
                TriggerButton(
                    label = "LT",
                    onValueChange = {
                        if (!editMode) emulator.setTrigger(0, NativeEmulator.Trigger.LEFT, it)
                    }
                )
                BumperButton(
                    label = "LB",
                    onPress = { if (!editMode) emulator.setButton(0, NativeEmulator.Button.LEFT_BUMPER, true) },
                    onRelease = { if (!editMode) emulator.setButton(0, NativeEmulator.Button.LEFT_BUMPER, false) }
                )
            }
            Column(horizontalAlignment = Alignment.End) {
                TriggerButton(
                    label = "RT",
                    onValueChange = {
                        if (!editMode) emulator.setTrigger(0, NativeEmulator.Trigger.RIGHT, it)
                    }
                )
                BumperButton(
                    label = "RB",
                    onPress = { if (!editMode) emulator.setButton(0, NativeEmulator.Button.RIGHT_BUMPER, true) },
                    onRelease = { if (!editMode) emulator.setButton(0, NativeEmulator.Button.RIGHT_BUMPER, false) }
                )
            }
        }

        // Edit mode visual indicator
        if (editMode) {
            Text(
                text = "EDIT MODE - Drag buttons to reposition",
                color = Color.Cyan,
                style = MaterialTheme.typography.labelMedium,
                modifier = Modifier
                    .align(Alignment.Center)
                    .background(Color.Black.copy(alpha = 0.7f), RoundedCornerShape(8.dp))
                    .padding(horizontal = 16.dp, vertical = 8.dp)
            )
        }
    }
}

/**
 * Top bar shown during layout editor mode
 */
@Composable
private fun LayoutEditorBar(
    onSave: () -> Unit,
    onReset: () -> Unit,
    onCancel: () -> Unit,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .background(Color.Black.copy(alpha = 0.8f))
            .padding(horizontal = 16.dp, vertical = 8.dp)
            .statusBarsPadding(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        TextButton(onClick = onCancel) {
            Icon(Icons.Default.Close, contentDescription = null, tint = Color.White)
            Spacer(modifier = Modifier.width(4.dp))
            Text("Cancel", color = Color.White)
        }

        Text("Edit Layout", color = Color.Cyan, style = MaterialTheme.typography.titleMedium)

        Row {
            TextButton(onClick = onReset) {
                Icon(Icons.Default.Refresh, contentDescription = null, tint = Color.Yellow)
                Spacer(modifier = Modifier.width(4.dp))
                Text("Reset", color = Color.Yellow)
            }
            TextButton(onClick = onSave) {
                Icon(Icons.Default.Check, contentDescription = null, tint = Color.Green)
                Spacer(modifier = Modifier.width(4.dp))
                Text("Save", color = Color.Green)
            }
        }
    }
}

@Composable
private fun DPadControl(
    onButtonEvent: (Int, Boolean) -> Unit,
    modifier: Modifier = Modifier
) {
    Box(modifier = modifier, contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            // Up
            DPadButton(
                label = "\u25B2",
                onPress = { onButtonEvent(NativeEmulator.Button.DPAD_UP, true) },
                onRelease = { onButtonEvent(NativeEmulator.Button.DPAD_UP, false) }
            )
            Row {
                // Left
                DPadButton(
                    label = "\u25C0",
                    onPress = { onButtonEvent(NativeEmulator.Button.DPAD_LEFT, true) },
                    onRelease = { onButtonEvent(NativeEmulator.Button.DPAD_LEFT, false) }
                )
                Spacer(modifier = Modifier.size(32.dp))
                // Right
                DPadButton(
                    label = "\u25B6",
                    onPress = { onButtonEvent(NativeEmulator.Button.DPAD_RIGHT, true) },
                    onRelease = { onButtonEvent(NativeEmulator.Button.DPAD_RIGHT, false) }
                )
            }
            // Down
            DPadButton(
                label = "\u25BC",
                onPress = { onButtonEvent(NativeEmulator.Button.DPAD_DOWN, true) },
                onRelease = { onButtonEvent(NativeEmulator.Button.DPAD_DOWN, false) }
            )
        }
    }
}

/**
 * Multi-touch aware button modifier.
 * Uses awaitPointerEventScope to avoid blocking — allows simultaneous presses.
 * Performs haptic tick on press if hapticOnPress is provided.
 */
private fun Modifier.multiTouchButton(
    onPress: () -> Unit,
    onRelease: () -> Unit,
    onPressedChange: (Boolean) -> Unit,
    hapticOnPress: (() -> Unit)? = null
): Modifier = this.pointerInput(Unit) {
    awaitPointerEventScope {
        while (true) {
            val down = awaitPointerEvent()
            if (down.type == PointerEventType.Press && down.changes.any { it.pressed }) {
                onPressedChange(true)
                onPress()
                hapticOnPress?.invoke()
                // Wait until all pointers in this component are released
                while (true) {
                    val event = awaitPointerEvent()
                    val anyPressed = event.changes.any { it.pressed }
                    if (!anyPressed) {
                        onPressedChange(false)
                        onRelease()
                        break
                    }
                }
            }
        }
    }
}

@Composable
private fun DPadButton(
    label: String,
    onPress: () -> Unit,
    onRelease: () -> Unit
) {
    var isPressed by remember { mutableStateOf(false) }
    val haptic = LocalHapticFeedback.current

    Box(
        modifier = Modifier
            .size(36.dp)
            .clip(RoundedCornerShape(4.dp))
            .background(if (isPressed) Color.White.copy(alpha = 0.4f) else Color.White.copy(alpha = 0.2f))
            .multiTouchButton(onPress, onRelease, { isPressed = it }) {
                haptic.performHapticFeedback(HapticFeedbackType.TextHandleMove)
            },
        contentAlignment = Alignment.Center
    ) {
        Text(text = label, color = Color.White, style = MaterialTheme.typography.labelMedium)
    }
}

@Composable
private fun JoystickControl(
    onMove: (Float, Float) -> Unit,
    modifier: Modifier = Modifier
) {
    var offsetX by remember { mutableStateOf(0f) }
    var offsetY by remember { mutableStateOf(0f) }
    var isDragging by remember { mutableStateOf(false) }

    Box(
        modifier = modifier
            .clip(CircleShape)
            .background(Color.White.copy(alpha = if (isDragging) 0.3f else 0.2f))
            .pointerInput(Unit) {
                detectDragGestures(
                    onDragStart = { isDragging = true },
                    onDragEnd = {
                        offsetX = 0f; offsetY = 0f; isDragging = false; onMove(0f, 0f)
                    },
                    onDragCancel = {
                        offsetX = 0f; offsetY = 0f; isDragging = false; onMove(0f, 0f)
                    }
                ) { change, dragAmount ->
                    change.consume()
                    val maxRadius = size.width / 2f
                    val newX = offsetX + dragAmount.x
                    val newY = offsetY + dragAmount.y

                    // Clamp to circle (radial clamp instead of rectangular)
                    val mag = sqrt(newX * newX + newY * newY)
                    if (mag > maxRadius) {
                        offsetX = newX / mag * maxRadius
                        offsetY = newY / mag * maxRadius
                    } else {
                        offsetX = newX
                        offsetY = newY
                    }
                    onMove(offsetX / maxRadius, -offsetY / maxRadius)
                }
            },
        contentAlignment = Alignment.Center
    ) {
        // Thumb indicator — follows the drag position
        val thumbSize = 44.dp
        Box(
            modifier = Modifier
                .offset { IntOffset((offsetX * 0.5f).roundToInt(), (offsetY * 0.5f).roundToInt()) }
                .size(thumbSize)
                .clip(CircleShape)
                .background(
                    if (isDragging) Color.White.copy(alpha = 0.7f)
                    else Color.White.copy(alpha = 0.4f)
                )
        )
    }
}

@Composable
private fun ABXYButtons(onButtonEvent: (Int, Boolean) -> Unit) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        FaceButton("Y", Color(0xFFFFEB3B),
            { onButtonEvent(NativeEmulator.Button.Y, true) },
            { onButtonEvent(NativeEmulator.Button.Y, false) })
        Row {
            FaceButton("X", Color(0xFF2196F3),
                { onButtonEvent(NativeEmulator.Button.X, true) },
                { onButtonEvent(NativeEmulator.Button.X, false) })
            Spacer(modifier = Modifier.width(32.dp))
            FaceButton("B", Color(0xFFF44336),
                { onButtonEvent(NativeEmulator.Button.B, true) },
                { onButtonEvent(NativeEmulator.Button.B, false) })
        }
        FaceButton("A", Color(0xFF4CAF50),
            { onButtonEvent(NativeEmulator.Button.A, true) },
            { onButtonEvent(NativeEmulator.Button.A, false) })
    }
}

@Composable
private fun FaceButton(label: String, color: Color, onPress: () -> Unit, onRelease: () -> Unit) {
    var isPressed by remember { mutableStateOf(false) }
    val haptic = LocalHapticFeedback.current
    Box(
        modifier = Modifier
            .size(48.dp)
            .clip(CircleShape)
            .background(if (isPressed) color else color.copy(alpha = 0.6f))
            .multiTouchButton(onPress, onRelease, { isPressed = it }) {
                haptic.performHapticFeedback(HapticFeedbackType.TextHandleMove)
            },
        contentAlignment = Alignment.Center
    ) {
        Text(label, color = Color.White, style = MaterialTheme.typography.titleMedium)
    }
}

@Composable
private fun ControlButton(
    icon: ImageVector, label: String,
    onPress: () -> Unit, onRelease: () -> Unit
) {
    var isPressed by remember { mutableStateOf(false) }
    val haptic = LocalHapticFeedback.current
    Surface(
        modifier = Modifier
            .multiTouchButton(onPress, onRelease, { isPressed = it }) {
                haptic.performHapticFeedback(HapticFeedbackType.TextHandleMove)
            },
        shape = RoundedCornerShape(8.dp),
        color = if (isPressed) Color.White.copy(alpha = 0.4f) else Color.White.copy(alpha = 0.2f)
    ) {
        Column(
            modifier = Modifier.padding(8.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Icon(icon, contentDescription = label, tint = Color.White, modifier = Modifier.size(20.dp))
            Text(label, color = Color.White, style = MaterialTheme.typography.labelSmall)
        }
    }
}

@Composable
private fun BumperButton(label: String, onPress: () -> Unit, onRelease: () -> Unit) {
    var isPressed by remember { mutableStateOf(false) }
    val haptic = LocalHapticFeedback.current
    Surface(
        modifier = Modifier
            .width(60.dp)
            .height(28.dp)
            .multiTouchButton(onPress, onRelease, { isPressed = it }) {
                haptic.performHapticFeedback(HapticFeedbackType.TextHandleMove)
            },
        shape = RoundedCornerShape(4.dp),
        color = if (isPressed) Color.White.copy(alpha = 0.4f) else Color.White.copy(alpha = 0.2f)
    ) {
        Box(contentAlignment = Alignment.Center) {
            Text(label, color = Color.White, style = MaterialTheme.typography.labelSmall)
        }
    }
}

@Composable
private fun TriggerButton(label: String, onValueChange: (Float) -> Unit) {
    var value by remember { mutableStateOf(0f) }
    val haptic = LocalHapticFeedback.current
    Surface(
        modifier = Modifier
            .width(60.dp)
            .height(40.dp)
            .pointerInput(Unit) {
                awaitPointerEventScope {
                    while (true) {
                        val down = awaitPointerEvent()
                        if (down.type == PointerEventType.Press && down.changes.any { it.pressed }) {
                            // Immediate full press on touch
                            value = 1f; onValueChange(1f)
                            haptic.performHapticFeedback(HapticFeedbackType.LongPress)
                            // Track drag for analog control
                            while (true) {
                                val event = awaitPointerEvent()
                                if (!event.changes.any { it.pressed }) {
                                    value = 0f; onValueChange(0f)
                                    break
                                }
                                // Use vertical drag for partial trigger
                                val change = event.changes.firstOrNull() ?: continue
                                val dy = change.position.y - change.previousPosition.y
                                value = (value - dy / 100f).coerceIn(0f, 1f)
                                onValueChange(value)
                            }
                        }
                    }
                }
            },
        shape = RoundedCornerShape(4.dp),
        color = Color.White.copy(alpha = 0.2f + value * 0.4f)
    ) {
        Box(contentAlignment = Alignment.Center) {
            Text(label, color = Color.White, style = MaterialTheme.typography.labelSmall)
        }
    }
}

@Composable
private fun InGameMenu(
    emulator: NativeEmulator,
    onDismiss: () -> Unit,
    onExit: () -> Unit,
    onToggleOpacitySlider: () -> Unit,
    onEditLayout: () -> Unit,
    profileManager: ControllerProfileManager,
    activeProfileName: String,
    onProfileSelected: (String) -> Unit,
    savePath: String,
    gamePath: String
) {
    var showExitConfirm by remember { mutableStateOf(false) }
    var showSaveSlots by remember { mutableStateOf(false) }
    var showLoadSlots by remember { mutableStateOf(false) }
    var showProfileList by remember { mutableStateOf(false) }
    var saveMessage by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(Unit) { emulator.pause() }

    // Get save file name from game path
    val saveBaseName = remember {
        File(gamePath).nameWithoutExtension
    }

    fun getSaveStatePath(slot: Int): String {
        return "$savePath/${saveBaseName}_slot$slot.sav"
    }

    fun saveStateExists(slot: Int): Boolean {
        return File(getSaveStatePath(slot)).exists()
    }

    AlertDialog(
        onDismissRequest = { emulator.run(); onDismiss() },
        title = { Text("Game Menu") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                TextButton(
                    onClick = { emulator.run(); onDismiss() },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(Icons.Default.PlayArrow, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text("Resume")
                }

                TextButton(
                    onClick = { emulator.reset(); onDismiss() },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(Icons.Default.Refresh, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text("Reset")
                }

                TextButton(
                    onClick = { emulator.testRender(); onDismiss() },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(Icons.Default.Build, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text("Test Render")
                }

                HorizontalDivider()

                // Save State
                TextButton(
                    onClick = { showSaveSlots = true },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(Icons.Default.Save, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text("Save State")
                }

                // Load State
                TextButton(
                    onClick = { showLoadSlots = true },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(Icons.Default.FolderOpen, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text("Load State")
                }

                HorizontalDivider()

                // Controls
                TextButton(
                    onClick = onToggleOpacitySlider,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(Icons.Default.Tune, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text("Control Settings")
                }

                TextButton(
                    onClick = { emulator.run(); onEditLayout() },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(Icons.Default.Edit, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text("Edit Button Layout")
                }

                TextButton(
                    onClick = { showProfileList = true },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(Icons.Default.Person, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text("Controller Profiles ($activeProfileName)")
                }

                HorizontalDivider()

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

                // Save message
                if (saveMessage != null) {
                    Text(
                        text = saveMessage!!,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.padding(top = 8.dp)
                    )
                }
            }
        },
        confirmButton = {}
    )

    // Save slots dialog
    if (showSaveSlots) {
        AlertDialog(
            onDismissRequest = { showSaveSlots = false },
            title = { Text("Save State") },
            text = {
                Column {
                    (1..5).forEach { slot ->
                        val exists = saveStateExists(slot)
                        TextButton(
                            onClick = {
                                val path = getSaveStatePath(slot)
                                val success = emulator.saveState(path)
                                saveMessage = if (success) "Saved to slot $slot" else "Save failed"
                                showSaveSlots = false
                            },
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text(
                                "Slot $slot${if (exists) " (overwrite)" else " (empty)"}",
                                modifier = Modifier.fillMaxWidth()
                            )
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showSaveSlots = false }) { Text("Cancel") }
            }
        )
    }

    // Load slots dialog
    if (showLoadSlots) {
        AlertDialog(
            onDismissRequest = { showLoadSlots = false },
            title = { Text("Load State") },
            text = {
                Column {
                    var hasAnySave = false
                    (1..5).forEach { slot ->
                        val exists = saveStateExists(slot)
                        if (exists) hasAnySave = true
                        TextButton(
                            onClick = {
                                val path = getSaveStatePath(slot)
                                val success = emulator.loadState(path)
                                saveMessage = if (success) "Loaded slot $slot" else "Load failed"
                                showLoadSlots = false
                            },
                            enabled = exists,
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text(
                                "Slot $slot${if (exists) "" else " (empty)"}",
                                modifier = Modifier.fillMaxWidth()
                            )
                        }
                    }
                    if (!hasAnySave) {
                        Text(
                            "No save states found",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(top = 8.dp)
                        )
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showLoadSlots = false }) { Text("Cancel") }
            }
        )
    }

    // Exit confirmation
    if (showExitConfirm) {
        AlertDialog(
            onDismissRequest = { showExitConfirm = false },
            title = { Text("Exit Game?") },
            text = { Text("Are you sure you want to exit? Unsaved progress will be lost.") },
            confirmButton = {
                TextButton(onClick = onExit) { Text("Exit") }
            },
            dismissButton = {
                TextButton(onClick = { showExitConfirm = false }) { Text("Cancel") }
            }
        )
    }

    // Profile list dialog
    if (showProfileList) {
        val profiles = remember { profileManager.listProfiles() }
        var showNewProfileInput by remember { mutableStateOf(false) }
        var newProfileName by remember { mutableStateOf("") }
        var profileToDelete by remember { mutableStateOf<String?>(null) }

        AlertDialog(
            onDismissRequest = { showProfileList = false },
            title = { Text("Controller Profiles") },
            text = {
                Column {
                    if (profiles.isEmpty()) {
                        Text(
                            "No saved profiles",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }

                    profiles.forEach { name ->
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable {
                                    onProfileSelected(name)
                                    showProfileList = false
                                    onDismiss()
                                }
                                .padding(vertical = 8.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.SpaceBetween
                        ) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                if (name == activeProfileName) {
                                    Icon(
                                        Icons.Default.Check,
                                        contentDescription = null,
                                        tint = MaterialTheme.colorScheme.primary,
                                        modifier = Modifier.size(20.dp)
                                    )
                                    Spacer(modifier = Modifier.width(8.dp))
                                }
                                Text(name)
                            }
                            if (name != "Default") {
                                IconButton(
                                    onClick = { profileToDelete = name },
                                    modifier = Modifier.size(24.dp)
                                ) {
                                    Icon(
                                        Icons.Default.Delete,
                                        contentDescription = "Delete",
                                        tint = MaterialTheme.colorScheme.error,
                                        modifier = Modifier.size(16.dp)
                                    )
                                }
                            }
                        }
                    }

                    HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))

                    if (showNewProfileInput) {
                        OutlinedTextField(
                            value = newProfileName,
                            onValueChange = { newProfileName = it },
                            label = { Text("Profile Name") },
                            singleLine = true,
                            modifier = Modifier.fillMaxWidth()
                        )
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.End
                        ) {
                            TextButton(onClick = { showNewProfileInput = false }) {
                                Text("Cancel")
                            }
                            TextButton(
                                onClick = {
                                    if (newProfileName.isNotBlank()) {
                                        val profile = ControllerProfile(name = newProfileName.trim())
                                        profileManager.saveProfile(profile)
                                        onProfileSelected(profile.name)
                                        showProfileList = false
                                        onDismiss()
                                    }
                                },
                                enabled = newProfileName.isNotBlank()
                            ) {
                                Text("Create")
                            }
                        }
                    } else {
                        TextButton(
                            onClick = { showNewProfileInput = true },
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Icon(Icons.Default.Add, contentDescription = null)
                            Spacer(modifier = Modifier.width(8.dp))
                            Text("New Profile")
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showProfileList = false }) { Text("Close") }
            }
        )

        // Delete profile confirmation
        profileToDelete?.let { name ->
            AlertDialog(
                onDismissRequest = { profileToDelete = null },
                title = { Text("Delete Profile?") },
                text = { Text("Delete profile \"$name\"? This cannot be undone.") },
                confirmButton = {
                    TextButton(onClick = {
                        profileManager.deleteProfile(name)
                        if (activeProfileName == name) {
                            onProfileSelected("Default")
                        }
                        profileToDelete = null
                        showProfileList = false
                    }) {
                        Text("Delete", color = MaterialTheme.colorScheme.error)
                    }
                },
                dismissButton = {
                    TextButton(onClick = { profileToDelete = null }) { Text("Cancel") }
                }
            )
        }
    }
}
