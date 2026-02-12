/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xenos GPU Implementation
 * 
 * Main GPU orchestrator that ties together:
 * - VulkanBackend (rendering)
 * - CommandProcessor (PM4 packet parsing)
 * - ShaderTranslator (Xenos -> SPIR-V)
 * - TextureCache (texture management)
 */

#include "gpu.h"
#include "command_processor.h"
#include "shader_translator.h"
#include "texture.h"
#include "edram.h"
#include "gpu/vulkan/vulkan_backend.h"
#include "gpu/shader_cache.h"
#include "gpu/descriptor_manager.h"
#include "gpu/buffer_pool.h"
#include "gpu/texture_cache.h"
#include "gpu/render_target.h"
#include "memory/memory.h"
#include "kernel/xobject.h"
#include <cstring>
#include <cstdio>
#include <chrono>
#include <thread>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-gpu"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[GPU] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[GPU ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

Gpu::Gpu() {
    registers_.fill(0);
}

Gpu::~Gpu() {
    shutdown();
}

Status Gpu::initialize(Memory* memory, const GpuConfig& config) {
    memory_ = memory;
    config_ = config;
    
    LOGI("Initializing GPU subsystem...");
    
    // Create Vulkan backend (defer full initialization until set_surface)
    vulkan_ = std::make_unique<VulkanBackend>();

    // Create shader translator
    shader_translator_ = std::make_unique<ShaderTranslator>();
    if (shader_translator_->initialize(config.cache_path) != Status::Ok) {
        LOGE("Failed to initialize shader translator");
        return Status::ErrorInit;
    }

    // Create shader cache
    shader_cache_ = std::make_unique<ShaderCache>();

    // Create descriptor manager
    descriptor_manager_ = std::make_unique<DescriptorManager>();

    // Create buffer pool
    buffer_pool_ = std::make_unique<BufferPool>();

    // Create texture cache
    texture_cache_ = std::make_unique<TextureCache>();

    // Create eDRAM manager
    edram_manager_ = std::make_unique<EdramManager>();
    if (edram_manager_->initialize() != Status::Ok) {
        LOGE("Failed to initialize eDRAM manager");
        return Status::ErrorInit;
    }

    // Create render target manager
    render_target_manager_ = std::make_unique<RenderTargetManager>();

    // Create command processor
    command_processor_ = std::make_unique<CommandProcessor>();
    
    // Initialize ring buffer state to 0 (game will configure it)
    ring_buffer_base_.store(0, std::memory_order_relaxed);
    ring_buffer_size_.store(0, std::memory_order_relaxed);
    read_ptr_.store(0, std::memory_order_relaxed);
    write_ptr_.store(0, std::memory_order_relaxed);
    
    // Set GPU status registers to indicate GPU is ready/idle
    // This helps games that poll GPU status before initializing
    registers_[0x0010] = 0x80000000;  // GRBM_STATUS - GUI_ACTIVE=0, indicates idle
    registers_[0x0014] = 0;           // GRBM_STATUS2
    
    LOGI("GPU initialized (waiting for game to configure ring buffer)");
    return Status::Ok;
}

void Gpu::shutdown() {
    surface_active_ = false;

    if (command_processor_) {
        command_processor_->shutdown();
        command_processor_.reset();
    }

    if (render_target_manager_) {
        render_target_manager_->shutdown();
        render_target_manager_.reset();
    }

    if (edram_manager_) {
        edram_manager_->shutdown();
        edram_manager_.reset();
    }

    if (texture_cache_) {
        texture_cache_->shutdown();
        texture_cache_.reset();
    }

    if (buffer_pool_) {
        buffer_pool_->shutdown();
        buffer_pool_.reset();
    }

    if (descriptor_manager_) {
        descriptor_manager_->shutdown();
        descriptor_manager_.reset();
    }

    if (shader_cache_) {
        shader_cache_->shutdown();
        shader_cache_.reset();
    }

    if (shader_translator_) {
        shader_translator_->shutdown();
        shader_translator_.reset();
    }

    if (vulkan_) {
        vulkan_->shutdown();
        vulkan_.reset();
    }

    memory_ = nullptr;
    LOGI("GPU shutdown complete");
}

void Gpu::reset() {
    // Reset registers
    registers_.fill(0);
    
    // Reset ring buffer state (atomic)
    ring_buffer_base_.store(0, std::memory_order_relaxed);
    ring_buffer_size_.store(0, std::memory_order_relaxed);
    read_ptr_.store(0, std::memory_order_relaxed);
    write_ptr_.store(0, std::memory_order_relaxed);
    
    // Set GPU status to idle/ready
    registers_[0x0010] = 0x80000000;  // GRBM_STATUS - idle
    registers_[0x0014] = 0;           // GRBM_STATUS2
    
    // Reset render state
    render_state_ = {};
    
    // Reset frame state
    frame_complete_ = false;
    in_frame_ = false;
    
    // Reset command processor
    if (command_processor_) {
        command_processor_->reset();
    }
    
    // Reset stats and frame pacing
    stats_ = {};
    frame_count_ = 0;
    last_present_time_ = {};

    LOGI("GPU reset");
}

void Gpu::set_surface(void* native_window) {
    LOGI("GPU::set_surface called with window=%p", native_window);
    
    if (!vulkan_) {
        LOGE("set_surface: Vulkan backend not created!");
        return;
    }
    
    if (!native_window) {
        LOGI("Clearing surface (window is null)");

        // Tear down subsystems that depend on Vulkan surface
        surface_active_ = false;
        in_frame_ = false;

        if (command_processor_) {
            command_processor_->shutdown();
        }
        if (render_target_manager_) {
            render_target_manager_->shutdown();
        }
        if (buffer_pool_) {
            buffer_pool_->shutdown();
        }
        if (descriptor_manager_) {
            descriptor_manager_->shutdown();
        }
        if (shader_cache_) {
            shader_cache_->shutdown();
        }
        if (vulkan_) {
            vulkan_->shutdown();
        }

        LOGI("Surface cleared, GPU subsystems shut down");
        return;
    }
    
    // Initialize Vulkan with the native window
    // Use default 1280x720 resolution, will be resized as needed
    LOGI("Initializing Vulkan with window %p...", native_window);
    Status status = vulkan_->initialize(native_window, 1280, 720);
    if (status != Status::Ok) {
        LOGE("Failed to initialize Vulkan with surface! Status=%d", static_cast<int>(status));
        return;
    }
    LOGI("Vulkan initialized successfully");

    // Initialize shader cache
    if (shader_cache_) {
        LOGI("Initializing shader cache...");
        status = shader_cache_->initialize(vulkan_.get(), shader_translator_.get(), config_.cache_path);
        if (status != Status::Ok) {
            LOGE("Failed to initialize shader cache");
            return;
        }
        LOGI("Shader cache initialized");
    }

    // Initialize descriptor manager
    if (descriptor_manager_) {
        LOGI("Initializing descriptor manager...");
        status = descriptor_manager_->initialize(vulkan_.get());
        if (status != Status::Ok) {
            LOGE("Failed to initialize descriptor manager");
            return;
        }
        LOGI("Descriptor manager initialized");
    }

    // Initialize buffer pool
    if (buffer_pool_) {
        LOGI("Initializing buffer pool...");
        status = buffer_pool_->initialize(vulkan_.get(), 3);  // 3 frames until reuse
        if (status != Status::Ok) {
            LOGE("Failed to initialize buffer pool");
            return;
        }
        LOGI("Buffer pool initialized");
    }

    // Initialize texture cache (xenos/texture.h TextureCache - takes max_size_mb)
    if (texture_cache_) {
        LOGI("Initializing texture cache...");
        status = texture_cache_->initialize(256); // 256MB max cache size
        if (status != Status::Ok) {
            LOGE("Failed to initialize texture cache");
            return;
        }
        LOGI("Texture cache initialized");
    }

    // Initialize render target manager with eDRAM manager
    if (render_target_manager_) {
        LOGI("Initializing render target manager...");
        status = render_target_manager_->initialize(vulkan_.get(), memory_, edram_manager_.get());
        if (status != Status::Ok) {
            LOGE("Failed to initialize render target manager");
            return;
        }
        LOGI("Render target manager initialized with eDRAM");
    }

    // Now initialize command processor with all subsystems
    if (command_processor_ && memory_) {
        LOGI("Initializing command processor with all subsystems...");
        // Note: CommandProcessor expects TextureCacheImpl* but we have TextureCache*
        // Cast it - they should be compatible for now
        status = command_processor_->initialize(memory_, vulkan_.get(),
                                               shader_translator_.get(), reinterpret_cast<TextureCacheImpl*>(texture_cache_.get()),
                                               shader_cache_.get(), descriptor_manager_.get(),
                                               buffer_pool_.get());
        if (status != Status::Ok) {
            LOGE("Failed to initialize command processor! Status=%d", static_cast<int>(status));
        } else {
            LOGI("Command processor initialized with all subsystems");
        }
    }
    
    // Perform a test render to verify Vulkan is working
    LOGI("Performing test render (clear to purple)...");
    vulkan_->clear_screen(0.4f, 0.1f, 0.6f);  // Purple color for debugging
    LOGI("Test render complete");
    
    surface_active_ = true;
    LOGI("Vulkan surface fully initialized");
}

void Gpu::resize(u32 width, u32 height) {
    if (vulkan_ && surface_active_) {
        // width=0, height=0 means recreate at current dimensions
        // (used for swapchain error recovery)
        vulkan_->resize(width, height);
    }
}

void Gpu::process_commands() {
    if (!command_processor_ || !memory_) return;
    
    // Load ring buffer state atomically (acquire to see CPU's writes)
    GuestAddr rb_base = ring_buffer_base_.load(std::memory_order_acquire);
    u32 rb_size = ring_buffer_size_.load(std::memory_order_acquire);
    
    // Check if we have commands to process
    if (rb_base == 0 || rb_size == 0) {
        return;
    }
    
    // Load pointers with acquire semantics to see CPU's command writes
    u32 rp = read_ptr_.load(std::memory_order_acquire);
    u32 wp = write_ptr_.load(std::memory_order_acquire);
    
    // Let the command processor handle the ring buffer
    bool frame_done = command_processor_->process(rb_base, rb_size, rp, wp);
    
    // Store updated read pointer with release semantics
    read_ptr_.store(rp, std::memory_order_release);

    // CP_RB_RPTR writeback: write read pointer to guest memory so CPU can track GPU progress
    // Games configure CP_RB_RPTR_ADDR to specify where the GPU writes back the read pointer
    u32 rptr_addr = registers_[xenos_reg::CP_RB_RPTR_ADDR];
    if (rptr_addr != 0) {
        memory_->write_u32(rptr_addr, rp);
    }

    // Signal GPU fence: we've processed up to the current CPU fence
    // This tells waiting CPU threads that GPU has caught up
    u64 current_cpu_fence = cpu_fence_.load(std::memory_order_acquire);
    if (current_cpu_fence > gpu_fence_.load(std::memory_order_relaxed)) {
        gpu_signal_fence(current_cpu_fence);
    }
    
    if (frame_done) {
        frame_complete_ = true;
        stats_.frames++;
    }
}

void Gpu::present() {
    frame_count_++;

    // Log every 60 frames
    if (frame_count_ % 60 == 1) {
        LOGI("GPU::present() called (frame %llu)", (unsigned long long)frame_count_);
    }

    if (!vulkan_ || !surface_active_) {
        if (frame_count_ % 60 == 1) {
            LOGE("GPU::present() - vulkan not ready (vulkan_=%p, surface_active_=%d)",
                 (void*)vulkan_.get(), surface_active_);
        }
        frame_complete_ = true;
        in_frame_ = false;
        return;
    }

    // Frame skip: only present every (frame_skip_ + 1) frames
    if (frame_skip_ > 0 && (frame_count_ % (frame_skip_ + 1)) != 0) {
        frame_complete_ = true;
        in_frame_ = false;
        return;
    }

    // Begin frame if not already in one
    if (!in_frame_) {
        Status status = vulkan_->begin_frame();
        if (status == Status::ErrorSwapchain) {
            // Swapchain out of date - recreate and retry once
            LOGI("Swapchain out of date on begin_frame, recreating...");
            Status resize_status = vulkan_->resize(0, 0);
            if (resize_status == Status::Ok) {
                status = vulkan_->begin_frame();
            }
        }
        if (status != Status::Ok) {
            if (frame_count_ % 60 == 1) {
                LOGE("Failed to begin frame for present");
            }
            frame_complete_ = true;
            in_frame_ = false;
            return;
        }
    }

    // End frame and present
    Status status = vulkan_->end_frame();
    if (status == Status::ErrorSwapchain) {
        // Swapchain suboptimal or out of date during present
        // Recreate for the next frame
        LOGI("Swapchain error on end_frame, will recreate next frame");
        vulkan_->resize(0, 0);
    } else if (status != Status::Ok) {
        if (frame_count_ % 60 == 1) {
            LOGE("end_frame() failed with status %d", static_cast<int>(status));
        }
    }

    stats_.frames++;
    frame_complete_ = true;
    in_frame_ = false;

    // Frame pacing: sleep to hit target FPS
    if (target_fps_ > 0) {
        auto now = std::chrono::steady_clock::now();
        auto target_interval = std::chrono::nanoseconds(1000000000ULL / target_fps_);

        if (last_present_time_.time_since_epoch().count() > 0) {
            auto elapsed = now - last_present_time_;
            if (elapsed < target_interval) {
                auto sleep_time = target_interval - elapsed;
                std::this_thread::sleep_for(sleep_time);
                now = std::chrono::steady_clock::now();
            }
        }
        last_present_time_ = now;
    }
}

void Gpu::set_vsync(bool enabled) {
    config_.enable_vsync = enabled;
    if (vulkan_) {
        VkPresentModeKHR mode = enabled ?
            VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
        vulkan_->set_present_mode(mode);
        LOGI("VSync %s (present mode %d)", enabled ? "enabled" : "disabled", mode);
    }
}

void Gpu::set_frame_skip(u32 skip_count) {
    frame_skip_ = skip_count;
    LOGI("Frame skip set to %u", skip_count);
}

void Gpu::set_target_fps(u32 fps) {
    target_fps_ = fps;
    LOGI("Target FPS set to %u", fps);
}

void Gpu::set_title_id(u32 title_id) {
    if (shader_cache_) {
        shader_cache_->set_title_id(title_id);
    }
}

void Gpu::test_render() {
    LOGI("=== TEST RENDER: Clearing screen to cyan ===");

    if (!vulkan_) {
        LOGE("Test render failed: Vulkan not initialized");
        return;
    }

    // clear_screen() handles its own frame management (begin/present)
    // so we don't need to call begin_frame/end_frame
    LOGI("Test render: Clearing to cyan (R=0.0, G=1.0, B=1.0)");
    vulkan_->clear_screen(0.0f, 1.0f, 1.0f);  // Cyan - bright and obvious

    LOGI("=== TEST RENDER COMPLETE ===");
    LOGI("If you see a CYAN screen, the rendering pipeline is working!");
}

u32 Gpu::read_register(u32 offset) {
    if (offset < registers_.size()) {
        return registers_[offset];
    }
    return 0;
}

void Gpu::write_register(u32 offset, u32 value) {
    // Log ALL register writes for debugging
    static int write_count = 0;
    write_count++;
    if (write_count <= 50 || (write_count % 1000 == 0)) {
        LOGI("GPU write_register #%d: offset=0x%04X value=0x%08X", write_count, offset, value);
    }
    
    if (offset < registers_.size()) {
        registers_[offset] = value;
        
        // Handle special registers
        switch (offset) {
            case xenos_reg::CP_RB_BASE:
                // Use release to ensure command buffer writes are visible before base is set
                ring_buffer_base_.store(value, std::memory_order_release);
                LOGI("Ring buffer base set: 0x%08X", value);
                break;
                
            case xenos_reg::CP_RB_CNTL:
                {
                    // Ring buffer size is encoded in bits
                    u32 rb_size = 1 << ((value & 0x3F) + 1);
                    ring_buffer_size_.store(rb_size, std::memory_order_release);
                    LOGD("Ring buffer size: %u bytes", rb_size);
                }
                break;
                
            case xenos_reg::CP_RB_RPTR:
                read_ptr_.store(value, std::memory_order_release);
                break;
                
            case xenos_reg::CP_RB_WPTR:
                // CRITICAL: Use release ordering so GPU sees all command buffer writes
                // that the CPU made before updating the write pointer
                write_ptr_.store(value, std::memory_order_release);
                LOGD("Ring buffer write pointer updated: %u", value);
                break;

            // Render target registers - trigger RT update
            case xenos_reg::RB_SURFACE_INFO:
            case xenos_reg::RB_COLOR_INFO:
            case xenos_reg::RB_COLOR1_INFO:
            case xenos_reg::RB_COLOR2_INFO:
            case xenos_reg::RB_COLOR3_INFO:
            case xenos_reg::RB_DEPTH_INFO:
                update_render_targets();
                break;
        }
    }
}

void Gpu::execute_packet(u32 packet) {
    u32 type = (packet >> 30) & 0x3;
    
    switch (type) {
        case 0:
            execute_type0(packet);
            break;
        case 2:
            // Type 2 = NOP, skip
            break;
        case 3:
            execute_type3(packet);
            break;
        default:
            LOGD("Unknown packet type: %u", type);
            break;
    }
}

void Gpu::execute_type0(u32 packet) {
    // Type 0: Register writes
    // Bits 0-15: base register
    // Bits 16-29: count - 1
    u32 base_reg = packet & 0xFFFF;
    u32 count = ((packet >> 16) & 0x3FFF) + 1;
    
    // Load ring buffer state atomically
    GuestAddr rb_base = ring_buffer_base_.load(std::memory_order_acquire);
    u32 rb_size = ring_buffer_size_.load(std::memory_order_acquire);
    u32 rp = read_ptr_.load(std::memory_order_acquire);
    
    // Read data words following the header
    for (u32 i = 0; i < count; i++) {
        rp = (rp + 1) % (rb_size / 4);
        GuestAddr data_addr = rb_base + (rp * 4);
        u32 data = memory_->read_u32(data_addr);
        
        write_register(base_reg + i, data);
    }
    
    // Store updated read pointer
    read_ptr_.store(rp, std::memory_order_release);
}

void Gpu::execute_type3(u32 packet) {
    // Type 3: IT (Indirect Token) commands
    // Bits 0-7: opcode
    // Bits 16-29: count
    u32 opcode = packet & 0xFF;
    u32 count = ((packet >> 16) & 0x3FFF);
    
    // Load ring buffer state atomically
    GuestAddr rb_base = ring_buffer_base_.load(std::memory_order_acquire);
    u32 rb_size = ring_buffer_size_.load(std::memory_order_acquire);
    u32 rp = read_ptr_.load(std::memory_order_acquire);
    
    // Read data words
    std::vector<u32> data(count);
    for (u32 i = 0; i < count; i++) {
        rp = (rp + 1) % (rb_size / 4);
        GuestAddr data_addr = rb_base + (rp * 4);
        data[i] = memory_->read_u32(data_addr);
    }
    
    // Store updated read pointer
    read_ptr_.store(rp, std::memory_order_release);
    
    // Dispatch based on opcode
    // These opcodes are from the PM4 spec
    switch (opcode) {
        case 0x00:  // NOP
            break;
            
        case 0x11:  // DRAW_INDX - indexed draw
            if (count >= 1) {
                u32 info = data[0];
                PrimitiveType prim = static_cast<PrimitiveType>((info >> 0) & 0x3F);
                u32 index_count = (info >> 16) & 0xFFFF;
                GuestAddr index_addr = count >= 2 ? data[1] : 0;
                cmd_draw_indices(prim, index_count, index_addr);
            }
            break;
            
        case 0x12:  // DRAW_INDX_2 - indexed draw variant
            if (count >= 1) {
                u32 info = data[0];
                PrimitiveType prim = static_cast<PrimitiveType>((info >> 0) & 0x3F);
                u32 index_count = (info >> 16) & 0xFFFF;
                cmd_draw_indices(prim, index_count, 0);
            }
            break;
            
        case 0x23:  // DRAW_AUTO - non-indexed draw
            if (count >= 1) {
                u32 info = data[0];
                PrimitiveType prim = static_cast<PrimitiveType>((info >> 0) & 0x3F);
                u32 vertex_count = (info >> 16) & 0xFFFF;
                cmd_draw_auto(prim, vertex_count);
            }
            break;
            
        case 0x25:  // SET_CONSTANT - write shader constants
            // Handle constant writes
            if (count >= 2) {
                u32 const_type = data[0];
                // data[1...] contains constant values
            }
            break;
            
        case 0x43:  // RESOLVE - copy render target to memory
            cmd_resolve();
            break;
            
        case 0x46:  // EVENT_WRITE - synchronization
            // Handle sync events
            break;
            
        case 0x47:  // EVENT_WRITE_SHD - shadow event
            break;
            
        default:
            LOGD("Unhandled PM4 opcode: 0x%02X (count=%u)", opcode, count);
            break;
    }
}

void Gpu::cmd_draw_indices(PrimitiveType type, u32 index_count, GuestAddr index_addr) {
    // Draws are handled by the command processor when it parses the ring buffer
    // This function is called when we manually parse packets in gpu.cpp
    // For now, just track stats - the real draw happens via command processor
    
    stats_.draw_calls++;
    stats_.triangles += index_count / 3;  // Approximate
    
    LOGD("Draw indexed: prim=%d, count=%u, addr=%08X", 
         static_cast<int>(type), index_count, index_addr);
    
    (void)type;
    (void)index_addr;
}

void Gpu::cmd_draw_auto(PrimitiveType type, u32 vertex_count) {
    stats_.draw_calls++;
    stats_.triangles += vertex_count / 3;
    
    LOGD("Draw auto: prim=%d, count=%u",
         static_cast<int>(type), vertex_count);
    
    (void)type;
}

void Gpu::cmd_resolve() {
    // Resolve eDRAM render targets to main memory
    LOGD("Resolve command");

    // Perform the actual eDRAM → main memory resolve using register state
    if (render_target_manager_) {
        render_target_manager_->resolve_edram_to_memory(registers_.data());
    }

    // Present after resolve (resolves typically mark end of rendering pass)
    if (in_frame_) {
        present();
    }
}

void Gpu::update_render_targets() {
    if (render_target_manager_) {
        render_target_manager_->update_from_registers(registers_.data());
    }
}

void Gpu::update_render_state() {
    // Read render state from our copy of registers
    // The command processor maintains its own copy from ring buffer commands
    render_state_.depth_test = (registers_[xenos_reg::RB_DEPTHCONTROL] & 0x1) != 0;
    render_state_.depth_write = (registers_[xenos_reg::RB_DEPTHCONTROL] & 0x2) != 0;
    render_state_.depth_func = (registers_[xenos_reg::RB_DEPTHCONTROL] >> 4) & 0x7;
    
    render_state_.blend_enable = (registers_[xenos_reg::RB_COLORCONTROL] & 0x1) != 0;
    
    u32 cull_mode_reg = registers_[xenos_reg::PA_SU_SC_MODE_CNTL];
    render_state_.cull_mode = (cull_mode_reg >> 0) & 0x3;
    render_state_.front_ccw = (cull_mode_reg >> 2) & 0x1;
}

void Gpu::update_shaders() {
    // Shader addresses from registers
    render_state_.vertex_shader_address = registers_[xenos_reg::SQ_VS_PROGRAM] << 8;
    render_state_.pixel_shader_address = registers_[xenos_reg::SQ_PS_PROGRAM] << 8;
}

void Gpu::update_textures() {
    // Texture state is handled by the texture cache when samplers are bound
}

//=============================================================================
// VSync Interrupt
//=============================================================================

void Gpu::signal_vsync() {
    // Increment VSync counter in GPU register space
    // Xbox 360 uses RBBM_STATUS (0x0E40) bit 0 for VBlank, and a VSync counter
    // at COHER_STATUS_HOST (used by D3D for VBlank queries)
    static u32 vsync_count = 0;
    vsync_count++;
    registers_[0x0E40] = vsync_count;  // VSync counter register

    // Signal GPU interrupt so kernel event waiters (VBlank wait) get woken
    KernelState::instance().queue_gpu_interrupt();

    LOGD("VSync signal #%u", vsync_count);
}

//=============================================================================
// CPU/GPU Fence Synchronization
//=============================================================================

void Gpu::cpu_signal_fence(u64 fence_value) {
    // CPU has written commands up to this fence
    u64 current = cpu_fence_.load(std::memory_order_relaxed);
    while (fence_value > current) {
        if (cpu_fence_.compare_exchange_weak(current, fence_value,
                                              std::memory_order_release,
                                              std::memory_order_relaxed)) {
            break;
        }
    }
    LOGD("CPU signaled fence: %llu", fence_value);
}

void Gpu::gpu_signal_fence(u64 fence_value) {
    // GPU has processed commands up to this fence
    u64 current = gpu_fence_.load(std::memory_order_relaxed);
    while (fence_value > current) {
        if (gpu_fence_.compare_exchange_weak(current, fence_value,
                                              std::memory_order_release,
                                              std::memory_order_relaxed)) {
            break;
        }
    }
    
    // Notify any waiting threads
    {
        std::lock_guard<std::mutex> lock(fence_mutex_);
        fence_cv_.notify_all();
    }
    
    LOGD("GPU signaled fence: %llu", fence_value);
}

bool Gpu::wait_for_gpu_fence(u64 fence_value, u64 timeout_ns) {
    // Fast path: already reached
    if (gpu_fence_.load(std::memory_order_acquire) >= fence_value) {
        return true;
    }
    
    // Zero timeout: just check, don't wait
    if (timeout_ns == 0) {
        return false;
    }
    
    // Wait with timeout
    std::unique_lock<std::mutex> lock(fence_mutex_);
    
    if (timeout_ns == UINT64_MAX) {
        // Infinite wait
        fence_cv_.wait(lock, [this, fence_value]() {
            return gpu_fence_.load(std::memory_order_acquire) >= fence_value;
        });
        return true;
    } else {
        // Timed wait
        auto timeout = std::chrono::nanoseconds(timeout_ns);
        return fence_cv_.wait_for(lock, timeout, [this, fence_value]() {
            return gpu_fence_.load(std::memory_order_acquire) >= fence_value;
        });
    }
}

} // namespace x360mu
