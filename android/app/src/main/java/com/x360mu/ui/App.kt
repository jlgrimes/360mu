package com.x360mu.ui

import android.util.Log
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Scaffold
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import com.x360mu.core.NativeEmulator
import com.x360mu.ui.screens.GameScreen
import com.x360mu.ui.screens.LibraryScreen
import com.x360mu.ui.screens.LogScreen
import com.x360mu.ui.screens.SettingsScreen
import com.x360mu.ui.screens.TestRenderScreen
import java.io.File
import java.net.URLDecoder
import java.net.URLEncoder

private const val TAG = "360mu-App"

sealed class Screen(val route: String) {
    object Library : Screen("library")
    object Settings : Screen("settings")
    object TestRender : Screen("test_render")
    object Logs : Screen("logs")
    object Game : Screen("game/{gamePath}") {
        fun createRoute(gamePath: String) = "game/${URLEncoder.encode(gamePath, "UTF-8")}"
    }
}

@Composable
fun App(onEmulatorCreated: (NativeEmulator) -> Unit = {}) {
    Log.i(TAG, "App() composable starting")

    val navController = rememberNavController()
    val context = LocalContext.current

    val emulator = remember {
        Log.i(TAG, "Creating NativeEmulator instance...")
        try {
            NativeEmulator().apply {
                val dataDir = context.filesDir.absolutePath
                val cacheDir = context.cacheDir.absolutePath
                val saveDir = File(context.filesDir, "saves").apply { mkdirs() }.absolutePath

                val result = initialize(
                    dataPath = dataDir,
                    cachePath = cacheDir,
                    savePath = saveDir,
                    enableJit = true,
                    enableVulkan = true
                )
                Log.i(TAG, "Emulator initialize() returned: $result, state: $state")

                // Install crash handler
                val crashDir = File(context.filesDir, "crashes").apply { mkdirs() }.absolutePath
                installCrashHandler(crashDir)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to create/initialize emulator: ${e.message}", e)
            throw e
        }
    }

    val crashDir = remember {
        File(context.filesDir, "crashes").apply { mkdirs() }.absolutePath
    }

    // Expose emulator reference for lifecycle handling
    LaunchedEffect(emulator) {
        onEmulatorCreated(emulator)
    }

    DisposableEffect(Unit) {
        onDispose {
            Log.i(TAG, "DisposableEffect onDispose - closing emulator")
            emulator.close()
        }
    }

    Scaffold { innerPadding ->
        NavHost(
            navController = navController,
            startDestination = Screen.Library.route,
            modifier = Modifier.padding(innerPadding)
        ) {
            composable(Screen.Library.route) {
                LibraryScreen(
                    emulator = emulator,
                    onGameSelected = { path ->
                        Log.i(TAG, "Game selected: $path")
                        navController.navigate(Screen.Game.createRoute(path))
                    },
                    onSettingsClick = {
                        navController.navigate(Screen.Settings.route)
                    },
                    onTestRenderClick = {
                        navController.navigate(Screen.TestRender.route)
                    },
                    onLogsClick = {
                        navController.navigate(Screen.Logs.route)
                    }
                )
            }

            composable(Screen.Settings.route) {
                SettingsScreen(
                    onBackClick = { navController.popBackStack() }
                )
            }

            composable(Screen.Logs.route) {
                LogScreen(
                    emulator = emulator,
                    onBackClick = { navController.popBackStack() },
                    crashDir = crashDir
                )
            }

            composable(Screen.TestRender.route) {
                TestRenderScreen(
                    emulator = emulator,
                    onBack = { navController.popBackStack() }
                )
            }

            composable(
                route = Screen.Game.route,
                arguments = listOf(navArgument("gamePath") { type = NavType.StringType })
            ) { backStackEntry ->
                val encodedPath = backStackEntry.arguments?.getString("gamePath") ?: ""
                val gamePath = URLDecoder.decode(encodedPath, "UTF-8")

                GameScreen(
                    emulator = emulator,
                    gamePath = gamePath,
                    onBack = {
                        emulator.stop()
                        navController.popBackStack()
                    }
                )
            }
        }
    }
}
