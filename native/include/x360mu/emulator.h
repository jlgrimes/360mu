/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Main emulator interface
 */

#pragma once

#include "x360mu/types.h"
#include <memory>
#include <string>
#include <functional>

namespace x360mu {

// Forward declarations
class Cpu;
class Gpu;
class Apu;
class Memory;
class Kernel;
class VirtualFileSystem;
class ThreadScheduler;

/**
 * Emulator configuration
 */
struct EmulatorConfig {
    // CPU settings
    bool enable_jit = true;  // Re-enabled for debugging
    u32 jit_cache_size_mb = 128;
    
    // GPU settings
    bool use_vulkan = true;
    u32 internal_resolution_scale = 1; // 1 = native, 2 = 2x, etc.
    bool enable_vsync = true;
    bool enable_async_shaders = true;
    
    // Audio settings
    u32 audio_buffer_size_ms = 20;
    bool enable_audio = true;
    
    // Debug settings
    bool enable_logging = true;
    bool enable_gpu_debug = false;
    bool enable_cpu_trace = false;
    
    // Paths
    std::string data_path;      // App internal storage
    std::string cache_path;     // Shader cache, etc.
    std::string save_path;      // Save data location
};

/**
 * Emulator state
 */
enum class EmulatorState {
    Uninitialized,
    Ready,      // Initialized, no game loaded
    Loaded,     // Game loaded, ready to run
    Running,
    Paused,
    Stopped,
    Error
};

/**
 * Frame callback for rendering
 */
using FrameCallback = std::function<void()>;

/**
 * Main emulator class
 * 
 * This is the primary interface for controlling the emulator.
 * It owns all subsystems and coordinates execution.
 */
class Emulator {
public:
    Emulator();
    ~Emulator();
    
    // Disable copy
    Emulator(const Emulator&) = delete;
    Emulator& operator=(const Emulator&) = delete;
    
    /**
     * Initialize the emulator with configuration
     */
    Status initialize(const EmulatorConfig& config);
    
    /**
     * Shutdown and release all resources
     */
    void shutdown();
    
    /**
     * Load a game from file path
     * Supports: .xex, .iso, .god
     */
    Status load_game(const std::string& path);
    
    /**
     * Unload the current game
     */
    void unload_game();
    
    /**
     * Start/resume emulation
     */
    Status run();
    
    /**
     * Pause emulation
     */
    void pause();
    
    /**
     * Stop emulation (can be restarted)
     */
    void stop();
    
    /**
     * Reset the emulated system
     */
    void reset();
    
    /**
     * Execute a single frame
     * Used for frame-by-frame debugging
     */
    void step_frame();
    
    /**
     * Save state to file
     */
    Status save_state(const std::string& path);
    
    /**
     * Load state from file
     */
    Status load_state(const std::string& path);
    
    // Input
    void set_button(u32 player, u32 button, bool pressed);
    void set_trigger(u32 player, u32 trigger, f32 value);
    void set_stick(u32 player, u32 stick, f32 x, f32 y);
    
    // Display
    void set_surface(void* native_window);
    void resize_surface(u32 width, u32 height);
    
    // Callbacks
    void set_frame_callback(FrameCallback callback);

    // Testing
    void test_render();

    // State queries
    EmulatorState get_state() const { return state_; }
    bool is_running() const { return state_ == EmulatorState::Running; }
    
    // Performance stats
    struct Stats {
        f64 fps;
        f64 frame_time_ms;
        u64 frames_rendered;
        u64 cpu_cycles;
        f64 cpu_usage_percent;
        f64 gpu_usage_percent;
        u64 memory_used_bytes;
    };
    Stats get_stats() const;
    
    // Access to subsystems (for debugging/advanced use)
    Cpu* cpu() { return cpu_.get(); }
    Gpu* gpu() { return gpu_.get(); }
    Apu* apu() { return apu_.get(); }
    Memory* memory() { return memory_.get(); }
    Kernel* kernel() { return kernel_.get(); }
    
private:
    // Main emulation loop (runs on dedicated thread)
    void emulation_thread_main();
    
    // Frame timing
    void synchronize_frame();
    
    EmulatorConfig config_;
    EmulatorState state_ = EmulatorState::Uninitialized;
    
    // Core subsystems
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<Cpu> cpu_;
    std::unique_ptr<Gpu> gpu_;
    std::unique_ptr<Apu> apu_;
    std::unique_ptr<Kernel> kernel_;
    std::unique_ptr<VirtualFileSystem> vfs_;
    std::unique_ptr<ThreadScheduler> scheduler_;
    
    // Threading
    struct EmulationThread;
    std::unique_ptr<EmulationThread> emu_thread_;
    
    // Callbacks
    FrameCallback frame_callback_;
    
    // Stats
    Stats stats_{};
};

} // namespace x360mu

