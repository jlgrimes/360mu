package com.x360mu.ui.screens

import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.selection.selectable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.x360mu.core.ButtonRemap
import com.x360mu.core.ControllerProfile
import com.x360mu.core.ControllerProfileManager
import com.x360mu.core.NativeEmulator
import com.x360mu.util.PreferencesManager
import org.json.JSONArray

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
    onBackClick: () -> Unit
) {
    val context = LocalContext.current
    val prefs = remember { PreferencesManager.getInstance(context) }

    // CPU settings
    var enableJit by remember { mutableStateOf(prefs.enableJit) }
    var interpreterFallback by remember { mutableStateOf(prefs.interpreterFallback) }

    // GPU settings
    var resolutionScale by remember { mutableStateOf(prefs.resolutionScale) }
    var enableVsync by remember { mutableStateOf(prefs.vsyncEnabled) }
    var frameSkip by remember { mutableStateOf(prefs.frameSkip) }

    // Audio settings
    var enableAudio by remember { mutableStateOf(prefs.enableAudio) }
    var audioBufferSize by remember { mutableStateOf(prefs.audioBufferSize) }

    // UI settings
    var showFps by remember { mutableStateOf(prefs.showFps) }
    var showPerfOverlay by remember { mutableStateOf(prefs.showPerfOverlay) }

    // Input settings
    var controlOpacity by remember { mutableStateOf(prefs.controlOpacity) }
    var controlLayoutLocked by remember { mutableStateOf(prefs.controlLayoutLocked) }
    var hapticEnabled by remember { mutableStateOf(prefs.hapticEnabled) }
    var activeProfile by remember { mutableStateOf(prefs.activeProfile) }
    val profileManager = remember { ControllerProfileManager(context) }
    var remapCount by remember {
        mutableStateOf(
            try {
                val json = prefs.buttonRemapsJson
                if (json != null) JSONArray(json).length() else 0
            } catch (_: Exception) { 0 }
        )
    }

    // Dialog state for input
    var showProfileDialog by remember { mutableStateOf(false) }
    var showRemapDialog by remember { mutableStateOf(false) }

    // Path settings
    var romsFolderUri by remember { mutableStateOf(prefs.romsFolderUri) }

    // Dialog state
    var showResolutionDialog by remember { mutableStateOf(false) }
    var showFrameSkipDialog by remember { mutableStateOf(false) }
    var showAudioBufferDialog by remember { mutableStateOf(false) }
    var showClearFolderDialog by remember { mutableStateOf(false) }

    // Folder picker
    val folderPicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? ->
        uri?.let {
            val takeFlags = Intent.FLAG_GRANT_READ_URI_PERMISSION or
                    Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            try {
                context.contentResolver.takePersistableUriPermission(it, takeFlags)
            } catch (_: Exception) { }
            prefs.romsFolderUri = it
            romsFolderUri = it
        }
    }

    fun getDisplayPath(uri: Uri?): String {
        if (uri == null) return "Not configured"
        return try {
            val docId = uri.lastPathSegment ?: return uri.toString()
            val parts = docId.split(":")
            if (parts.size >= 2) {
                val storageType = parts[0]
                val path = parts.getOrNull(1) ?: ""
                when {
                    storageType.equals("primary", ignoreCase = true) -> "Internal/$path"
                    else -> "$storageType/$path"
                }
            } else docId
        } catch (_: Exception) { uri.toString() }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Settings") },
                navigationIcon = {
                    IconButton(onClick = onBackClick) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                    }
                }
            )
        }
    ) { paddingValues ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(paddingValues)
        ) {
            // ── Game Library ──
            item { SettingsSection(title = "Game Library") }

            item {
                ListItem(
                    headlineContent = { Text("ROMs Folder") },
                    supportingContent = {
                        Text(
                            text = getDisplayPath(romsFolderUri),
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                    },
                    leadingContent = { Icon(Icons.Default.Folder, contentDescription = null) },
                    trailingContent = {
                        Icon(Icons.Default.ChevronRight, contentDescription = null,
                            tint = MaterialTheme.colorScheme.onSurfaceVariant)
                    },
                    modifier = Modifier.clickable { folderPicker.launch(null) }
                )
            }

            if (romsFolderUri != null) {
                item {
                    ListItem(
                        headlineContent = {
                            Text("Clear ROMs Folder", color = MaterialTheme.colorScheme.error)
                        },
                        supportingContent = { Text("Remove the configured folder and rescan") },
                        leadingContent = {
                            Icon(Icons.Default.DeleteForever, contentDescription = null,
                                tint = MaterialTheme.colorScheme.error)
                        },
                        modifier = Modifier.clickable { showClearFolderDialog = true }
                    )
                }
            }

            // ── CPU ──
            item { SettingsSection(title = "CPU") }

            item {
                SwitchSetting(
                    icon = Icons.Default.Speed,
                    title = "JIT Recompiler",
                    subtitle = "Enable dynamic recompilation for better performance",
                    checked = enableJit,
                    onCheckedChange = {
                        enableJit = it
                        prefs.enableJit = it
                    }
                )
            }

            item {
                SwitchSetting(
                    icon = Icons.Default.SwapHoriz,
                    title = "Interpreter Fallback",
                    subtitle = "Fall back to interpreter for unsupported instructions",
                    checked = interpreterFallback,
                    onCheckedChange = {
                        interpreterFallback = it
                        prefs.interpreterFallback = it
                    }
                )
            }

            // ── Graphics ──
            item { SettingsSection(title = "Graphics") }

            item {
                ClickableSetting(
                    icon = Icons.Default.AspectRatio,
                    title = "Resolution Scale",
                    subtitle = "${resolutionScale}x (${720 * resolutionScale}p)",
                    onClick = { showResolutionDialog = true }
                )
            }

            item {
                ClickableSetting(
                    icon = Icons.Default.FastForward,
                    title = "Frame Skip",
                    subtitle = if (frameSkip == 0) "Disabled" else "Skip $frameSkip frame(s)",
                    onClick = { showFrameSkipDialog = true }
                )
            }

            item {
                SwitchSetting(
                    icon = Icons.Default.Sync,
                    title = "VSync",
                    subtitle = "Synchronize frame rate with display refresh",
                    checked = enableVsync,
                    onCheckedChange = {
                        enableVsync = it
                        prefs.vsyncEnabled = it
                    }
                )
            }

            // ── Audio ──
            item { SettingsSection(title = "Audio") }

            item {
                SwitchSetting(
                    icon = Icons.Default.VolumeUp,
                    title = "Enable Audio",
                    subtitle = "Play game audio",
                    checked = enableAudio,
                    onCheckedChange = {
                        enableAudio = it
                        prefs.enableAudio = it
                    }
                )
            }

            item {
                ClickableSetting(
                    icon = Icons.Default.Tune,
                    title = "Audio Buffer Size",
                    subtitle = when (audioBufferSize) {
                        0 -> "Low (256) - Less latency"
                        1 -> "Medium (512) - Balanced"
                        else -> "High (1024) - More stable"
                    },
                    onClick = { showAudioBufferDialog = true }
                )
            }

            // ── Input ──
            item { SettingsSection(title = "Input") }

            item {
                Column(modifier = Modifier.padding(horizontal = 16.dp)) {
                    Text(
                        text = "Control Opacity",
                        style = MaterialTheme.typography.bodyLarge
                    )
                    Text(
                        text = "${(controlOpacity * 100).toInt()}%",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Slider(
                        value = controlOpacity,
                        onValueChange = {
                            controlOpacity = it
                            prefs.controlOpacity = it
                        },
                        valueRange = 0.1f..1.0f,
                        steps = 8,
                        modifier = Modifier.fillMaxWidth()
                    )
                }
            }

            item {
                SwitchSetting(
                    icon = Icons.Default.Lock,
                    title = "Lock Control Layout",
                    subtitle = "Prevent accidental repositioning of on-screen controls",
                    checked = controlLayoutLocked,
                    onCheckedChange = {
                        controlLayoutLocked = it
                        prefs.controlLayoutLocked = it
                    }
                )
            }

            item {
                SwitchSetting(
                    icon = Icons.Default.Vibration,
                    title = "Haptic Feedback",
                    subtitle = "Vibrate on touch control presses",
                    checked = hapticEnabled,
                    onCheckedChange = {
                        hapticEnabled = it
                        prefs.hapticEnabled = it
                    }
                )
            }

            item {
                ClickableSetting(
                    icon = Icons.Default.Person,
                    title = "Controller Profile",
                    subtitle = activeProfile,
                    onClick = { showProfileDialog = true }
                )
            }

            item {
                ClickableSetting(
                    icon = Icons.Default.SwapHoriz,
                    title = "Button Remapping",
                    subtitle = "$remapCount custom mapping(s)",
                    onClick = { showRemapDialog = true }
                )
            }

            // ── Interface ──
            item { SettingsSection(title = "Interface") }

            item {
                SwitchSetting(
                    icon = Icons.Default.Speed,
                    title = "Show FPS Counter",
                    subtitle = "Display frame rate during gameplay",
                    checked = showFps,
                    onCheckedChange = {
                        showFps = it
                        prefs.showFps = it
                    }
                )
            }

            item {
                SwitchSetting(
                    icon = Icons.Default.Analytics,
                    title = "Performance Overlay",
                    subtitle = "Show frame time and emulator state",
                    checked = showPerfOverlay,
                    onCheckedChange = {
                        showPerfOverlay = it
                        prefs.showPerfOverlay = it
                    }
                )
            }

            // ── About ──
            item { SettingsSection(title = "About") }

            item {
                InfoSetting(
                    icon = Icons.Default.Info,
                    title = "Version",
                    value = "0.1.0-alpha"
                )
            }

            item {
                ClickableSetting(
                    icon = Icons.Default.Code,
                    title = "Source Code",
                    subtitle = "View on GitHub",
                    onClick = { /* Open GitHub */ }
                )
            }

            item { Spacer(modifier = Modifier.height(32.dp)) }
        }
    }

    // Resolution dialog
    if (showResolutionDialog) {
        AlertDialog(
            onDismissRequest = { showResolutionDialog = false },
            title = { Text("Resolution Scale") },
            text = {
                Column {
                    listOf(
                        1 to "1x (720p) - Best Performance",
                        2 to "2x (1440p) - Balanced",
                        3 to "3x (2160p) - High Quality"
                    ).forEach { (scale, label) ->
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .selectable(
                                    selected = resolutionScale == scale,
                                    onClick = {
                                        resolutionScale = scale
                                        prefs.resolutionScale = scale
                                        showResolutionDialog = false
                                    }
                                )
                                .padding(12.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            RadioButton(selected = resolutionScale == scale, onClick = null)
                            Spacer(modifier = Modifier.width(12.dp))
                            Text(label)
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showResolutionDialog = false }) { Text("Cancel") }
            }
        )
    }

    // Frame skip dialog
    if (showFrameSkipDialog) {
        AlertDialog(
            onDismissRequest = { showFrameSkipDialog = false },
            title = { Text("Frame Skip") },
            text = {
                Column {
                    listOf(
                        0 to "Disabled - Best visual quality",
                        1 to "Skip 1 - Slight speedup",
                        2 to "Skip 2 - Moderate speedup",
                        3 to "Skip 3 - Maximum speedup"
                    ).forEach { (skip, label) ->
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .selectable(
                                    selected = frameSkip == skip,
                                    onClick = {
                                        frameSkip = skip
                                        prefs.frameSkip = skip
                                        showFrameSkipDialog = false
                                    }
                                )
                                .padding(12.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            RadioButton(selected = frameSkip == skip, onClick = null)
                            Spacer(modifier = Modifier.width(12.dp))
                            Text(label)
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showFrameSkipDialog = false }) { Text("Cancel") }
            }
        )
    }

    // Audio buffer dialog
    if (showAudioBufferDialog) {
        AlertDialog(
            onDismissRequest = { showAudioBufferDialog = false },
            title = { Text("Audio Buffer Size") },
            text = {
                Column {
                    listOf(
                        0 to "Low (256) - Less latency, may crackle",
                        1 to "Medium (512) - Balanced",
                        2 to "High (1024) - Most stable, more latency"
                    ).forEach { (size, label) ->
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .selectable(
                                    selected = audioBufferSize == size,
                                    onClick = {
                                        audioBufferSize = size
                                        prefs.audioBufferSize = size
                                        showAudioBufferDialog = false
                                    }
                                )
                                .padding(12.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            RadioButton(selected = audioBufferSize == size, onClick = null)
                            Spacer(modifier = Modifier.width(12.dp))
                            Text(label)
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showAudioBufferDialog = false }) { Text("Cancel") }
            }
        )
    }

    // Clear folder confirmation
    if (showClearFolderDialog) {
        AlertDialog(
            onDismissRequest = { showClearFolderDialog = false },
            icon = { Icon(Icons.Default.Warning, contentDescription = null) },
            title = { Text("Clear ROMs Folder?") },
            text = {
                Text("This will remove the configured ROMs folder. You'll need to select a folder again to see your games.")
            },
            confirmButton = {
                TextButton(onClick = {
                    prefs.romsFolderUri = null
                    romsFolderUri = null
                    showClearFolderDialog = false
                }) {
                    Text("Clear", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { showClearFolderDialog = false }) { Text("Cancel") }
            }
        )
    }

    // Profile selection dialog
    if (showProfileDialog) {
        val profiles = remember { profileManager.listProfiles() }
        var newName by remember { mutableStateOf("") }
        var showCreate by remember { mutableStateOf(false) }

        AlertDialog(
            onDismissRequest = { showProfileDialog = false },
            title = { Text("Controller Profiles") },
            text = {
                Column {
                    profiles.forEach { name ->
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .selectable(
                                    selected = activeProfile == name,
                                    onClick = {
                                        activeProfile = name
                                        prefs.activeProfile = name
                                        showProfileDialog = false
                                    }
                                )
                                .padding(12.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            RadioButton(selected = activeProfile == name, onClick = null)
                            Spacer(modifier = Modifier.width(12.dp))
                            Text(name)
                        }
                    }

                    if (profiles.isEmpty()) {
                        Text("No profiles. Create one below.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(12.dp))
                    }

                    HorizontalDivider(modifier = Modifier.padding(vertical = 4.dp))

                    if (showCreate) {
                        OutlinedTextField(
                            value = newName,
                            onValueChange = { newName = it },
                            label = { Text("Profile Name") },
                            singleLine = true,
                            modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp)
                        )
                        Row(
                            modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp),
                            horizontalArrangement = Arrangement.End
                        ) {
                            TextButton(onClick = { showCreate = false }) { Text("Cancel") }
                            TextButton(
                                onClick = {
                                    if (newName.isNotBlank()) {
                                        val profile = ControllerProfile(name = newName.trim())
                                        profileManager.saveProfile(profile)
                                        activeProfile = profile.name
                                        prefs.activeProfile = profile.name
                                        showProfileDialog = false
                                    }
                                },
                                enabled = newName.isNotBlank()
                            ) { Text("Create") }
                        }
                    } else {
                        TextButton(onClick = { showCreate = true }) {
                            Icon(Icons.Default.Add, contentDescription = null)
                            Spacer(modifier = Modifier.width(4.dp))
                            Text("New Profile")
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showProfileDialog = false }) { Text("Close") }
            }
        )
    }

    // Button remapping dialog
    if (showRemapDialog) {
        val buttonNames = listOf(
            "A" to NativeEmulator.Button.A,
            "B" to NativeEmulator.Button.B,
            "X" to NativeEmulator.Button.X,
            "Y" to NativeEmulator.Button.Y,
            "D-Pad Up" to NativeEmulator.Button.DPAD_UP,
            "D-Pad Down" to NativeEmulator.Button.DPAD_DOWN,
            "D-Pad Left" to NativeEmulator.Button.DPAD_LEFT,
            "D-Pad Right" to NativeEmulator.Button.DPAD_RIGHT,
            "Start" to NativeEmulator.Button.START,
            "Back" to NativeEmulator.Button.BACK,
            "LB" to NativeEmulator.Button.LEFT_BUMPER,
            "RB" to NativeEmulator.Button.RIGHT_BUMPER,
            "L3" to NativeEmulator.Button.LEFT_STICK,
            "R3" to NativeEmulator.Button.RIGHT_STICK,
            "Guide" to NativeEmulator.Button.GUIDE
        )

        // Load current remaps
        var remaps by remember {
            mutableStateOf(
                try {
                    val json = prefs.buttonRemapsJson
                    if (json != null) {
                        val arr = JSONArray(json)
                        (0 until arr.length()).map { ButtonRemap.fromJson(arr.getJSONObject(it)) }
                    } else emptyList()
                } catch (_: Exception) { emptyList() }
            )
        }

        AlertDialog(
            onDismissRequest = { showRemapDialog = false },
            title = { Text("Button Remapping") },
            text = {
                Column {
                    Text(
                        "Map keyboard keys to Xbox 360 buttons.\nFormat: Android keyCode -> Xbox button",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(bottom = 8.dp)
                    )

                    remaps.forEachIndexed { index, remap ->
                        Row(
                            modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.SpaceBetween
                        ) {
                            Text(
                                "Key ${remap.sourceCode} -> ${buttonNames.find { it.second == remap.targetButton }?.first ?: "?"}",
                                style = MaterialTheme.typography.bodyMedium
                            )
                            IconButton(
                                onClick = {
                                    remaps = remaps.toMutableList().apply { removeAt(index) }
                                    val arr = JSONArray()
                                    remaps.forEach { arr.put(it.toJson()) }
                                    prefs.buttonRemapsJson = if (arr.length() > 0) arr.toString() else null
                                    remapCount = remaps.size
                                },
                                modifier = Modifier.size(24.dp)
                            ) {
                                Icon(Icons.Default.Close, contentDescription = "Remove",
                                    modifier = Modifier.size(16.dp))
                            }
                        }
                    }

                    if (remaps.isEmpty()) {
                        Text(
                            "No custom mappings configured",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(vertical = 8.dp)
                        )
                    }

                    HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))

                    Text(
                        "To add a mapping, connect a keyboard or controller and press a key/button while a game is running.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = { showRemapDialog = false }) { Text("Close") }
            },
            dismissButton = {
                if (remaps.isNotEmpty()) {
                    TextButton(onClick = {
                        remaps = emptyList()
                        prefs.buttonRemapsJson = null
                        remapCount = 0
                    }) {
                        Text("Clear All", color = MaterialTheme.colorScheme.error)
                    }
                }
            }
        )
    }
}

@Composable
private fun SettingsSection(title: String) {
    Text(
        text = title,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp)
    )
}

@Composable
private fun SwitchSetting(
    icon: ImageVector,
    title: String,
    subtitle: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit
) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = { Text(subtitle) },
        leadingContent = { Icon(icon, contentDescription = null) },
        trailingContent = {
            Switch(checked = checked, onCheckedChange = onCheckedChange)
        },
        modifier = Modifier.clickable { onCheckedChange(!checked) }
    )
}

@Composable
private fun ClickableSetting(
    icon: ImageVector,
    title: String,
    subtitle: String,
    onClick: () -> Unit
) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = { Text(subtitle) },
        leadingContent = { Icon(icon, contentDescription = null) },
        trailingContent = {
            Icon(Icons.Default.ChevronRight, contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant)
        },
        modifier = Modifier.clickable(onClick = onClick)
    )
}

@Composable
private fun InfoSetting(
    icon: ImageVector,
    title: String,
    value: String
) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = { Text(value) },
        leadingContent = { Icon(icon, contentDescription = null) }
    )
}
