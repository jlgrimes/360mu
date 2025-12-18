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
import com.x360mu.util.PreferencesManager

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
    onBackClick: () -> Unit
) {
    val context = LocalContext.current
    val prefs = remember { PreferencesManager.getInstance(context) }
    
    // Settings state - load from preferences
    var enableJit by remember { mutableStateOf(prefs.enableJit) }
    var resolutionScale by remember { mutableStateOf(prefs.resolutionScale) }
    var enableVsync by remember { mutableStateOf(prefs.vsyncEnabled) }
    var enableAudio by remember { mutableStateOf(true) }
    var showFps by remember { mutableStateOf(true) }
    var romsFolderUri by remember { mutableStateOf(prefs.romsFolderUri) }
    
    var showResolutionDialog by remember { mutableStateOf(false) }
    var showClearFolderDialog by remember { mutableStateOf(false) }
    
    // Folder picker launcher
    val folderPicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? ->
        uri?.let {
            // Get persistent permission for the folder
            val takeFlags = Intent.FLAG_GRANT_READ_URI_PERMISSION or 
                           Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            try {
                context.contentResolver.takePersistableUriPermission(it, takeFlags)
            } catch (_: Exception) { }
            
            // Save the folder URI
            prefs.romsFolderUri = it
            romsFolderUri = it
        }
    }
    
    // Helper to get display path from URI
    fun getDisplayPath(uri: Uri?): String {
        if (uri == null) return "Not configured"
        
        val uriString = uri.toString()
        // Extract a user-friendly path
        return try {
            val docId = uri.lastPathSegment ?: return uriString
            val parts = docId.split(":")
            if (parts.size >= 2) {
                val storageType = parts[0]
                val path = parts.getOrNull(1) ?: ""
                when {
                    storageType.equals("primary", ignoreCase = true) -> "Internal/$path"
                    else -> "$storageType/$path"
                }
            } else {
                docId
            }
        } catch (_: Exception) {
            uriString
        }
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
            // ROMs Section - Put this first since it's most important
            item {
                SettingsSection(title = "Game Library")
            }
            
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
                    leadingContent = {
                        Icon(Icons.Default.Folder, contentDescription = null)
                    },
                    trailingContent = {
                        Icon(
                            Icons.Default.ChevronRight,
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    },
                    modifier = Modifier.clickable { folderPicker.launch(null) }
                )
            }
            
            if (romsFolderUri != null) {
                item {
                    ListItem(
                        headlineContent = { 
                            Text(
                                "Clear ROMs Folder",
                                color = MaterialTheme.colorScheme.error
                            )
                        },
                        supportingContent = { 
                            Text("Remove the configured folder and rescan")
                        },
                        leadingContent = {
                            Icon(
                                Icons.Default.DeleteForever, 
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.error
                            )
                        },
                        modifier = Modifier.clickable { showClearFolderDialog = true }
                    )
                }
            }
            
            // CPU Section
            item {
                SettingsSection(title = "CPU")
            }
            
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
            
            // GPU Section
            item {
                SettingsSection(title = "Graphics")
            }
            
            item {
                ClickableSetting(
                    icon = Icons.Default.AspectRatio,
                    title = "Resolution Scale",
                    subtitle = "${resolutionScale}x (${720 * resolutionScale}p)",
                    onClick = { showResolutionDialog = true }
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
            
            // Audio Section
            item {
                SettingsSection(title = "Audio")
            }
            
            item {
                SwitchSetting(
                    icon = Icons.Default.VolumeUp,
                    title = "Enable Audio",
                    subtitle = "Play game audio",
                    checked = enableAudio,
                    onCheckedChange = { enableAudio = it }
                )
            }
            
            // UI Section
            item {
                SettingsSection(title = "Interface")
            }
            
            item {
                SwitchSetting(
                    icon = Icons.Default.Speed,
                    title = "Show FPS Counter",
                    subtitle = "Display frame rate during gameplay",
                    checked = showFps,
                    onCheckedChange = { showFps = it }
                )
            }
            
            // About Section
            item {
                SettingsSection(title = "About")
            }
            
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
            
            // Bottom spacing
            item {
                Spacer(modifier = Modifier.height(32.dp))
            }
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
                            RadioButton(
                                selected = resolutionScale == scale,
                                onClick = null
                            )
                            Spacer(modifier = Modifier.width(12.dp))
                            Text(label)
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showResolutionDialog = false }) {
                    Text("Cancel")
                }
            }
        )
    }
    
    // Clear folder confirmation dialog
    if (showClearFolderDialog) {
        AlertDialog(
            onDismissRequest = { showClearFolderDialog = false },
            icon = { Icon(Icons.Default.Warning, contentDescription = null) },
            title = { Text("Clear ROMs Folder?") },
            text = { 
                Text("This will remove the configured ROMs folder. You'll need to select a folder again to see your games.")
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        prefs.romsFolderUri = null
                        romsFolderUri = null
                        showClearFolderDialog = false
                    }
                ) {
                    Text("Clear", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { showClearFolderDialog = false }) {
                    Text("Cancel")
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
        leadingContent = {
            Icon(icon, contentDescription = null)
        },
        trailingContent = {
            Switch(
                checked = checked,
                onCheckedChange = onCheckedChange
            )
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
        leadingContent = {
            Icon(icon, contentDescription = null)
        },
        trailingContent = {
            Icon(
                Icons.Default.ChevronRight,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant
            )
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
        leadingContent = {
            Icon(icon, contentDescription = null)
        }
    )
}
