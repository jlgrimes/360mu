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
    object Game : Screen("game/{gamePath}") {
        fun createRoute(gamePath: String) = "game/${URLEncoder.encode(gamePath, "UTF-8")}"
    }
}

@Composable
fun App() {
    Log.i(TAG, "App() composable starting")
    
    val navController = rememberNavController()
    val context = LocalContext.current
    
    LaunchedEffect(Unit) {
        Log.i(TAG, "App LaunchedEffect - composition complete")
    }
    
    // Create emulator instance
    val emulator = remember {
        Log.i(TAG, "Creating NativeEmulator instance...")
        try {
            NativeEmulator().apply {
                val dataDir = context.filesDir.absolutePath
                val cacheDir = context.cacheDir.absolutePath
                val saveDir = File(context.filesDir, "saves").apply { mkdirs() }.absolutePath
                
                Log.i(TAG, "Emulator paths:")
                Log.i(TAG, "  dataDir: $dataDir")
                Log.i(TAG, "  cacheDir: $cacheDir")
                Log.i(TAG, "  saveDir: $saveDir")
                
                val result = initialize(
                    dataPath = dataDir,
                    cachePath = cacheDir,
                    savePath = saveDir,
                    enableJit = true,
                    enableVulkan = true
                )
                
                Log.i(TAG, "Emulator initialize() returned: $result")
                Log.i(TAG, "Emulator state: $state")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to create/initialize emulator: ${e.message}", e)
            throw e
        }.also {
            Log.i(TAG, "NativeEmulator instance created successfully")
        }
    }
    
    DisposableEffect(Unit) {
        Log.i(TAG, "DisposableEffect setup")
        onDispose {
            Log.i(TAG, "DisposableEffect onDispose - closing emulator")
            emulator.close()
        }
    }
    
    Log.i(TAG, "Setting up Scaffold...")
    
    Scaffold { innerPadding ->
        Log.i(TAG, "Scaffold content - innerPadding: $innerPadding")
        
        NavHost(
            navController = navController,
            startDestination = Screen.Library.route,
            modifier = Modifier.padding(innerPadding)
        ) {
            composable(Screen.Library.route) {
                Log.i(TAG, "Navigated to Library screen")
                LibraryScreen(
                    emulator = emulator,
                    onGameSelected = { path ->
                        Log.i(TAG, "Game selected: $path")
                        navController.navigate(Screen.Game.createRoute(path))
                    },
                    onSettingsClick = {
                        Log.i(TAG, "Settings clicked")
                        navController.navigate(Screen.Settings.route)
                    },
                    onTestRenderClick = {
                        Log.i(TAG, "Test Render clicked")
                        navController.navigate(Screen.TestRender.route)
                    }
                )
            }
            
            composable(Screen.Settings.route) {
                Log.i(TAG, "Navigated to Settings screen")
                SettingsScreen(
                    onBackClick = {
                        Log.i(TAG, "Settings back clicked")
                        navController.popBackStack()
                    }
                )
            }

            composable(Screen.TestRender.route) {
                Log.i(TAG, "Navigated to TestRender screen")
                TestRenderScreen(
                    emulator = emulator,
                    onBack = {
                        Log.i(TAG, "TestRender back clicked")
                        navController.popBackStack()
                    }
                )
            }

            composable(
                route = Screen.Game.route,
                arguments = listOf(
                    navArgument("gamePath") { type = NavType.StringType }
                )
            ) { backStackEntry ->
                val encodedPath = backStackEntry.arguments?.getString("gamePath") ?: ""
                val gamePath = URLDecoder.decode(encodedPath, "UTF-8")
                
                Log.i(TAG, "Navigated to Game screen, path: $gamePath")
                
                GameScreen(
                    emulator = emulator,
                    gamePath = gamePath,
                    onBack = {
                        Log.i(TAG, "Game back clicked")
                        emulator.stop()
                        navController.popBackStack()
                    }
                )
            }
        }
    }
    
    Log.i(TAG, "App() composable completed")
}

