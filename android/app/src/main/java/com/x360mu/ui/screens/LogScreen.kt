package com.x360mu.ui.screens

import android.content.Intent
import android.util.Log
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.x360mu.core.NativeEmulator
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

private const val TAG = "360mu-LogScreen"

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LogScreen(
    emulator: NativeEmulator,
    onBackClick: () -> Unit,
    crashDir: String
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    // Tab state: 0 = Live Logs, 1 = Crash Logs
    var selectedTab by remember { mutableStateOf(0) }

    // Log filter state
    var severityFilter by remember { mutableStateOf(NativeEmulator.LogSeverity.Debug) }
    var componentFilter by remember { mutableStateOf(-1) } // -1 = all
    var searchQuery by remember { mutableStateOf("") }
    var autoScroll by remember { mutableStateOf(true) }

    // Log entries
    var logEntries by remember { mutableStateOf<List<NativeEmulator.LogEntry>>(emptyList()) }
    var isRefreshing by remember { mutableStateOf(false) }

    // Crash logs
    var crashLogs by remember { mutableStateOf<List<String>>(emptyList()) }
    var selectedCrashLog by remember { mutableStateOf<String?>(null) }
    var crashLogContent by remember { mutableStateOf("") }

    // Refresh logs
    fun refreshLogs() {
        scope.launch {
            isRefreshing = true
            val entries = withContext(Dispatchers.IO) {
                emulator.getLogs(severityFilter, componentFilter)
            }
            logEntries = if (searchQuery.isBlank()) entries
                         else entries.filter { it.message.contains(searchQuery, ignoreCase = true) }
            isRefreshing = false
        }
    }

    // Auto-refresh logs every 2 seconds
    LaunchedEffect(selectedTab, severityFilter, componentFilter) {
        if (selectedTab == 0) {
            while (true) {
                refreshLogs()
                delay(2000)
            }
        }
    }

    // Load crash logs on tab switch
    LaunchedEffect(selectedTab) {
        if (selectedTab == 1) {
            crashLogs = withContext(Dispatchers.IO) {
                emulator.listCrashLogs(crashDir)
            }
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Logs & Crashes") },
                navigationIcon = {
                    IconButton(onClick = onBackClick) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = {
                    if (selectedTab == 0) {
                        // Export logs
                        IconButton(onClick = {
                            scope.launch {
                                val text = withContext(Dispatchers.IO) { emulator.exportLogs() }
                                val sendIntent = Intent().apply {
                                    action = Intent.ACTION_SEND
                                    putExtra(Intent.EXTRA_TEXT, text)
                                    putExtra(Intent.EXTRA_SUBJECT, "360mu Log Export")
                                    type = "text/plain"
                                }
                                context.startActivity(Intent.createChooser(sendIntent, "Export Logs"))
                            }
                        }) {
                            Icon(Icons.Default.Share, contentDescription = "Export")
                        }

                        // Clear logs
                        IconButton(onClick = {
                            emulator.clearLogs()
                            logEntries = emptyList()
                        }) {
                            Icon(Icons.Default.Delete, contentDescription = "Clear")
                        }

                        // Toggle auto-scroll
                        IconButton(onClick = { autoScroll = !autoScroll }) {
                            Icon(
                                if (autoScroll) Icons.Default.VerticalAlignBottom
                                else Icons.Default.VerticalAlignCenter,
                                contentDescription = "Auto-scroll"
                            )
                        }
                    }
                }
            )
        }
    ) { paddingValues ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(paddingValues)
        ) {
            // Tab row
            TabRow(selectedTabIndex = selectedTab) {
                Tab(
                    selected = selectedTab == 0,
                    onClick = { selectedTab = 0 },
                    text = { Text("Live Logs") },
                    icon = { Icon(Icons.Default.List, contentDescription = null) }
                )
                Tab(
                    selected = selectedTab == 1,
                    onClick = { selectedTab = 1 },
                    text = { Text("Crash Logs (${crashLogs.size})") },
                    icon = { Icon(Icons.Default.Warning, contentDescription = null) }
                )
            }

            when (selectedTab) {
                0 -> LiveLogView(
                    entries = logEntries,
                    severityFilter = severityFilter,
                    componentFilter = componentFilter,
                    searchQuery = searchQuery,
                    autoScroll = autoScroll,
                    isRefreshing = isRefreshing,
                    onSeverityChange = { severityFilter = it; refreshLogs() },
                    onComponentChange = { componentFilter = it; refreshLogs() },
                    onSearchChange = { searchQuery = it; refreshLogs() }
                )
                1 -> CrashLogView(
                    crashLogs = crashLogs,
                    selectedLog = selectedCrashLog,
                    logContent = crashLogContent,
                    onSelectLog = { path ->
                        selectedCrashLog = path
                        scope.launch {
                            crashLogContent = withContext(Dispatchers.IO) {
                                emulator.readCrashLog(path)
                            }
                        }
                    },
                    onBack = { selectedCrashLog = null },
                    onShare = { content ->
                        val sendIntent = Intent().apply {
                            action = Intent.ACTION_SEND
                            putExtra(Intent.EXTRA_TEXT, content)
                            putExtra(Intent.EXTRA_SUBJECT, "360mu Crash Report")
                            type = "text/plain"
                        }
                        context.startActivity(Intent.createChooser(sendIntent, "Share Crash Report"))
                    }
                )
            }
        }
    }
}

@Composable
private fun LiveLogView(
    entries: List<NativeEmulator.LogEntry>,
    severityFilter: NativeEmulator.LogSeverity,
    componentFilter: Int,
    searchQuery: String,
    autoScroll: Boolean,
    isRefreshing: Boolean,
    onSeverityChange: (NativeEmulator.LogSeverity) -> Unit,
    onComponentChange: (Int) -> Unit,
    onSearchChange: (String) -> Unit
) {
    val listState = rememberLazyListState()

    // Auto-scroll to bottom
    LaunchedEffect(entries.size, autoScroll) {
        if (autoScroll && entries.isNotEmpty()) {
            listState.animateScrollToItem(entries.size - 1)
        }
    }

    Column(modifier = Modifier.fillMaxSize()) {
        // Filter bar
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 8.dp, vertical = 4.dp)
                .horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(4.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Severity filter chips
            NativeEmulator.LogSeverity.entries.forEach { sev ->
                FilterChip(
                    selected = severityFilter == sev,
                    onClick = { onSeverityChange(sev) },
                    label = { Text(sev.name, fontSize = 11.sp) },
                    colors = FilterChipDefaults.filterChipColors(
                        selectedContainerColor = severityColor(sev).copy(alpha = 0.3f)
                    ),
                    modifier = Modifier.height(28.dp)
                )
            }

            VerticalDivider(modifier = Modifier.height(24.dp))

            // Component filter
            FilterChip(
                selected = componentFilter == -1,
                onClick = { onComponentChange(-1) },
                label = { Text("All", fontSize = 11.sp) },
                modifier = Modifier.height(28.dp)
            )
            NativeEmulator.LogComponent.entries.forEach { comp ->
                FilterChip(
                    selected = componentFilter == comp.value,
                    onClick = { onComponentChange(comp.value) },
                    label = { Text(comp.displayName, fontSize = 11.sp) },
                    modifier = Modifier.height(28.dp)
                )
            }
        }

        // Search bar
        OutlinedTextField(
            value = searchQuery,
            onValueChange = onSearchChange,
            placeholder = { Text("Search logs...", fontSize = 12.sp) },
            leadingIcon = { Icon(Icons.Default.Search, contentDescription = null, modifier = Modifier.size(18.dp)) },
            singleLine = true,
            textStyle = LocalTextStyle.current.copy(fontSize = 12.sp),
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 8.dp, vertical = 2.dp)
                .height(42.dp)
        )

        // Entry count
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 2.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                "${entries.size} entries",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            if (isRefreshing) {
                CircularProgressIndicator(modifier = Modifier.size(12.dp), strokeWidth = 1.5.dp)
            }
        }

        // Log list
        LazyColumn(
            state = listState,
            modifier = Modifier
                .fillMaxSize()
                .background(Color(0xFF1A1A2E))
        ) {
            items(entries) { entry ->
                LogEntryRow(entry)
            }
        }
    }
}

@Composable
private fun LogEntryRow(entry: NativeEmulator.LogEntry) {
    val sevColor = severityColor(entry.severity)

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 4.dp, vertical = 1.dp),
        verticalAlignment = Alignment.Top
    ) {
        // Timestamp
        Text(
            text = formatTimestamp(entry.timestampMs),
            fontSize = 10.sp,
            fontFamily = FontFamily.Monospace,
            color = Color(0xFF888888),
            modifier = Modifier.width(60.dp)
        )

        // Severity indicator
        Text(
            text = entry.severity.name.first().toString(),
            fontSize = 10.sp,
            fontFamily = FontFamily.Monospace,
            fontWeight = FontWeight.Bold,
            color = sevColor,
            modifier = Modifier.width(14.dp)
        )

        // Component tag
        Text(
            text = entry.component.displayName.padEnd(5),
            fontSize = 10.sp,
            fontFamily = FontFamily.Monospace,
            color = Color(0xFF6699CC),
            modifier = Modifier.width(42.dp)
        )

        // Message
        Text(
            text = entry.message,
            fontSize = 10.sp,
            fontFamily = FontFamily.Monospace,
            color = Color(0xFFCCCCCC),
            maxLines = 3,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f)
        )
    }
}

@Composable
private fun CrashLogView(
    crashLogs: List<String>,
    selectedLog: String?,
    logContent: String,
    onSelectLog: (String) -> Unit,
    onBack: () -> Unit,
    onShare: (String) -> Unit
) {
    if (selectedLog != null) {
        // Show crash log content
        Column(modifier = Modifier.fillMaxSize()) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(8.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                IconButton(onClick = onBack) {
                    Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                }
                Text(
                    File(selectedLog).name,
                    style = MaterialTheme.typography.titleSmall,
                    modifier = Modifier.weight(1f),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
                IconButton(onClick = { onShare(logContent) }) {
                    Icon(Icons.Default.Share, contentDescription = "Share")
                }
            }

            // Crash content
            LazyColumn(
                modifier = Modifier
                    .fillMaxSize()
                    .background(Color(0xFF1A1A2E))
                    .padding(8.dp)
            ) {
                item {
                    Text(
                        text = logContent.ifEmpty { "Loading..." },
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace,
                        color = Color(0xFFCCCCCC),
                        lineHeight = 16.sp
                    )
                }
            }
        }
    } else {
        // Show crash log list
        if (crashLogs.isEmpty()) {
            Box(
                modifier = Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Icon(
                        Icons.Default.CheckCircle,
                        contentDescription = null,
                        modifier = Modifier.size(48.dp),
                        tint = MaterialTheme.colorScheme.primary
                    )
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        "No Crash Logs",
                        style = MaterialTheme.typography.titleMedium
                    )
                    Text(
                        "No crashes have been recorded.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        } else {
            LazyColumn(modifier = Modifier.fillMaxSize()) {
                items(crashLogs) { path ->
                    val filename = File(path).name
                    Card(
                        onClick = { onSelectLog(path) },
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(horizontal = 12.dp, vertical = 4.dp)
                    ) {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(12.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Icon(
                                Icons.Default.Warning,
                                contentDescription = null,
                                tint = Color(0xFFF44336),
                                modifier = Modifier.size(24.dp)
                            )
                            Spacer(modifier = Modifier.width(12.dp))
                            Column(modifier = Modifier.weight(1f)) {
                                Text(
                                    filename,
                                    style = MaterialTheme.typography.bodyMedium,
                                    fontFamily = FontFamily.Monospace,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis
                                )
                                // Extract signal from filename
                                val signal = when {
                                    filename.contains("_11.") -> "SIGSEGV"
                                    filename.contains("_7.") -> "SIGBUS"
                                    filename.contains("_6.") -> "SIGABRT"
                                    filename.contains("_8.") -> "SIGFPE"
                                    else -> "Unknown"
                                }
                                Text(
                                    signal,
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.error
                                )
                            }
                            Icon(
                                Icons.Default.ChevronRight,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }
                }
            }
        }
    }
}

private fun severityColor(severity: NativeEmulator.LogSeverity): Color {
    return when (severity) {
        NativeEmulator.LogSeverity.Debug -> Color(0xFF888888)
        NativeEmulator.LogSeverity.Info -> Color(0xFF4FC3F7)
        NativeEmulator.LogSeverity.Warning -> Color(0xFFFFB74D)
        NativeEmulator.LogSeverity.Error -> Color(0xFFEF5350)
    }
}

private fun formatTimestamp(ms: Long): String {
    val secs = ms / 1000
    val millis = ms % 1000
    return "%d.%03d".format(secs, millis)
}
