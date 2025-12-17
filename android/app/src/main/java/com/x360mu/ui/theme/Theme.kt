package com.x360mu.ui.theme

import android.app.Activity
import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.core.view.WindowCompat

// Xbox 360 inspired colors
private val XboxGreen = Color(0xFF107C10)
private val XboxGreenLight = Color(0xFF0E6A0E)
private val XboxGreenDark = Color(0xFF054B05)
private val XboxBlack = Color(0xFF0D0D0D)
private val XboxDarkGray = Color(0xFF1F1F1F)
private val XboxMediumGray = Color(0xFF2D2D2D)
private val XboxLightGray = Color(0xFFE5E5E5)

private val DarkColorScheme = darkColorScheme(
    primary = XboxGreen,
    onPrimary = Color.White,
    primaryContainer = XboxGreenDark,
    onPrimaryContainer = Color.White,
    secondary = Color(0xFF5C6BC0),
    onSecondary = Color.White,
    tertiary = Color(0xFF7E57C2),
    onTertiary = Color.White,
    background = XboxBlack,
    onBackground = Color.White,
    surface = XboxDarkGray,
    onSurface = Color.White,
    surfaceVariant = XboxMediumGray,
    onSurfaceVariant = Color(0xFFCAC4D0),
    error = Color(0xFFCF6679),
    onError = Color.Black,
)

private val LightColorScheme = lightColorScheme(
    primary = XboxGreen,
    onPrimary = Color.White,
    primaryContainer = Color(0xFFB8F5B8),
    onPrimaryContainer = XboxGreenDark,
    secondary = Color(0xFF5C6BC0),
    onSecondary = Color.White,
    tertiary = Color(0xFF7E57C2),
    onTertiary = Color.White,
    background = Color(0xFFFFFBFE),
    onBackground = Color(0xFF1C1B1F),
    surface = Color(0xFFFFFBFE),
    onSurface = Color(0xFF1C1B1F),
    surfaceVariant = Color(0xFFE7E0EC),
    onSurfaceVariant = Color(0xFF49454F),
)

@Composable
fun X360muTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    dynamicColor: Boolean = false,
    content: @Composable () -> Unit
) {
    val colorScheme = when {
        dynamicColor && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            val context = LocalContext.current
            if (darkTheme) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        }
        darkTheme -> DarkColorScheme
        else -> LightColorScheme
    }
    
    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as Activity).window
            window.statusBarColor = colorScheme.background.toArgb()
            WindowCompat.getInsetsController(window, view).isAppearanceLightStatusBars = !darkTheme
        }
    }

    MaterialTheme(
        colorScheme = colorScheme,
        typography = Typography,
        content = content
    )
}

