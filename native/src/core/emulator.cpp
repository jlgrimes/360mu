/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Main emulator implementation
 */

#include "x360mu/emulator.h"
#include "cpu/xenon/cpu.h"
#include "gpu/xenos/gpu.h"
#include "apu/audio.h"
#include "memory/memory.h"
#include "kernel/kernel.h"
#include "kernel/filesystem/vfs.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[INFO] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

// Frame timing constants
constexpr auto FRAME_TIME_60FPS = std::chrono::microseconds(16667);  // ~60fps
constexpr auto FRAME_TIME_30FPS = std::chrono::microseconds(33333);  // ~30fps

/**
 * Internal emulation thread state
 */
struct Emulator::EmulationThread {
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> should_stop{false};
    std::atomic<bool> step_frame{false};
    
    std::mutex mutex;
    std::condition_variable cv;
};

Emulator::Emulator() = default;

Emulator::~Emulator() {
    shutdown();
}

Status Emulator::initialize(const EmulatorConfig& config) {
    if (state_ != EmulatorState::Uninitialized) {
        LOGE("Emulator already initialized");
        return Status::Error;
    }
    
    LOGI("Initializing 360μ emulator");
    config_ = config;
    
    // Initialize memory subsystem first (others depend on it)
    LOGI("Initializing memory subsystem");
    memory_ = std::make_unique<Memory>();
    Status status = memory_->initialize();
    if (status != Status::Ok) {
        LOGE("Failed to initialize memory: %s", status_to_string(status));
        return status;
    }
    
    // Initialize CPU
    LOGI("Initializing CPU (JIT: %s)", config_.enable_jit ? "enabled" : "disabled");
    cpu_ = std::make_unique<Cpu>();
    CpuConfig cpu_config{};
    cpu_config.enable_jit = config_.enable_jit;
    cpu_config.jit_cache_size = config_.jit_cache_size_mb * MB;
    status = cpu_->initialize(memory_.get(), cpu_config);
    if (status != Status::Ok) {
        LOGE("Failed to initialize CPU: %s", status_to_string(status));
        return status;
    }
    
    // Initialize GPU
    LOGI("Initializing GPU (Vulkan: %s)", config_.use_vulkan ? "enabled" : "disabled");
    gpu_ = std::make_unique<Gpu>();
    GpuConfig gpu_config{};
    gpu_config.use_vulkan = config_.use_vulkan;
    gpu_config.resolution_scale = config_.internal_resolution_scale;
    gpu_config.enable_vsync = config_.enable_vsync;
    gpu_config.cache_path = config_.cache_path;
    status = gpu_->initialize(memory_.get(), gpu_config);
    if (status != Status::Ok) {
        LOGE("Failed to initialize GPU: %s", status_to_string(status));
        return status;
    }
    
    // Initialize audio
    if (config_.enable_audio) {
        LOGI("Initializing audio subsystem");
        apu_ = std::make_unique<Apu>();
        ApuConfig apu_config{};
        apu_config.buffer_size_ms = config_.audio_buffer_size_ms;
        status = apu_->initialize(memory_.get(), apu_config);
        if (status != Status::Ok) {
            LOGE("Failed to initialize audio: %s", status_to_string(status));
            // Audio failure is non-fatal, continue without audio
            apu_.reset();
        }
    }
    
    // Initialize virtual file system
    LOGI("Initializing virtual file system");
    vfs_ = std::make_unique<VirtualFileSystem>();
    status = vfs_->initialize(config_.data_path, config_.save_path);
    if (status != Status::Ok) {
        LOGE("Failed to initialize VFS: %s", status_to_string(status));
        return status;
    }
    
    // Initialize kernel (HLE)
    LOGI("Initializing kernel HLE");
    kernel_ = std::make_unique<Kernel>();
    status = kernel_->initialize(memory_.get(), cpu_.get(), vfs_.get());
    if (status != Status::Ok) {
        LOGE("Failed to initialize kernel: %s", status_to_string(status));
        return status;
    }
    
    // Connect kernel to CPU for syscall dispatch
    cpu_->set_kernel(kernel_.get());
    LOGI("Connected kernel to CPU for syscall dispatch");
    
    // Create emulation thread controller
    emu_thread_ = std::make_unique<EmulationThread>();
    
    state_ = EmulatorState::Ready;
    LOGI("Emulator initialized successfully");
    return Status::Ok;
}

void Emulator::shutdown() {
    LOGI("Shutting down emulator");
    
    // Stop emulation if running
    stop();
    
    // Wait for thread to finish
    if (emu_thread_ && emu_thread_->thread.joinable()) {
        emu_thread_->should_stop = true;
        emu_thread_->cv.notify_all();
        emu_thread_->thread.join();
    }
    
    // Shutdown subsystems in reverse order
    kernel_.reset();
    vfs_.reset();
    apu_.reset();
    gpu_.reset();
    cpu_.reset();
    memory_.reset();
    emu_thread_.reset();
    
    state_ = EmulatorState::Uninitialized;
    LOGI("Emulator shutdown complete");
}

Status Emulator::load_game(const std::string& path) {
    if (state_ == EmulatorState::Uninitialized) {
        LOGE("Emulator not initialized");
        return Status::Error;
    }
    
    if (state_ == EmulatorState::Running) {
        LOGE("Cannot load game while running");
        return Status::Error;
    }
    
    LOGI("Loading game: %s", path.c_str());
    
    // Determine file type and mount appropriately
    std::string extension = path.substr(path.find_last_of('.'));
    
    if (extension == ".xex" || extension == ".XEX") {
        // Direct XEX file
        Status status = kernel_->load_xex(path);
        if (status != Status::Ok) {
            LOGE("Failed to load XEX: %s", status_to_string(status));
            return status;
        }
    } else if (extension == ".iso" || extension == ".ISO") {
        // ISO image
        Status status = vfs_->mount_iso("\\Device\\Cdrom0", path);
        if (status != Status::Ok) {
            LOGE("Failed to mount ISO: %s", status_to_string(status));
            return status;
        }
        
        // Look for default.xex in root
        status = kernel_->load_xex("\\Device\\Cdrom0\\default.xex");
        if (status != Status::Ok) {
            LOGE("Failed to load default.xex from ISO: %s", status_to_string(status));
            return status;
        }
    } else {
        LOGE("Unknown file format: %s", extension.c_str());
        return Status::InvalidFormat;
    }
    
    state_ = EmulatorState::Loaded;
    LOGI("Game loaded successfully");
    return Status::Ok;
}

void Emulator::unload_game() {
    if (state_ == EmulatorState::Running) {
        stop();
    }
    
    kernel_->unload();
    vfs_->unmount_all();
    memory_->reset();
    
    state_ = EmulatorState::Ready;
}

Status Emulator::run() {
    if (state_ == EmulatorState::Uninitialized || state_ == EmulatorState::Ready) {
        LOGE("No game loaded");
        return Status::Error;
    }
    
    if (state_ == EmulatorState::Running) {
        return Status::Ok; // Already running
    }
    
    LOGI("Starting emulation");
    
    // Start emulation thread if not already started
    if (!emu_thread_->thread.joinable()) {
        emu_thread_->should_stop = false;
        emu_thread_->thread = std::thread([this]() {
            emulation_thread_main();
        });
    }
    
    // Unpause
    {
        std::lock_guard<std::mutex> lock(emu_thread_->mutex);
        emu_thread_->running = true;
        emu_thread_->paused = false;
    }
    emu_thread_->cv.notify_all();
    
    state_ = EmulatorState::Running;
    return Status::Ok;
}

void Emulator::pause() {
    if (state_ != EmulatorState::Running) {
        return;
    }
    
    LOGI("Pausing emulation");
    
    {
        std::lock_guard<std::mutex> lock(emu_thread_->mutex);
        emu_thread_->paused = true;
    }
    
    state_ = EmulatorState::Paused;
}

void Emulator::stop() {
    if (state_ != EmulatorState::Running && state_ != EmulatorState::Paused) {
        return;
    }
    
    LOGI("Stopping emulation");
    
    {
        std::lock_guard<std::mutex> lock(emu_thread_->mutex);
        emu_thread_->running = false;
        emu_thread_->paused = false;
    }
    emu_thread_->cv.notify_all();
    
    state_ = EmulatorState::Stopped;
}

void Emulator::reset() {
    bool was_running = (state_ == EmulatorState::Running);
    
    stop();
    
    // Reset all subsystems
    cpu_->reset();
    gpu_->reset();
    if (apu_) apu_->reset();
    memory_->reset();
    kernel_->reset();
    
    // Reload the game entry point
    kernel_->prepare_entry();
    
    if (was_running) {
        run();
    }
}

void Emulator::step_frame() {
    if (state_ != EmulatorState::Paused) {
        return;
    }
    
    emu_thread_->step_frame = true;
    emu_thread_->cv.notify_all();
}

void Emulator::emulation_thread_main() {
    LOGI("Emulation thread started");
    
    using Clock = std::chrono::high_resolution_clock;
    auto last_frame_time = Clock::now();
    auto target_frame_time = FRAME_TIME_30FPS; // TODO: configurable
    
    while (!emu_thread_->should_stop) {
        // Wait for run signal or step
        {
            std::unique_lock<std::mutex> lock(emu_thread_->mutex);
            emu_thread_->cv.wait(lock, [this]() {
                return emu_thread_->should_stop ||
                       (emu_thread_->running && !emu_thread_->paused) ||
                       emu_thread_->step_frame;
            });
        }
        
        if (emu_thread_->should_stop) {
            break;
        }
        
        bool single_step = emu_thread_->step_frame.exchange(false);
        
        // Execute one frame
        auto frame_start = Clock::now();
        
        // Run CPU until GPU signals frame complete
        // This is a simplified version - real implementation needs proper synchronization
        bool frame_complete = false;
        while (!frame_complete && !emu_thread_->paused && !emu_thread_->should_stop) {
            // Execute a batch of CPU cycles
            cpu_->execute(cpu::CLOCK_SPEED / 60 / 100); // ~1/100th of a frame
            
            // Process GPU command buffer
            gpu_->process_commands();
            
            // Check if GPU finished a frame
            frame_complete = gpu_->frame_complete();
            
            // Process audio
            if (apu_) {
                apu_->process();
            }
        }
        
        // Present frame
        if (frame_complete) {
            gpu_->present();
            stats_.frames_rendered++;
            
            if (frame_callback_) {
                frame_callback_();
            }
        }
        
        // Frame timing
        auto frame_end = Clock::now();
        auto frame_duration = std::chrono::duration_cast<std::chrono::microseconds>(frame_end - frame_start);
        stats_.frame_time_ms = frame_duration.count() / 1000.0;
        
        // Calculate FPS
        auto since_last = std::chrono::duration_cast<std::chrono::microseconds>(frame_end - last_frame_time);
        stats_.fps = 1000000.0 / since_last.count();
        last_frame_time = frame_end;
        
        // Sync to target frame rate (if we're ahead)
        if (!single_step && frame_duration < target_frame_time) {
            std::this_thread::sleep_for(target_frame_time - frame_duration);
        }
        
        // Stop after single frame if stepping
        if (single_step) {
            emu_thread_->paused = true;
        }
    }
    
    LOGI("Emulation thread stopped");
}

// Input methods
void Emulator::set_button(u32 player, u32 button, bool pressed) {
    if (kernel_) {
        kernel_->input_button(player, button, pressed);
    }
}

void Emulator::set_trigger(u32 player, u32 trigger, f32 value) {
    if (kernel_) {
        kernel_->input_trigger(player, trigger, value);
    }
}

void Emulator::set_stick(u32 player, u32 stick, f32 x, f32 y) {
    if (kernel_) {
        kernel_->input_stick(player, stick, x, y);
    }
}

// Display methods
void Emulator::set_surface(void* native_window) {
    if (gpu_) {
        gpu_->set_surface(native_window);
    }
}

void Emulator::resize_surface(u32 width, u32 height) {
    if (gpu_) {
        gpu_->resize(width, height);
    }
}

void Emulator::set_frame_callback(FrameCallback callback) {
    frame_callback_ = std::move(callback);
}

Emulator::Stats Emulator::get_stats() const {
    return stats_;
}

// Save states
Status Emulator::save_state(const std::string& path) {
    // TODO: Implement save states
    (void)path;
    return Status::NotImplemented;
}

Status Emulator::load_state(const std::string& path) {
    // TODO: Implement save states
    (void)path;
    return Status::NotImplemented;
}

} // namespace x360mu

