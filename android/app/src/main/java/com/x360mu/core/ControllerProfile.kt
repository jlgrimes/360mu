package com.x360mu.core

import org.json.JSONArray
import org.json.JSONObject

/**
 * Position and size of a single on-screen button (normalized 0..1 coordinates)
 */
data class ButtonLayout(
    val x: Float,       // Center X (0..1)
    val y: Float,       // Center Y (0..1)
    val width: Float,   // Width (0..1)
    val height: Float,  // Height (0..1)
    val visible: Boolean = true
) {
    fun toJson(): JSONObject = JSONObject().apply {
        put("x", x.toDouble())
        put("y", y.toDouble())
        put("w", width.toDouble())
        put("h", height.toDouble())
        put("visible", visible)
    }

    companion object {
        fun fromJson(json: JSONObject): ButtonLayout = ButtonLayout(
            x = json.optDouble("x", 0.0).toFloat(),
            y = json.optDouble("y", 0.0).toFloat(),
            width = json.optDouble("w", 0.05).toFloat(),
            height = json.optDouble("h", 0.05).toFloat(),
            visible = json.optBoolean("visible", true)
        )
    }
}

/**
 * Button remap entry: maps a source input to a target Xbox 360 button
 */
data class ButtonRemap(
    val sourceType: SourceType,
    val sourceCode: Int,     // Android keyCode or button index
    val targetButton: Int    // Xbox 360 button index (NativeEmulator.Button.*)
) {
    enum class SourceType { KEYBOARD, GAMEPAD }

    fun toJson(): JSONObject = JSONObject().apply {
        put("type", sourceType.name)
        put("source", sourceCode)
        put("target", targetButton)
    }

    companion object {
        fun fromJson(json: JSONObject): ButtonRemap = ButtonRemap(
            sourceType = try { SourceType.valueOf(json.optString("type", "GAMEPAD")) }
                         catch (_: Exception) { SourceType.GAMEPAD },
            sourceCode = json.optInt("source", 0),
            targetButton = json.optInt("target", 0)
        )
    }
}

/**
 * Complete controller profile — button layout + remapping + settings
 */
data class ControllerProfile(
    val name: String,
    val buttons: Map<String, ButtonLayout> = DEFAULT_LAYOUT,
    val remaps: List<ButtonRemap> = emptyList(),
    val opacity: Float = 0.5f,
    val hapticEnabled: Boolean = true,
    val stickDeadZoneInner: Float = 0.24f,
    val stickDeadZoneOuter: Float = 0.95f,
    val triggerDeadZone: Float = 0.12f
) {
    fun toJson(): JSONObject = JSONObject().apply {
        put("name", name)
        put("opacity", opacity.toDouble())
        put("haptic", hapticEnabled)
        put("stick_dz_inner", stickDeadZoneInner.toDouble())
        put("stick_dz_outer", stickDeadZoneOuter.toDouble())
        put("trigger_dz", triggerDeadZone.toDouble())

        val buttonsObj = JSONObject()
        buttons.forEach { (key, layout) -> buttonsObj.put(key, layout.toJson()) }
        put("buttons", buttonsObj)

        val remapsArr = JSONArray()
        remaps.forEach { remapsArr.put(it.toJson()) }
        put("remaps", remapsArr)
    }

    fun toJsonString(): String = toJson().toString(2)

    companion object {
        // Button keys matching the control composables
        const val DPAD_UP = "dpad_up"
        const val DPAD_DOWN = "dpad_down"
        const val DPAD_LEFT = "dpad_left"
        const val DPAD_RIGHT = "dpad_right"
        const val BTN_A = "btn_a"
        const val BTN_B = "btn_b"
        const val BTN_X = "btn_x"
        const val BTN_Y = "btn_y"
        const val BTN_START = "btn_start"
        const val BTN_BACK = "btn_back"
        const val BTN_LB = "btn_lb"
        const val BTN_RB = "btn_rb"
        const val BTN_LT = "btn_lt"
        const val BTN_RT = "btn_rt"
        const val STICK_LEFT = "stick_left"
        const val STICK_RIGHT = "stick_right"

        /** Default button layout (normalized screen coordinates) */
        val DEFAULT_LAYOUT: Map<String, ButtonLayout> = mapOf(
            // D-Pad (bottom-left)
            DPAD_UP    to ButtonLayout(0.10f, 0.55f, 0.06f, 0.06f),
            DPAD_DOWN  to ButtonLayout(0.10f, 0.70f, 0.06f, 0.06f),
            DPAD_LEFT  to ButtonLayout(0.04f, 0.625f, 0.06f, 0.06f),
            DPAD_RIGHT to ButtonLayout(0.16f, 0.625f, 0.06f, 0.06f),
            // Face buttons (bottom-right)
            BTN_Y to ButtonLayout(0.90f, 0.55f, 0.07f, 0.07f),
            BTN_A to ButtonLayout(0.90f, 0.70f, 0.07f, 0.07f),
            BTN_X to ButtonLayout(0.84f, 0.625f, 0.07f, 0.07f),
            BTN_B to ButtonLayout(0.96f, 0.625f, 0.07f, 0.07f),
            // Center
            BTN_START to ButtonLayout(0.55f, 0.92f, 0.08f, 0.05f),
            BTN_BACK  to ButtonLayout(0.45f, 0.92f, 0.08f, 0.05f),
            // Bumpers
            BTN_LB to ButtonLayout(0.10f, 0.15f, 0.10f, 0.04f),
            BTN_RB to ButtonLayout(0.90f, 0.15f, 0.10f, 0.04f),
            // Triggers
            BTN_LT to ButtonLayout(0.10f, 0.08f, 0.10f, 0.05f),
            BTN_RT to ButtonLayout(0.90f, 0.08f, 0.10f, 0.05f),
            // Sticks
            STICK_LEFT  to ButtonLayout(0.20f, 0.85f, 0.15f, 0.15f),
            STICK_RIGHT to ButtonLayout(0.80f, 0.85f, 0.12f, 0.12f)
        )

        fun fromJson(json: JSONObject): ControllerProfile {
            val buttonsObj = json.optJSONObject("buttons")
            val buttons = if (buttonsObj != null) {
                val map = mutableMapOf<String, ButtonLayout>()
                for (key in buttonsObj.keys()) {
                    map[key] = ButtonLayout.fromJson(buttonsObj.getJSONObject(key))
                }
                // Merge with defaults for any missing keys
                DEFAULT_LAYOUT.forEach { (key, default) ->
                    if (key !in map) map[key] = default
                }
                map
            } else {
                DEFAULT_LAYOUT
            }

            val remapsArr = json.optJSONArray("remaps")
            val remaps = if (remapsArr != null) {
                (0 until remapsArr.length()).map { ButtonRemap.fromJson(remapsArr.getJSONObject(it)) }
            } else {
                emptyList()
            }

            return ControllerProfile(
                name = json.optString("name", "Default"),
                buttons = buttons,
                remaps = remaps,
                opacity = json.optDouble("opacity", 0.5).toFloat(),
                hapticEnabled = json.optBoolean("haptic", true),
                stickDeadZoneInner = json.optDouble("stick_dz_inner", 0.24).toFloat(),
                stickDeadZoneOuter = json.optDouble("stick_dz_outer", 0.95).toFloat(),
                triggerDeadZone = json.optDouble("trigger_dz", 0.12).toFloat()
            )
        }

        fun fromJsonString(json: String): ControllerProfile = fromJson(JSONObject(json))
    }
}
