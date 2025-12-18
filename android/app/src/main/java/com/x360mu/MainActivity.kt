package com.x360mu

import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.x360mu.ui.App
import com.x360mu.ui.theme.X360muTheme

private const val TAG = "360mu-MainActivity"

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i(TAG, "=== onCreate started ===")
        
        try {
            enableEdgeToEdge()
            Log.i(TAG, "enableEdgeToEdge called")
        } catch (e: Exception) {
            Log.e(TAG, "enableEdgeToEdge failed: ${e.message}", e)
        }
        
        Log.i(TAG, "Setting content...")
        
        setContent {
            LaunchedEffect(Unit) {
                Log.i(TAG, "LaunchedEffect: Compose content is being composed")
            }
            
            Log.i(TAG, "setContent lambda executing")
            
            X360muTheme {
                Log.i(TAG, "X360muTheme applied")
                
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    Log.i(TAG, "Surface composable executing")
                    
                    // Debug overlay to show something is rendering
                    Box(modifier = Modifier.fillMaxSize()) {
                        App()
                        
                        // Debug indicator - small colored box in corner
                        DebugIndicator(
                            modifier = Modifier
                                .align(Alignment.TopStart)
                                .padding(top = 48.dp, start = 8.dp)
                        )
                    }
                }
            }
        }
        
        Log.i(TAG, "=== onCreate completed ===")
    }
    
    override fun onStart() {
        super.onStart()
        Log.i(TAG, "onStart")
    }
    
    override fun onResume() {
        super.onResume()
        Log.i(TAG, "onResume")
    }
    
    override fun onPause() {
        super.onPause()
        Log.i(TAG, "onPause")
    }
    
    override fun onStop() {
        super.onStop()
        Log.i(TAG, "onStop")
    }
    
    override fun onDestroy() {
        super.onDestroy()
        Log.i(TAG, "onDestroy")
    }
}

@Composable
private fun DebugIndicator(modifier: Modifier = Modifier) {
    LaunchedEffect(Unit) {
        Log.i(TAG, "DebugIndicator: Compose is rendering!")
    }
    
    Column(
        modifier = modifier
            .background(Color.Red.copy(alpha = 0.8f))
            .padding(4.dp)
    ) {
        Text(
            text = "DEBUG",
            color = Color.White,
            style = MaterialTheme.typography.labelSmall
        )
        Text(
            text = "UI OK",
            color = Color.White,
            style = MaterialTheme.typography.labelSmall
        )
    }
}

