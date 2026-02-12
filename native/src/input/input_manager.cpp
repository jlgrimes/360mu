/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Input Manager implementation
 */

#include "input_manager.h"
#include "../kernel/kernel.h"

#include <algorithm>
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-input"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[Input] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#define LOGW(...) printf("[Input WARN] " __VA_ARGS__); printf("\n")
#endif

namespace x360mu {

static InputManager g_input_manager;

InputManager& get_input_manager() {
    return g_input_manager;
}

InputManager::InputManager() {
    // Player 1 connected by default (touch controls)
    controllers_[0].connected = true;

    // Default dead zones (matching XInput SDK recommendations)
    stick_dead_zones_[0] = { .inner = 0.24f, .outer = 0.95f, .radial = true }; // Left
    stick_dead_zones_[1] = { .inner = 0.27f, .outer = 0.95f, .radial = true }; // Right
    trigger_dead_zone_ = 0.12f;

    // Clear touch points
    for (auto& tp : touch_points_) {
        tp.id = -1;
    }

    setup_default_touch_layout();
}

void InputManager::set_controller_connected(u32 player, bool connected) {
    if (player >= xinput::MAX_CONTROLLERS) return;
    std::lock_guard<std::mutex> lock(mutex_);
    controllers_[player].connected = connected;
    LOGI("Controller %u %s", player, connected ? "connected" : "disconnected");
}

bool InputManager::is_controller_connected(u32 player) const {
    if (player >= xinput::MAX_CONTROLLERS) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return controllers_[player].connected;
}

void InputManager::set_button(u32 player, u32 android_button, bool pressed) {
    if (player >= xinput::MAX_CONTROLLERS) return;

    u16 xinput_flag = android_button_to_xinput(android_button);
    if (xinput_flag == 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = controllers_[player].state;

    if (pressed) {
        state.gamepad.buttons |= xinput_flag;
    } else {
        state.gamepad.buttons &= ~xinput_flag;
    }
    state.packet_number++;
}

void InputManager::set_trigger(u32 player, u32 trigger_id, f32 value) {
    if (player >= xinput::MAX_CONTROLLERS) return;

    // Clamp 0..1 and apply dead zone
    value = std::clamp(value, 0.0f, 1.0f);
    value = apply_trigger_dead_zone(value);
    u8 raw = static_cast<u8>(value * 255.0f);

    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = controllers_[player].state;

    if (trigger_id == 0) {
        state.gamepad.left_trigger = raw;
    } else {
        state.gamepad.right_trigger = raw;
    }
    state.packet_number++;
}

void InputManager::set_stick(u32 player, u32 stick_id, f32 x, f32 y) {
    if (player >= xinput::MAX_CONTROLLERS) return;

    // Clamp -1..1
    x = std::clamp(x, -1.0f, 1.0f);
    y = std::clamp(y, -1.0f, 1.0f);

    // Apply dead zone
    apply_stick_dead_zone(stick_id <= 1 ? stick_id : 0, x, y);

    s16 sx = static_cast<s16>(x * 32767.0f);
    s16 sy = static_cast<s16>(y * 32767.0f);

    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = controllers_[player].state;

    if (stick_id == 0) {
        state.gamepad.thumb_lx = sx;
        state.gamepad.thumb_ly = sy;
    } else {
        state.gamepad.thumb_rx = sx;
        state.gamepad.thumb_ry = sy;
    }
    state.packet_number++;
}

void InputManager::set_xinput_button(u32 player, u16 xinput_button, bool pressed) {
    if (player >= xinput::MAX_CONTROLLERS) return;

    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = controllers_[player].state;

    if (pressed) {
        state.gamepad.buttons |= xinput_button;
    } else {
        state.gamepad.buttons &= ~xinput_button;
    }
    state.packet_number++;
}

const XInputState& InputManager::get_state(u32 player) const {
    static const XInputState empty{};
    if (player >= xinput::MAX_CONTROLLERS) return empty;
    // Caller should hold lock or call from single thread
    return controllers_[player].state;
}

u32 InputManager::get_packet_number(u32 player) const {
    if (player >= xinput::MAX_CONTROLLERS) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    return controllers_[player].state.packet_number;
}

void InputManager::set_vibration(u32 player, u16 left_motor, u16 right_motor) {
    if (player >= xinput::MAX_CONTROLLERS) return;

    VibrationCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& vib = controllers_[player].vibration;

        // Skip if unchanged
        if (vib.left_motor_speed == left_motor && vib.right_motor_speed == right_motor) return;

        vib.left_motor_speed = left_motor;
        vib.right_motor_speed = right_motor;

        // Throttle callback to ~60Hz (16.6ms)
        if (vibration_callback_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_vibration_time_[player]);
            if (elapsed.count() >= 16) {
                last_vibration_time_[player] = now;
                cb = vibration_callback_;
            }
        }
    }

    // Invoke callback outside lock to avoid deadlock
    if (cb) {
        cb(player, left_motor, right_motor);
    }
}

XInputVibration InputManager::get_vibration(u32 player) const {
    if (player >= xinput::MAX_CONTROLLERS) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    return controllers_[player].vibration;
}

void InputManager::set_vibration_callback(VibrationCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    vibration_callback_ = std::move(callback);
    LOGI("Vibration callback %s", vibration_callback_ ? "registered" : "cleared");
}

void InputManager::sync_to_xam() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (u32 i = 0; i < xinput::MAX_CONTROLLERS; i++) {
        if (!controllers_[i].connected) continue;

        const auto& gp = controllers_[i].state.gamepad;
        xam_set_input_state(
            i,
            gp.buttons,
            gp.left_trigger,
            gp.right_trigger,
            gp.thumb_lx,
            gp.thumb_ly,
            gp.thumb_rx,
            gp.thumb_ry
        );
    }
}

// ============================================================================
// Touch controls
// ============================================================================

void InputManager::add_touch_zone(f32 x, f32 y, f32 w, f32 h, u16 button,
                                   bool is_stick, bool is_trigger, u32 stick_or_trigger_id) {
    if (num_touch_zones_ >= touch_zones_.size()) return;
    touch_zones_[num_touch_zones_++] = {
        .x = x, .y = y,
        .width = w, .height = h,
        .button = button,
        .is_stick = is_stick,
        .is_trigger = is_trigger,
        .trigger_id = is_trigger ? stick_or_trigger_id : 0u,
        .stick_id = is_stick ? stick_or_trigger_id : 0u,
    };
}

void InputManager::setup_default_touch_layout() {
    num_touch_zones_ = 0;

    // ---- Left side: DPad ----
    // DPad Up
    add_touch_zone(0.10f, 0.55f, 0.07f, 0.08f, xinput::GAMEPAD_DPAD_UP);
    // DPad Down
    add_touch_zone(0.10f, 0.75f, 0.07f, 0.08f, xinput::GAMEPAD_DPAD_DOWN);
    // DPad Left
    add_touch_zone(0.05f, 0.65f, 0.07f, 0.08f, xinput::GAMEPAD_DPAD_LEFT);
    // DPad Right
    add_touch_zone(0.15f, 0.65f, 0.07f, 0.08f, xinput::GAMEPAD_DPAD_RIGHT);

    // ---- Right side: Face buttons ----
    // Y (top)
    add_touch_zone(0.90f, 0.55f, 0.07f, 0.08f, xinput::GAMEPAD_Y);
    // A (bottom)
    add_touch_zone(0.90f, 0.75f, 0.07f, 0.08f, xinput::GAMEPAD_A);
    // X (left)
    add_touch_zone(0.85f, 0.65f, 0.07f, 0.08f, xinput::GAMEPAD_X);
    // B (right)
    add_touch_zone(0.95f, 0.65f, 0.07f, 0.08f, xinput::GAMEPAD_B);

    // ---- Center: Start/Back ----
    add_touch_zone(0.55f, 0.92f, 0.08f, 0.06f, xinput::GAMEPAD_START);
    add_touch_zone(0.45f, 0.92f, 0.08f, 0.06f, xinput::GAMEPAD_BACK);

    // ---- Shoulders/Bumpers ----
    add_touch_zone(0.10f, 0.08f, 0.12f, 0.06f, xinput::GAMEPAD_LEFT_SHOULDER);
    add_touch_zone(0.90f, 0.08f, 0.12f, 0.06f, xinput::GAMEPAD_RIGHT_SHOULDER);

    // ---- Triggers ----
    add_touch_zone(0.10f, 0.02f, 0.12f, 0.05f, 0, false, true, 0); // LT
    add_touch_zone(0.90f, 0.02f, 0.12f, 0.05f, 0, false, true, 1); // RT

    // ---- Analog sticks ----
    add_touch_zone(0.20f, 0.85f, 0.18f, 0.18f, 0, true, false, 0); // Left stick
    add_touch_zone(0.80f, 0.85f, 0.18f, 0.18f, 0, true, false, 1); // Right stick

    LOGI("Touch layout configured: %u zones", num_touch_zones_);
}

s32 InputManager::find_touch_zone(f32 nx, f32 ny) const {
    for (u32 i = 0; i < num_touch_zones_; i++) {
        if (touch_zones_[i].contains(nx, ny)) {
            return static_cast<s32>(i);
        }
    }
    return -1;
}

s32 InputManager::find_touch_point(s32 pointer_id) const {
    for (u32 i = 0; i < touch_points_.size(); i++) {
        if (touch_points_[i].id == pointer_id) {
            return static_cast<s32>(i);
        }
    }
    return -1;
}

s32 InputManager::alloc_touch_point(s32 pointer_id) const {
    for (u32 i = 0; i < touch_points_.size(); i++) {
        if (touch_points_[i].id == -1) {
            return static_cast<s32>(i);
        }
    }
    return -1;
}

void InputManager::on_touch_down(u32 player, s32 pointer_id, f32 x, f32 y,
                                  f32 screen_w, f32 screen_h) {
    if (player >= xinput::MAX_CONTROLLERS) return;

    // Normalize coordinates
    f32 nx = x / screen_w;
    f32 ny = y / screen_h;

    std::lock_guard<std::mutex> lock(mutex_);

    // Find the zone being touched
    s32 zone_idx = find_touch_zone(nx, ny);
    if (zone_idx < 0) return;

    // Allocate a touch point tracker
    s32 tp_idx = alloc_touch_point(pointer_id);
    if (tp_idx < 0) return;

    auto& tp = touch_points_[tp_idx];
    tp.id = pointer_id;
    tp.start_x = nx;
    tp.start_y = ny;
    tp.current_x = nx;
    tp.current_y = ny;
    tp.zone_index = zone_idx;

    const auto& zone = touch_zones_[zone_idx];
    auto& state = controllers_[player].state;

    if (zone.button != 0) {
        // Button press
        state.gamepad.buttons |= zone.button;
        state.packet_number++;
    } else if (zone.is_trigger) {
        // Trigger fully pressed on touch
        if (zone.trigger_id == 0) {
            state.gamepad.left_trigger = 255;
        } else {
            state.gamepad.right_trigger = 255;
        }
        state.packet_number++;
    }
    // Stick: handled on move
}

void InputManager::on_touch_move(u32 player, s32 pointer_id, f32 x, f32 y,
                                  f32 screen_w, f32 screen_h) {
    if (player >= xinput::MAX_CONTROLLERS) return;

    f32 nx = x / screen_w;
    f32 ny = y / screen_h;

    std::lock_guard<std::mutex> lock(mutex_);

    s32 tp_idx = find_touch_point(pointer_id);
    if (tp_idx < 0) return;

    auto& tp = touch_points_[tp_idx];
    tp.current_x = nx;
    tp.current_y = ny;

    if (tp.zone_index < 0 || static_cast<u32>(tp.zone_index) >= num_touch_zones_) return;

    const auto& zone = touch_zones_[tp.zone_index];
    if (!zone.is_stick) return;

    // Calculate stick displacement relative to zone center
    f32 dx = (nx - zone.x) / (zone.width / 2.0f);
    f32 dy = -(ny - zone.y) / (zone.height / 2.0f); // Invert Y (screen Y is down, stick Y is up)

    // Clamp to circle
    f32 mag = std::sqrt(dx * dx + dy * dy);
    if (mag > 1.0f) {
        dx /= mag;
        dy /= mag;
    }

    // Apply dead zone
    apply_stick_dead_zone(zone.stick_id, dx, dy);

    s16 sx = static_cast<s16>(dx * 32767.0f);
    s16 sy = static_cast<s16>(dy * 32767.0f);

    auto& state = controllers_[player].state;
    if (zone.stick_id == 0) {
        state.gamepad.thumb_lx = sx;
        state.gamepad.thumb_ly = sy;
    } else {
        state.gamepad.thumb_rx = sx;
        state.gamepad.thumb_ry = sy;
    }
    state.packet_number++;
}

void InputManager::on_touch_up(u32 player, s32 pointer_id) {
    if (player >= xinput::MAX_CONTROLLERS) return;

    std::lock_guard<std::mutex> lock(mutex_);

    s32 tp_idx = find_touch_point(pointer_id);
    if (tp_idx < 0) return;

    auto& tp = touch_points_[tp_idx];

    if (tp.zone_index >= 0 && static_cast<u32>(tp.zone_index) < num_touch_zones_) {
        const auto& zone = touch_zones_[tp.zone_index];
        auto& state = controllers_[player].state;

        if (zone.button != 0) {
            state.gamepad.buttons &= ~zone.button;
            state.packet_number++;
        } else if (zone.is_trigger) {
            if (zone.trigger_id == 0) {
                state.gamepad.left_trigger = 0;
            } else {
                state.gamepad.right_trigger = 0;
            }
            state.packet_number++;
        } else if (zone.is_stick) {
            if (zone.stick_id == 0) {
                state.gamepad.thumb_lx = 0;
                state.gamepad.thumb_ly = 0;
            } else {
                state.gamepad.thumb_rx = 0;
                state.gamepad.thumb_ry = 0;
            }
            state.packet_number++;
        }
    }

    tp.id = -1;
    tp.zone_index = -1;
}

// ============================================================================
// Dead zone
// ============================================================================

void InputManager::apply_stick_dead_zone(u32 stick_id, f32& x, f32& y) const {
    const auto& dz = stick_dead_zones_[stick_id <= 1 ? stick_id : 0];

    if (dz.radial) {
        // Radial dead zone — treats the 2D input as a vector
        f32 mag = std::sqrt(x * x + y * y);
        if (mag < dz.inner) {
            x = 0.0f;
            y = 0.0f;
            return;
        }

        // Rescale from [inner..outer] to [0..1]
        f32 effective_range = dz.outer - dz.inner;
        if (effective_range <= 0.0f) effective_range = 0.01f;

        f32 clamped_mag = std::min(mag, dz.outer);
        f32 normalized = (clamped_mag - dz.inner) / effective_range;

        // Preserve direction, apply new magnitude
        f32 scale = normalized / mag;
        x *= scale;
        y *= scale;
    } else {
        // Axial dead zone — treat each axis independently
        auto apply_axis = [&](f32& v) {
            f32 abs_v = std::abs(v);
            if (abs_v < dz.inner) {
                v = 0.0f;
            } else {
                f32 effective_range = dz.outer - dz.inner;
                if (effective_range <= 0.0f) effective_range = 0.01f;
                f32 clamped = std::min(abs_v, dz.outer);
                f32 normalized = (clamped - dz.inner) / effective_range;
                v = (v > 0 ? normalized : -normalized);
            }
        };
        apply_axis(x);
        apply_axis(y);
    }
}

f32 InputManager::apply_trigger_dead_zone(f32 value) const {
    if (value < trigger_dead_zone_) return 0.0f;

    f32 range = 1.0f - trigger_dead_zone_;
    if (range <= 0.0f) return value;

    return (value - trigger_dead_zone_) / range;
}

void InputManager::set_stick_dead_zone(u32 stick_id, f32 inner, f32 outer) {
    if (stick_id > 1) return;
    std::lock_guard<std::mutex> lock(mutex_);
    stick_dead_zones_[stick_id].inner = std::clamp(inner, 0.0f, 0.9f);
    stick_dead_zones_[stick_id].outer = std::clamp(outer, stick_dead_zones_[stick_id].inner + 0.05f, 1.0f);
    LOGI("Stick %u dead zone: inner=%.2f outer=%.2f",
         stick_id, stick_dead_zones_[stick_id].inner, stick_dead_zones_[stick_id].outer);
}

void InputManager::set_trigger_dead_zone(f32 threshold) {
    std::lock_guard<std::mutex> lock(mutex_);
    trigger_dead_zone_ = std::clamp(threshold, 0.0f, 0.5f);
    LOGI("Trigger dead zone: %.2f", trigger_dead_zone_);
}

const DeadZoneConfig& InputManager::get_stick_dead_zone(u32 stick_id) const {
    static const DeadZoneConfig default_dz{};
    if (stick_id > 1) return default_dz;
    return stick_dead_zones_[stick_id];
}

} // namespace x360mu
