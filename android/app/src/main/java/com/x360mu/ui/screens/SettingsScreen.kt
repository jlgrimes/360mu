package com.x360mu.ui.screens

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
import androidx.compose.ui.unit.dp

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
    onBackClick: () -> Unit
) {
    // Settings state
    var enableJit by remember { mutableStateOf(true) }
    var resolutionScale by remember { mutableStateOf(1) }
    var enableVsync by remember { mutableStateOf(true) }
    var enableAudio by remember { mutableStateOf(true) }
    var showFps by remember { mutableStateOf(true) }
    
    var showResolutionDialog by remember { mutableStateOf(false) }
    
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
                    onCheckedChange = { enableJit = it }
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
                    onCheckedChange = { enableVsync = it }
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

