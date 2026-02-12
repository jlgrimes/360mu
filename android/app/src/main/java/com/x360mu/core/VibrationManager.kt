package com.x360mu.core

import android.content.Context
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import android.view.InputDevice

private const val TAG = "360mu-VibrationManager"

/**
 * Manages haptic feedback for emulated Xbox 360 controller vibration.
 *
 * Receives vibration events from native XInputSetState HLE via reverse JNI callback
 * and maps Xbox 360 motor speeds (0-65535) to Android vibration amplitudes (0-255).
 */
class VibrationManager(private val context: Context) {

    /**
     * Interface for native vibration callback (called from C++ via JNI)
     */
    interface VibrationListener {
        fun onVibration(player: Int, leftMotor: Int, rightMotor: Int)
    }

    private val vibrator: Vibrator? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        val vm = context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager
        vm?.defaultVibrator
    } else {
        @Suppress("DEPRECATION")
        context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
    }

    private val hasVibrator: Boolean = vibrator?.hasVibrator() == true
    private val hasAmplitudeControl: Boolean =
        hasVibrator && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O &&
                vibrator?.hasAmplitudeControl() == true

    /**
     * The listener object passed to native code for reverse JNI callback
     */
    val listener: VibrationListener = object : VibrationListener {
        override fun onVibration(player: Int, leftMotor: Int, rightMotor: Int) {
            handleVibration(player, leftMotor, rightMotor)
        }
    }

    init {
        Log.i(TAG, "VibrationManager init: hasVibrator=$hasVibrator, hasAmplitudeControl=$hasAmplitudeControl")
    }

    private fun handleVibration(player: Int, leftMotor: Int, rightMotor: Int) {
        // Try physical controller vibration first
        if (vibratePhysicalController(player, leftMotor, rightMotor)) return

        // Fall back to device vibrator for touch-only players
        if (!hasVibrator) return

        // Combine both motors: take the stronger of the two
        // Xbox 360: left = low-freq heavy, right = high-freq light
        val combined = maxOf(leftMotor, rightMotor)
        val amplitude = (combined * 255 / 65535).coerceIn(0, 255)

        if (amplitude == 0) {
            vibrator?.cancel()
            return
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val effect = if (hasAmplitudeControl) {
                VibrationEffect.createOneShot(50, amplitude)
            } else {
                VibrationEffect.createOneShot(50, VibrationEffect.DEFAULT_AMPLITUDE)
            }
            vibrator?.vibrate(effect)
        } else {
            @Suppress("DEPRECATION")
            vibrator?.vibrate(50)
        }
    }

    /**
     * Attempt to vibrate a physical Bluetooth/USB controller.
     * Returns true if a physical controller was found and vibrated.
     */
    private fun vibratePhysicalController(player: Int, leftMotor: Int, rightMotor: Int): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return false

        val deviceIds = InputDevice.getDeviceIds()
        var gamepadIndex = 0

        for (id in deviceIds) {
            val device = InputDevice.getDevice(id) ?: continue
            val sources = device.sources

            if (sources and InputDevice.SOURCE_GAMEPAD != 0 ||
                sources and InputDevice.SOURCE_JOYSTICK != 0) {

                if (gamepadIndex == player) {
                    val vm = device.vibratorManager
                    val vibratorIds = vm.vibratorIds

                    if (vibratorIds.isEmpty()) return false

                    val amplitude = (maxOf(leftMotor, rightMotor) * 255 / 65535).coerceIn(0, 255)
                    if (amplitude == 0) {
                        for (vid in vibratorIds) {
                            vm.getVibrator(vid).cancel()
                        }
                    } else {
                        val effect = VibrationEffect.createOneShot(50, amplitude)
                        for (vid in vibratorIds) {
                            vm.getVibrator(vid).vibrate(effect)
                        }
                    }
                    return true
                }
                gamepadIndex++
            }
        }

        return false
    }

    fun release() {
        vibrator?.cancel()
        Log.i(TAG, "VibrationManager released")
    }
}
