package com.routeos.companion.ui.theme

import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val DarkColorScheme = darkColorScheme(
    primary          = RouteOSGreen,
    onPrimary        = Color.Black,
    primaryContainer = RouteOSGreenDark,
    secondary        = RouteOSLime,
    onSecondary      = Color.Black,
    background       = RouteOSDark,
    onBackground     = RouteOSOnSurface,
    surface          = RouteOSSurface,
    onSurface        = RouteOSOnSurface,
    surfaceVariant   = RouteOSCard,
    onSurfaceVariant = RouteOSOnSurface,
    error            = RouteOSOrange,
    onError          = Color.White,
)

@Composable
fun RouteOSTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = DarkColorScheme,
        typography  = Typography(),
        content     = content,
    )
}
