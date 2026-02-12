package com.x360mu.ui.screens

import android.content.Intent
import android.net.Uri
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.DpOffset
import androidx.compose.ui.unit.dp
import com.x360mu.BuildConfig
import com.x360mu.util.PreferencesManager
import com.x360mu.util.RomFile
import com.x360mu.util.RomScanner
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.*

private const val TAG = "360mu-LibraryScreen"

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun LibraryScreen(
    emulator: com.x360mu.core.NativeEmulator,
    onGameSelected: (String) -> Unit,
    onSettingsClick: () -> Unit,
    onTestRenderClick: () -> Unit = {},
    onLogsClick: () -> Unit = {}
) {
    Log.i(TAG, "LibraryScreen composable rendering")

    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val prefs = remember { PreferencesManager.getInstance(context) }

    var games by remember { mutableStateOf<List<RomFile>>(emptyList()) }
    var isLoading by remember { mutableStateOf(false) }
    var isScanning by remember { mutableStateOf(false) }
    var scanProgress by remember { mutableStateOf(0) }
    var scanTotal by remember { mutableStateOf(0) }
    var romsFolderConfigured by remember { mutableStateOf(prefs.isRomsFolderConfigured) }
    var selectedGame by remember { mutableStateOf<RomFile?>(null) }
    var showGameInfoDialog by remember { mutableStateOf(false) }
    var showContextMenu by remember { mutableStateOf(false) }
    var contextMenuGame by remember { mutableStateOf<RomFile?>(null) }
    var gameInfo by remember { mutableStateOf<com.x360mu.core.NativeEmulator.GameInfo?>(null) }
    var isLoadingInfo by remember { mutableStateOf(false) }

    fun scanRomsFolder() {
        val folderUri = prefs.romsFolderUri ?: return

        scope.launch {
            isScanning = true
            scanProgress = 0
            scanTotal = 0
            Log.i(TAG, "Starting ROM scan...")

            val foundRoms = withContext(Dispatchers.IO) {
                RomScanner.scanFolder(context, folderUri)
            }

            games = foundRoms
            isScanning = false
            Log.i(TAG, "ROM scan complete, found ${foundRoms.size} games")
        }
    }

    val folderPicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? ->
        uri?.let {
            Log.i(TAG, "Folder selected: $it")
            val takeFlags = Intent.FLAG_GRANT_READ_URI_PERMISSION or
                    Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            try {
                context.contentResolver.takePersistableUriPermission(it, takeFlags)
            } catch (e: Exception) {
                Log.e(TAG, "Failed to get persistent permission: ${e.message}")
            }
            prefs.romsFolderUri = it
            romsFolderConfigured = true
            scanRomsFolder()
        }
    }

    LaunchedEffect(romsFolderConfigured) {
        if (romsFolderConfigured) {
            scanRomsFolder()
        }
    }

    Scaffold(
        topBar = {
            LargeTopAppBar(
                title = {
                    Column {
                        Text(
                            text = "360\u03bc",
                            style = MaterialTheme.typography.headlineLarge,
                            fontWeight = FontWeight.Bold
                        )
                        Text(
                            text = "Xbox 360 Emulator",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                        val buildTime = remember {
                            val timestamp = BuildConfig.BUILD_TIME.toLongOrNull() ?: 0L
                            val sdf = SimpleDateFormat("MMM dd, HH:mm", Locale.US)
                            sdf.format(Date(timestamp))
                        }
                        Text(
                            text = "v${BuildConfig.VERSION_NAME} \u2022 Build ${BuildConfig.VERSION_CODE} \u2022 $buildTime",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
                        )
                    }
                },
                actions = {
                    IconButton(onClick = {
                        Log.i(TAG, "Test Render button clicked")
                        onTestRenderClick()
                    }) {
                        Icon(Icons.Default.Build, contentDescription = "Test Render")
                    }
                    if (romsFolderConfigured) {
                        IconButton(
                            onClick = { scanRomsFolder() },
                            enabled = !isScanning
                        ) {
                            Icon(Icons.Default.Refresh, contentDescription = "Refresh")
                        }
                    }
                    IconButton(onClick = onLogsClick) {
                        Icon(Icons.Default.BugReport, contentDescription = "Logs")
                    }
                    IconButton(onClick = onSettingsClick) {
                        Icon(Icons.Default.Settings, contentDescription = "Settings")
                    }
                },
                colors = TopAppBarDefaults.largeTopAppBarColors(
                    containerColor = MaterialTheme.colorScheme.background
                )
            )
        },
        floatingActionButton = {
            if (romsFolderConfigured) {
                ExtendedFloatingActionButton(
                    onClick = { folderPicker.launch(null) },
                    icon = { Icon(Icons.Default.FolderOpen, contentDescription = null) },
                    text = { Text("Change Folder") },
                    containerColor = MaterialTheme.colorScheme.secondary
                )
            }
        }
    ) { paddingValues ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(paddingValues)
        ) {
            when {
                !romsFolderConfigured -> {
                    SetupScreen(onSelectFolder = { folderPicker.launch(null) })
                }

                isScanning -> {
                    Box(
                        modifier = Modifier.fillMaxSize(),
                        contentAlignment = Alignment.Center
                    ) {
                        Column(horizontalAlignment = Alignment.CenterHorizontally) {
                            CircularProgressIndicator(
                                color = MaterialTheme.colorScheme.primary
                            )
                            Spacer(modifier = Modifier.height(16.dp))
                            Text(
                                text = "Scanning for games...",
                                style = MaterialTheme.typography.bodyLarge
                            )
                            if (scanTotal > 0) {
                                Spacer(modifier = Modifier.height(8.dp))
                                LinearProgressIndicator(
                                    progress = { scanProgress.toFloat() / scanTotal },
                                    modifier = Modifier
                                        .width(200.dp)
                                        .padding(top = 8.dp)
                                )
                                Text(
                                    text = "$scanProgress / $scanTotal files checked",
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }
                        }
                    }
                }

                games.isEmpty() -> {
                    EmptyLibraryState(
                        onChangeFolder = { folderPicker.launch(null) },
                        onRefresh = { scanRomsFolder() }
                    )
                }

                else -> {
                    Column {
                        // Game count header
                        Text(
                            text = "${games.size} game${if (games.size != 1) "s" else ""} found",
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)
                        )

                        LazyVerticalGrid(
                            columns = GridCells.Adaptive(minSize = 160.dp),
                            contentPadding = PaddingValues(16.dp),
                            horizontalArrangement = Arrangement.spacedBy(12.dp),
                            verticalArrangement = Arrangement.spacedBy(12.dp)
                        ) {
                            items(games) { game ->
                                GameCard(
                                    game = game,
                                    onClick = {
                                        val realPath = RomScanner.getRomRealPath(context, game)
                                        if (realPath != null) {
                                            Log.i(TAG, "Launching game: $realPath")
                                            onGameSelected(realPath)
                                        } else {
                                            Log.e(TAG, "Could not resolve real path for: ${game.name}")
                                        }
                                    },
                                    onLongClick = {
                                        contextMenuGame = game
                                        showContextMenu = true
                                    }
                                )
                            }
                        }
                    }
                }
            }

            // Loading overlay
            AnimatedVisibility(
                visible = isLoading,
                enter = fadeIn(),
                exit = fadeOut()
            ) {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .background(Color.Black.copy(alpha = 0.5f)),
                    contentAlignment = Alignment.Center
                ) {
                    CircularProgressIndicator(color = MaterialTheme.colorScheme.primary)
                }
            }
        }
    }

    // Context menu dialog
    if (showContextMenu && contextMenuGame != null) {
        val game = contextMenuGame!!
        AlertDialog(
            onDismissRequest = {
                showContextMenu = false
                contextMenuGame = null
            },
            title = { Text(game.displayName, maxLines = 2, overflow = TextOverflow.Ellipsis) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    // Game info
                    Text(
                        text = "Type: ${game.type.name}",
                        style = MaterialTheme.typography.bodyMedium
                    )
                    Text(
                        text = "Size: ${RomScanner.formatFileSize(game.size)}",
                        style = MaterialTheme.typography.bodyMedium
                    )
                    Text(
                        text = "Format: .${game.extension}",
                        style = MaterialTheme.typography.bodyMedium
                    )
                    if (game.realPath != null) {
                        Text(
                            text = "Path: ${game.realPath}",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis
                        )
                    }

                    Spacer(modifier = Modifier.height(12.dp))
                    HorizontalDivider()
                    Spacer(modifier = Modifier.height(4.dp))

                    // Actions
                    TextButton(
                        onClick = {
                            val realPath = RomScanner.getRomRealPath(context, game)
                            if (realPath != null) {
                                onGameSelected(realPath)
                            }
                            showContextMenu = false
                            contextMenuGame = null
                        },
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Icon(Icons.Default.PlayArrow, contentDescription = null)
                        Spacer(modifier = Modifier.width(8.dp))
                        Text("Launch Game")
                    }

                    TextButton(
                        onClick = {
                            selectedGame = game
                            showContextMenu = false
                            val realPath = RomScanner.getRomRealPath(context, game)
                            if (realPath != null) {
                                isLoadingInfo = true
                                gameInfo = null
                                showGameInfoDialog = true
                                scope.launch {
                                    val info = withContext(Dispatchers.IO) {
                                        try {
                                            emulator.getGameInfo(realPath)
                                        } catch (e: Exception) {
                                            Log.e(TAG, "Failed to get game info: ${e.message}", e)
                                            null
                                        }
                                    }
                                    gameInfo = info
                                    isLoadingInfo = false
                                }
                            } else {
                                showGameInfoDialog = true
                            }
                        },
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Icon(Icons.Default.Info, contentDescription = null)
                        Spacer(modifier = Modifier.width(8.dp))
                        Text("Game Info")
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    showContextMenu = false
                    contextMenuGame = null
                }) { Text("Close") }
            }
        )
    }

    // Game info dialog
    if (showGameInfoDialog && selectedGame != null) {
        val game = selectedGame!!
        AlertDialog(
            onDismissRequest = {
                showGameInfoDialog = false
                selectedGame = null
                gameInfo = null
            },
            icon = { Icon(Icons.Default.Gamepad, contentDescription = null) },
            title = { Text(game.displayName, maxLines = 2, overflow = TextOverflow.Ellipsis) },
            text = {
                Column(
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                    modifier = Modifier.heightIn(max = 400.dp)
                        .then(Modifier.verticalScroll(rememberScrollState()))
                ) {
                    // Basic file info
                    InfoRow("File Name", game.name)
                    InfoRow("Type", when (game.type) {
                        com.x360mu.util.GameType.DISC -> "Full Disc Image (ISO)"
                        com.x360mu.util.GameType.XBLA -> "Xbox Executable (XEX)"
                        else -> "Unknown"
                    })
                    InfoRow("Size", RomScanner.formatFileSize(game.size))

                    if (isLoadingInfo) {
                        Spacer(modifier = Modifier.height(8.dp))
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            CircularProgressIndicator(
                                modifier = Modifier.size(16.dp),
                                strokeWidth = 2.dp
                            )
                            Text(
                                "Analyzing XEX...",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }

                    gameInfo?.let { info ->
                        Spacer(modifier = Modifier.height(4.dp))
                        HorizontalDivider()
                        Spacer(modifier = Modifier.height(4.dp))

                        // Compatibility badge
                        val badgeColor = when {
                            info.compatibilityPercent >= 90 -> Color(0xFF4CAF50)
                            info.compatibilityPercent >= 70 -> Color(0xFF8BC34A)
                            info.compatibilityPercent >= 50 -> Color(0xFFFFC107)
                            info.compatibilityPercent >= 30 -> Color(0xFFFF9800)
                            else -> Color(0xFFF44336)
                        }
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            Surface(
                                shape = RoundedCornerShape(6.dp),
                                color = badgeColor
                            ) {
                                Text(
                                    text = "${info.compatibilityRating} (${info.compatibilityPercent}%)",
                                    style = MaterialTheme.typography.labelMedium,
                                    color = Color.White,
                                    fontWeight = FontWeight.Bold,
                                    modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp)
                                )
                            }
                            Text(
                                text = "${info.resolvedImports}/${info.totalImports} imports",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }

                        Spacer(modifier = Modifier.height(4.dp))
                        HorizontalDivider()
                        Spacer(modifier = Modifier.height(4.dp))

                        // XEX metadata
                        Text(
                            "XEX Metadata",
                            style = MaterialTheme.typography.titleSmall,
                            fontWeight = FontWeight.Bold
                        )
                        InfoRow("Title ID", info.titleId)
                        InfoRow("Media ID", info.mediaId)
                        if (info.moduleName.isNotEmpty()) {
                            InfoRow("Module", info.moduleName)
                        }
                        InfoRow("Version", "${info.version} (base ${info.baseVersion})")
                        if (info.discCount > 0) {
                            InfoRow("Disc", "${info.discNumber} of ${info.discCount}")
                        }
                        InfoRow("Region", info.gameRegion)

                        Spacer(modifier = Modifier.height(4.dp))
                        HorizontalDivider()
                        Spacer(modifier = Modifier.height(4.dp))

                        // Technical details
                        Text(
                            "Technical Details",
                            style = MaterialTheme.typography.titleSmall,
                            fontWeight = FontWeight.Bold
                        )
                        InfoRow("Base Address", info.baseAddress)
                        InfoRow("Entry Point", info.entryPoint)
                        InfoRow("Image Size", formatHexSize(info.imageSize))
                        InfoRow("Stack Size", formatHexSize(info.stackSize))
                        InfoRow("Heap Size", formatHexSize(info.heapSize))
                        InfoRow("Sections", info.sectionCount.toString())

                        // Import libraries breakdown
                        if (info.importLibraries.isNotEmpty()) {
                            Spacer(modifier = Modifier.height(4.dp))
                            HorizontalDivider()
                            Spacer(modifier = Modifier.height(4.dp))

                            Text(
                                "Import Libraries",
                                style = MaterialTheme.typography.titleSmall,
                                fontWeight = FontWeight.Bold
                            )
                            info.importLibraries.forEach { (lib, count) ->
                                Row(
                                    modifier = Modifier.fillMaxWidth(),
                                    horizontalArrangement = Arrangement.SpaceBetween
                                ) {
                                    Text(
                                        text = lib,
                                        style = MaterialTheme.typography.bodySmall,
                                        modifier = Modifier.weight(1f)
                                    )
                                    Text(
                                        text = "$count imports",
                                        style = MaterialTheme.typography.bodySmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant
                                    )
                                }
                            }
                        }
                    }

                    if (!isLoadingInfo && gameInfo == null) {
                        // Fallback: no XEX info available
                        InfoRow("Extension", ".${game.extension}")
                        if (game.realPath != null) {
                            InfoRow("Full Path", game.realPath)
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    showGameInfoDialog = false
                    selectedGame = null
                    gameInfo = null
                }) { Text("Close") }
            }
        )
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Column {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.primary
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
            maxLines = 3,
            overflow = TextOverflow.Ellipsis
        )
    }
}

private fun formatHexSize(bytes: Long): String {
    return when {
        bytes >= 1024 * 1024 -> "0x${bytes.toString(16).uppercase()} (${bytes / (1024 * 1024)} MB)"
        bytes >= 1024 -> "0x${bytes.toString(16).uppercase()} (${bytes / 1024} KB)"
        bytes > 0 -> "0x${bytes.toString(16).uppercase()} ($bytes B)"
        else -> "0"
    }
}

@Composable
private fun SetupScreen(onSelectFolder: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .padding(32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Box(
            modifier = Modifier
                .border(2.dp, MaterialTheme.colorScheme.primary, RoundedCornerShape(16.dp))
                .padding(32.dp)
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Icon(
                    imageVector = Icons.Default.SportsEsports,
                    contentDescription = null,
                    modifier = Modifier.size(96.dp),
                    tint = MaterialTheme.colorScheme.primary
                )
                Spacer(modifier = Modifier.height(24.dp))
                Text(
                    text = "360\u03bc",
                    style = MaterialTheme.typography.displaySmall,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.primary
                )
                Text(
                    text = "Xbox 360 Emulator",
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Spacer(modifier = Modifier.height(32.dp))
                Text(
                    text = "Welcome!",
                    style = MaterialTheme.typography.headlineSmall,
                    fontWeight = FontWeight.SemiBold
                )
                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    text = "Select your ROMs folder to get started.\nThe app will scan for .iso and .xex files.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center
                )
                Spacer(modifier = Modifier.height(32.dp))
                Button(
                    onClick = onSelectFolder,
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(56.dp),
                    shape = RoundedCornerShape(12.dp)
                ) {
                    Icon(Icons.Default.FolderOpen, contentDescription = null)
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(
                        text = "Select ROMs Folder",
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold
                    )
                }
            }
        }
    }
}

@Composable
private fun EmptyLibraryState(
    onChangeFolder: () -> Unit,
    onRefresh: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Icon(
            imageVector = Icons.Default.SearchOff,
            contentDescription = null,
            modifier = Modifier.size(64.dp),
            tint = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Spacer(modifier = Modifier.height(16.dp))
        Text(
            text = "No Games Found",
            style = MaterialTheme.typography.headlineSmall,
            fontWeight = FontWeight.SemiBold
        )
        Spacer(modifier = Modifier.height(8.dp))
        Text(
            text = "No .iso or .xex files were found in the selected folder.\nMake sure your ROMs are in the correct location.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center
        )
        Spacer(modifier = Modifier.height(24.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            OutlinedButton(onClick = onRefresh, modifier = Modifier.weight(1f)) {
                Icon(Icons.Default.Refresh, contentDescription = null)
                Spacer(modifier = Modifier.width(8.dp))
                Text("Rescan")
            }
            Button(onClick = onChangeFolder, modifier = Modifier.weight(1f)) {
                Icon(Icons.Default.FolderOpen, contentDescription = null)
                Spacer(modifier = Modifier.width(8.dp))
                Text("Change Folder")
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun GameCard(
    game: RomFile,
    onClick: () -> Unit,
    onLongClick: () -> Unit
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .aspectRatio(0.75f)
            .combinedClickable(
                onClick = onClick,
                onLongClick = onLongClick
            ),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant
        )
    ) {
        Box(modifier = Modifier.fillMaxSize()) {
            // Gradient background
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(
                        brush = Brush.verticalGradient(
                            colors = listOf(
                                MaterialTheme.colorScheme.primary.copy(alpha = 0.3f),
                                MaterialTheme.colorScheme.primary.copy(alpha = 0.1f)
                            )
                        )
                    )
            )

            // Game icon placeholder + size
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(12.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center
            ) {
                Icon(
                    imageVector = Icons.Default.Gamepad,
                    contentDescription = null,
                    modifier = Modifier.size(48.dp),
                    tint = MaterialTheme.colorScheme.primary
                )
                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    text = RomScanner.formatFileSize(game.size),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            // Game info at bottom
            Column(
                modifier = Modifier
                    .align(Alignment.BottomStart)
                    .fillMaxWidth()
                    .background(
                        brush = Brush.verticalGradient(
                            colors = listOf(
                                Color.Transparent,
                                Color.Black.copy(alpha = 0.8f)
                            )
                        )
                    )
                    .padding(12.dp)
            ) {
                Text(
                    text = game.displayName,
                    style = MaterialTheme.typography.titleSmall,
                    color = Color.White,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis
                )
                Spacer(modifier = Modifier.height(4.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    Surface(
                        shape = RoundedCornerShape(4.dp),
                        color = when (game.type) {
                            com.x360mu.util.GameType.DISC -> MaterialTheme.colorScheme.primary.copy(alpha = 0.8f)
                            com.x360mu.util.GameType.XBLA -> MaterialTheme.colorScheme.tertiary.copy(alpha = 0.8f)
                            else -> MaterialTheme.colorScheme.secondary.copy(alpha = 0.8f)
                        }
                    ) {
                        Text(
                            text = when (game.type) {
                                com.x360mu.util.GameType.DISC -> "DISC"
                                com.x360mu.util.GameType.XBLA -> "XBLA"
                                else -> game.extension
                            },
                            style = MaterialTheme.typography.labelSmall,
                            color = Color.White,
                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp)
                        )
                    }

                    // Size badge
                    Surface(
                        shape = RoundedCornerShape(4.dp),
                        color = Color.White.copy(alpha = 0.2f)
                    ) {
                        Text(
                            text = RomScanner.formatFileSize(game.size),
                            style = MaterialTheme.typography.labelSmall,
                            color = Color.White,
                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp)
                        )
                    }
                }
            }
        }
    }
}
