/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Input Manager - XInput controller emulation with touch and physical controller support
 */

#pragma once

#include "x360mu/types.h"
#include <array>
#include <atomic>
#include <mutex>
#include <cmath>
#include <chrono>
#include <functional>

namespace x360mu {

// XInput button bit flags (matching Xbox 360 SDK)
namespace xinput {
    constexpr u16 GAMEPAD_DPAD_UP        = 0x0001;
    constexpr u16 GAMEPAD_DPAD_DOWN      = 0x0002;
    constexpr u16 GAMEPAD_DPAD_LEFT      = 0x0004;
    constexpr u16 GAMEPAD_DPAD_RIGHT     = 0x0008;
    constexpr u16 GAMEPAD_START          = 0x0010;
    constexpr u16 GAMEPAD_BACK           = 0x0020;
    constexpr u16 GAMEPAD_LEFT_THUMB     = 0x0040;
    constexpr u16 GAMEPAD_RIGHT_THUMB    = 0x0080;
    constexpr u16 GAMEPAD_LEFT_SHOULDER  = 0x0100;
    constexpr u16 GAMEPAD_RIGHT_SHOULDER = 0x0200;
    constexpr u16 GAMEPAD_GUIDE          = 0x0400;
    constexpr u16 GAMEPAD_A              = 0x1000;
    constexpr u16 GAMEPAD_B              = 0x2000;
    constexpr u16 GAMEPAD_X              = 0x4000;
    constexpr u16 GAMEPAD_Y              = 0x8000;

    // Trigger range
    constexpr u8 TRIGGER_MAX = 255;

    // Stick range
    constexpr s16 STICK_MIN = -32768;
    constexpr s16 STICK_MAX = 32767;

    // Deadzone defaults
    constexpr s16 LEFT_THUMB_DEADZONE  = 7849;
    constexpr s16 RIGHT_THUMB_DEADZONE = 8689;
    constexpr u8  TRIGGER_THRESHOLD    = 30;

    // Max controllers
    constexpr u32 MAX_CONTROLLERS = 4;

    // Device types
    constexpr u8 DEVTYPE_GAMEPAD = 0x01;
    constexpr u8 DEVSUBTYPE_GAMEPAD = 0x01;
}

/**
 * Android button indices (matching NativeEmulator.kt Button object)
 */
enum class AndroidButton : u32 {
    A = 0,
    B = 1,
    X = 2,
    Y = 3,
    DpadUp = 4,
    DpadDown = 5,
    DpadLeft = 6,
    DpadRight = 7,
    Start = 8,
    Back = 9,
    LeftBumper = 10,
    RightBumper = 11,
    LeftStick = 12,
    RightStick = 13,
    Guide = 14,
    Count = 15
};

/**
 * Maps Android button index to XInput button bit flag
 */
inline u16 android_button_to_xinput(u32 button) {
    switch (static_cast<AndroidButton>(button)) {
        case AndroidButton::A:           return xinput::GAMEPAD_A;
        case AndroidButton::B:           return xinput::GAMEPAD_B;
        case AndroidButton::X:           return xinput::GAMEPAD_X;
        case AndroidButton::Y:           return xinput::GAMEPAD_Y;
        case AndroidButton::DpadUp:      return xinput::GAMEPAD_DPAD_UP;
        case AndroidButton::DpadDown:    return xinput::GAMEPAD_DPAD_DOWN;
        case AndroidButton::DpadLeft:    return xinput::GAMEPAD_DPAD_LEFT;
        case AndroidButton::DpadRight:   return xinput::GAMEPAD_DPAD_RIGHT;
        case AndroidButton::Start:       return xinput::GAMEPAD_START;
        case AndroidButton::Back:        return xinput::GAMEPAD_BACK;
        case AndroidButton::LeftBumper:  return xinput::GAMEPAD_LEFT_SHOULDER;
        case AndroidButton::RightBumper: return xinput::GAMEPAD_RIGHT_SHOULDER;
        case AndroidButton::LeftStick:   return xinput::GAMEPAD_LEFT_THUMB;
        case AndroidButton::RightStick:  return xinput::GAMEPAD_RIGHT_THUMB;
        case AndroidButton::Guide:       return xinput::GAMEPAD_GUIDE;
        default: return 0;
    }
}

/**
 * XINPUT_GAMEPAD structure (matches Xbox 360 SDK layout)
 */
struct XInputGamepad {
    u16 buttons = 0;
    u8  left_trigger = 0;
    u8  right_trigger = 0;
    s16 thumb_lx = 0;
    s16 thumb_ly = 0;
    s16 thumb_rx = 0;
    s16 thumb_ry = 0;
};

/**
 * XINPUT_STATE structure
 */
struct XInputState {
    u32 packet_number = 0;
    XInputGamepad gamepad;
};

/**
 * XINPUT_VIBRATION structure
 */
struct XInputVibration {
    u16 left_motor_speed = 0;
    u16 right_motor_speed = 0;
};

/**
 * Touch control zone - rectangular hit area on screen
 */
struct TouchZone {
    f32 x, y;          // Center position (0..1 normalized)
    f32 width, height; // Size (0..1 normalized)
    u16 button;        // XInput button bit flag (0 for analog zones)
    bool is_stick;     // True if this zone is an analog stick
    bool is_trigger;   // True if this zone is a trigger
    u32 trigger_id;    // 0=left, 1=right (only if is_trigger)
    u32 stick_id;      // 0=left, 1=right (only if is_stick)

    bool contains(f32 px, f32 py) const {
        return px >= (x - width / 2) && px <= (x + width / 2) &&
               py >= (y - height / 2) && py <= (y + height / 2);
    }
};

/**
 * Tracks an active touch point
 */
struct TouchPoint {
    s32 id = -1;       // Android pointer ID (-1 = inactive)
    f32 start_x = 0;   // Touch start position
    f32 start_y = 0;
    f32 current_x = 0; // Current position
    f32 current_y = 0;
    s32 zone_index = -1; // Which TouchZone this touch is in
};

/**
 * Controller state for a single player
 */
struct ControllerState {
    XInputState state;
    XInputVibration vibration;
    bool connected = false;
    bool physical_controller = false; // True if a physical controller is mapped
};

/**
 * Dead zone configuration for analog sticks
 */
struct DeadZoneConfig {
    f32 inner = 0.15f;   // Inner dead zone (0..1) — below this, output is 0
    f32 outer = 0.95f;   // Outer dead zone (0..1) — above this, output is 1
    bool radial = true;  // True = radial dead zone, false = axial
};

/**
 * Input Manager
 *
 * Central input system that:
 * - Maintains XInput state for up to 4 controllers
 * - Maps Android button/trigger/stick events to XInput
 * - Handles touch-to-controller mapping with multi-touch
 * - Supports physical Bluetooth/USB controllers
 * - Manages vibration/rumble feedback
 * - Applies configurable dead zones to analog sticks
 */
class InputManager {
public:
    InputManager();
    ~InputManager() = default;

    // Controller connection
    void set_controller_connected(u32 player, bool connected);
    bool is_controller_connected(u32 player) const;

    // Button input (from Android Button indices)
    void set_button(u32 player, u32 android_button, bool pressed);

    // Trigger input (0.0 - 1.0 float from Android)
    void set_trigger(u32 player, u32 trigger_id, f32 value);

    // Stick input (-1.0 to 1.0 float from Android, dead zone applied)
    void set_stick(u32 player, u32 stick_id, f32 x, f32 y);

    // Raw XInput button (using XInput bit flags directly)
    void set_xinput_button(u32 player, u16 xinput_button, bool pressed);

    // Touch input handling (multi-touch aware)
    void on_touch_down(u32 player, s32 pointer_id, f32 x, f32 y, f32 screen_w, f32 screen_h);
    void on_touch_move(u32 player, s32 pointer_id, f32 x, f32 y, f32 screen_w, f32 screen_h);
    void on_touch_up(u32 player, s32 pointer_id);

    // Get current state
    const XInputState& get_state(u32 player) const;
    u32 get_packet_number(u32 player) const;

    // Vibration
    using VibrationCallback = std::function<void(u32 player, u16 left_motor, u16 right_motor)>;
    void set_vibration(u32 player, u16 left_motor, u16 right_motor);
    XInputVibration get_vibration(u32 player) const;
    void set_vibration_callback(VibrationCallback callback);

    // Sync state to XAM HLE (called per-frame)
    void sync_to_xam();

    // Touch control layout
    void setup_default_touch_layout();

    // Dead zone configuration
    void set_stick_dead_zone(u32 stick_id, f32 inner, f32 outer);
    void set_trigger_dead_zone(f32 threshold);
    const DeadZoneConfig& get_stick_dead_zone(u32 stick_id) const;
    f32 get_trigger_dead_zone() const { return trigger_dead_zone_; }

private:
    mutable std::mutex mutex_;
    std::array<ControllerState, xinput::MAX_CONTROLLERS> controllers_;

    // Dead zone settings
    std::array<DeadZoneConfig, 2> stick_dead_zones_; // [0]=left, [1]=right
    f32 trigger_dead_zone_ = 0.1f; // Trigger dead zone (0..1)

    // Vibration callback (called from set_vibration, throttled to 60Hz)
    VibrationCallback vibration_callback_;
    std::array<std::chrono::steady_clock::time_point, xinput::MAX_CONTROLLERS> last_vibration_time_;

    // Touch controls
    std::array<TouchZone, 20> touch_zones_;
    u32 num_touch_zones_ = 0;
    std::array<TouchPoint, 10> touch_points_; // Max 10 simultaneous touches

    void add_touch_zone(f32 x, f32 y, f32 w, f32 h, u16 button,
                        bool is_stick = false, bool is_trigger = false,
                        u32 stick_or_trigger_id = 0);

    s32 find_touch_zone(f32 nx, f32 ny) const;
    s32 find_touch_point(s32 pointer_id) const;
    s32 alloc_touch_point(s32 pointer_id) const;

    // Apply dead zone to a stick axis pair, returns adjusted values
    void apply_stick_dead_zone(u32 stick_id, f32& x, f32& y) const;

    // Apply dead zone to trigger value
    f32 apply_trigger_dead_zone(f32 value) const;
};

// Global input manager instance
InputManager& get_input_manager();

} // namespace x360mu
