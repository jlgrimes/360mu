package com.x360mu.ui

import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Scaffold
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
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
import java.io.File
import java.net.URLDecoder
import java.net.URLEncoder

sealed class Screen(val route: String) {
    object Library : Screen("library")
    object Settings : Screen("settings")
    object Game : Screen("game/{gamePath}") {
        fun createRoute(gamePath: String) = "game/${URLEncoder.encode(gamePath, "UTF-8")}"
    }
}

@Composable
fun App() {
    val navController = rememberNavController()
    val context = LocalContext.current
    
    // Create emulator instance
    val emulator = remember {
        NativeEmulator().apply {
            val dataDir = context.filesDir.absolutePath
            val cacheDir = context.cacheDir.absolutePath
            val saveDir = File(context.filesDir, "saves").apply { mkdirs() }.absolutePath
            
            initialize(
                dataPath = dataDir,
                cachePath = cacheDir,
                savePath = saveDir,
                enableJit = true,
                enableVulkan = true
            )
        }
    }
    
    DisposableEffect(Unit) {
        onDispose {
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
                    onGameSelected = { path ->
                        navController.navigate(Screen.Game.createRoute(path))
                    },
                    onSettingsClick = {
                        navController.navigate(Screen.Settings.route)
                    }
                )
            }
            
            composable(Screen.Settings.route) {
                SettingsScreen(
                    onBackClick = {
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

