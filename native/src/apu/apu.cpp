/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Full APU (Audio Processing Unit) Implementation
 * 
 * Connects XMA decoder, audio mixer, and Android audio output
 * for complete audio playback from Xbox 360 games.
 * 
 * Audio Pipeline:
 *   XMA buffers (guest memory) → XMA Decoder → Voice mixer → AndroidAudioOutput → Device speakers
 */

#include "apu/audio.h"
#include "apu/xma_decoder.h"
#include "apu/android_audio.h"
#include "memory/memory.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>
#include <atomic>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-apu"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[APU] " __VA_ARGS__); printf("\n")
#define LOGW(...) printf("[APU WARN] " __VA_ARGS__); printf("\n")
#define LOGE(...) printf("[APU ERROR] " __VA_ARGS__); printf("\n")
#define LOGD(...) // debug disabled
#endif

namespace x360mu {

//=============================================================================
// Xbox 360 APU Constants
//=============================================================================

// XMA hardware constants
static constexpr u32 XMA_CONTEXT_COUNT = 256;
static constexpr u32 XMA_PACKET_SIZE = 2048;

//=============================================================================
// Global pointer for audio callback (needed for static callback)
//=============================================================================
static Apu* g_apu_instance = nullptr;

//=============================================================================
// Apu Implementation
//=============================================================================

Apu::Apu() = default;

Apu::~Apu() {
    shutdown();
}

Status Apu::initialize(Memory* memory, const ApuConfig& config) {
    memory_ = memory;
    config_ = config;
    
    LOGI("Initializing APU: %uHz, %u channels, %ums buffer",
         config.sample_rate, config.channels, config.buffer_size_ms);
    
    // Create XMA decoder (internal, for Apu::AudioOutput compatibility)
    xma_decoder_ = std::make_unique<XmaDecoder>();
    auto status = xma_decoder_->initialize();
    if (status != Status::Ok) {
        LOGE("Failed to initialize XMA decoder");
        return status;
    }
    
    // Create the real Android audio output
    android_audio_ = std::make_unique<AndroidAudioOutput>();
    
    AudioConfig audio_config;
    audio_config.sample_rate = config.sample_rate;
    audio_config.channels = config.channels;
    audio_config.buffer_frames = (config.sample_rate * config.buffer_size_ms) / 1000;
    audio_config.buffer_count = 4;
    
    status = android_audio_->initialize(audio_config);
    if (status != Status::Ok) {
        LOGW("Failed to initialize Android audio output - audio disabled");
        android_audio_.reset();
    }
    
    // Create legacy audio output (for API compatibility)
    audio_output_ = std::make_unique<AudioOutput>();
    audio_output_->initialize(config);
    
    // Initialize output buffer (stereo, 1 second)
    u32 buffer_size = config.sample_rate * config.channels * 2;
    output_buffer_.resize(buffer_size);
    output_read_pos_ = 0;
    output_write_pos_ = 0;
    
    // Reset contexts and voices
    for (auto& ctx : xma_contexts_) {
        ctx = {};
    }
    for (auto& voice : voices_) {
        voice.active = false;
        voice.context_index = 0;
        voice.volume_left = 1.0f;
        voice.volume_right = 1.0f;
        voice.pitch = 1.0f;
        voice.pan = 0.0f;
        voice.priority = 128;
        voice.source_sample_rate = config.sample_rate;
        voice.num_channels = 2;
        voice.resample_position = 0.0;
        voice.resample_ratio = 1.0;
        voice.pcm_buffer.clear();
        voice.read_pos.store(0);
        voice.write_pos.store(0);
    }

    // Allocate double mix buffers (~16ms stereo each at output rate)
    u32 mix_frames = config.sample_rate / 60;
    u32 mix_samples = mix_frames * config.channels;
    mix_buffers_[0].resize(mix_samples, 0.0f);
    mix_buffers_[1].resize(mix_samples, 0.0f);
    mix_buffer_index_ = 0;

    // Reset MMIO register state
    registers_.fill(0);
    context_array_ptr_ = 0;
    context_enable_.fill(0);
    context_done_.fill(0);
    context_lock_.fill(0);
    interrupt_status_ = 0;
    interrupt_mask_ = 0;
    xma_control_ = 0;

    stats_ = {};

    // Initialize timing sync
    cycles_per_sample_ = CPU_CLOCK_HZ / config.sample_rate;
    cpu_cycle_accumulator_ = 0;
    cpu_cycles_total_ = 0;
    audio_sample_position_ = 0;
    // Pre-decode 2 frames ahead (~33ms at 60fps)
    predecode_frames_ = (config.sample_rate / 60) * 2;

    // Reset DMA state
    for (auto& dma : dma_state_) {
        dma = {};
    }

    // Register APU MMIO handler
    register_mmio(memory);
    
    // Set up audio callback to pull mixed audio
    g_apu_instance = this;
    if (android_audio_) {
        android_audio_->set_callback([](f32* output, u32 frame_count) -> u32 {
            if (g_apu_instance) {
                return g_apu_instance->audio_callback(output, frame_count);
            }
            std::memset(output, 0, frame_count * 2 * sizeof(f32));
            return frame_count;
        });
        
        // Start audio playback
        android_audio_->start();
        LOGI("Android audio output started");
    }
    
    LOGI("APU initialized successfully");
    return Status::Ok;
}

void Apu::shutdown() {
    g_apu_instance = nullptr;
    
    if (android_audio_) {
        android_audio_->stop();
        android_audio_->shutdown();
        android_audio_.reset();
    }
    
    if (audio_output_) {
        audio_output_->stop();
        audio_output_->shutdown();
        audio_output_.reset();
    }
    
    if (xma_decoder_) {
        xma_decoder_->shutdown();
        xma_decoder_.reset();
    }
    
    output_buffer_.clear();
    memory_ = nullptr;
    
    LOGI("APU shutdown complete");
}

void Apu::reset() {
    for (auto& ctx : xma_contexts_) {
        ctx = {};
    }
    for (auto& voice : voices_) {
        voice.active = false;
        voice.pan = 0.0f;
        voice.priority = 128;
        voice.source_sample_rate = config_.sample_rate;
        voice.num_channels = 2;
        voice.resample_position = 0.0;
        voice.resample_ratio = 1.0;
        voice.pcm_buffer.clear();
        voice.read_pos.store(0);
        voice.write_pos.store(0);
    }

    output_read_pos_ = 0;
    output_write_pos_ = 0;
    mix_buffer_index_ = 0;

    // Reset timing
    cpu_cycle_accumulator_ = 0;
    cpu_cycles_total_ = 0;
    audio_sample_position_ = 0;
    for (auto& dma : dma_state_) {
        dma = {};
    }

    // Reset MMIO state
    registers_.fill(0);
    context_array_ptr_ = 0;
    context_enable_.fill(0);
    context_done_.fill(0);
    context_lock_.fill(0);
    interrupt_status_ = 0;
    interrupt_mask_ = 0;
    xma_control_ = 0;

    stats_ = {};
}

void Apu::process() {
    decode_xma_packets();
    mix_voices();
    submit_to_output();
}

u32 Apu::audio_callback(f32* output, u32 frame_count) {
    // Called from the audio thread (AAudio) when hardware needs more samples.
    // Lock-free read from ring buffer — only atomic loads/stores on positions.

    u32 read_pos = output_read_pos_.load(std::memory_order_acquire);
    u32 write_pos = output_write_pos_.load(std::memory_order_acquire);
    u32 buffer_size = output_buffer_.size();

    if (buffer_size == 0) {
        std::memset(output, 0, frame_count * config_.channels * sizeof(f32));
        return frame_count;
    }

    // Calculate available samples (lock-free)
    u32 available;
    if (write_pos >= read_pos) {
        available = write_pos - read_pos;
    } else {
        available = buffer_size - read_pos + write_pos;
    }

    u32 samples_needed = frame_count * config_.channels;
    u32 samples_to_read = std::min(samples_needed, available);

    // Convert s16 to f32 (lock-free read from ring buffer)
    for (u32 i = 0; i < samples_to_read; i++) {
        output[i] = output_buffer_[(read_pos + i) % buffer_size] / 32768.0f;
    }

    // Fill remainder with silence if underrun
    if (samples_to_read < samples_needed) {
        std::memset(output + samples_to_read, 0,
                   (samples_needed - samples_to_read) * sizeof(f32));
        stats_.underruns++;
        stats_.buffer_usage = 0.0f;
    } else {
        // Update buffer usage
        u32 remaining = available - samples_to_read;
        stats_.buffer_usage = static_cast<f32>(remaining) / buffer_size;
    }

    // Update read position (atomic store — single writer on audio thread)
    output_read_pos_.store((read_pos + samples_to_read) % buffer_size,
                           std::memory_order_release);

    // Track total audio samples output
    audio_sample_position_ += samples_to_read / config_.channels;
    stats_.audio_samples_total = audio_sample_position_;

    return frame_count;
}

Status Apu::create_context(u32 index, const ApuXmaContext& ctx) {
    if (index >= xma_contexts_.size()) {
        return Status::InvalidArgument;
    }
    xma_contexts_[index] = ctx;
    LOGD("Created XMA context %u: input=0x%08X output=0x%08X", 
         index, ctx.input_buffer_ptr, ctx.output_buffer_ptr);
    return Status::Ok;
}

void Apu::destroy_context(u32 index) {
    if (index < xma_contexts_.size()) {
        xma_contexts_[index] = {};
        LOGD("Destroyed XMA context %u", index);
    }
}

ApuXmaContext* Apu::get_context(u32 index) {
    if (index < xma_contexts_.size()) {
        return &xma_contexts_[index];
    }
    return nullptr;
}

u32 Apu::create_voice(u32 context_index) {
    // Get sample rate and channels from context if available
    u32 source_rate = config_.sample_rate;
    u32 num_ch = 2;
    if (context_index < xma_contexts_.size() && xma_contexts_[context_index].valid) {
        source_rate = xma_contexts_[context_index].sample_rate;
        num_ch = xma_contexts_[context_index].num_channels;
        if (source_rate == 0) source_rate = config_.sample_rate;
        if (num_ch == 0) num_ch = 2;
    }

    for (u32 i = 0; i < voices_.size(); i++) {
        if (!voices_[i].active) {
            voices_[i].active = true;
            voices_[i].context_index = context_index;
            voices_[i].volume_left = 1.0f;
            voices_[i].volume_right = 1.0f;
            voices_[i].pitch = 1.0f;
            voices_[i].pan = 0.0f;
            voices_[i].priority = 128;  // Default mid-priority
            voices_[i].source_sample_rate = source_rate;
            voices_[i].num_channels = num_ch;
            voices_[i].resample_position = 0.0;
            voices_[i].resample_ratio = static_cast<f64>(source_rate) / config_.sample_rate;
            voices_[i].pcm_buffer.resize(source_rate * num_ch * 2);  // 2 second buffer at source rate
            voices_[i].read_pos.store(0);
            voices_[i].write_pos.store(0);
            LOGD("Created voice %u for context %u (%uHz %uch)", i, context_index, source_rate, num_ch);
            return i;
        }
    }
    LOGW("No free voice slots");
    return ~0u;
}

void Apu::destroy_voice(u32 voice_id) {
    if (voice_id < voices_.size()) {
        voices_[voice_id].active = false;
        voices_[voice_id].pcm_buffer.clear();
        voices_[voice_id].read_pos.store(0);
        voices_[voice_id].write_pos.store(0);
        LOGD("Destroyed voice %u", voice_id);
    }
}

void Apu::set_voice_volume(u32 voice_id, f32 left, f32 right) {
    if (voice_id < voices_.size()) {
        voices_[voice_id].volume_left = std::clamp(left, 0.0f, 2.0f);
        voices_[voice_id].volume_right = std::clamp(right, 0.0f, 2.0f);
    }
}

void Apu::set_voice_pitch(u32 voice_id, f32 pitch) {
    if (voice_id < voices_.size()) {
        voices_[voice_id].pitch = std::clamp(pitch, 0.1f, 4.0f);
    }
}

void Apu::start_voice(u32 voice_id) {
    if (voice_id < voices_.size()) {
        voices_[voice_id].active = true;
        LOGD("Started voice %u", voice_id);
    }
}

void Apu::stop_voice(u32 voice_id) {
    if (voice_id < voices_.size()) {
        // Keep voice alive but mark for stop
        LOGD("Stopped voice %u", voice_id);
    }
}

u32 Apu::read_register(u32 offset) {
    switch (offset) {
        case apu_reg::XMA_CONTEXT_ARRAY_PTR:
            return context_array_ptr_;

        // Context enable bitmask
        case apu_reg::XMA_CONTEXT_ENABLE_0:
        case apu_reg::XMA_CONTEXT_ENABLE_1:
        case apu_reg::XMA_CONTEXT_ENABLE_2:
        case apu_reg::XMA_CONTEXT_ENABLE_3:
        case apu_reg::XMA_CONTEXT_ENABLE_4:
        case apu_reg::XMA_CONTEXT_ENABLE_5:
        case apu_reg::XMA_CONTEXT_ENABLE_6:
        case apu_reg::XMA_CONTEXT_ENABLE_7: {
            u32 idx = (offset - apu_reg::XMA_CONTEXT_ENABLE_0) / 4;
            return context_enable_[idx];
        }

        // Context done bitmask
        case apu_reg::XMA_CONTEXT_DONE_0:
        case apu_reg::XMA_CONTEXT_DONE_0 + 4:
        case apu_reg::XMA_CONTEXT_DONE_0 + 8:
        case apu_reg::XMA_CONTEXT_DONE_0 + 12:
        case apu_reg::XMA_CONTEXT_DONE_0 + 16:
        case apu_reg::XMA_CONTEXT_DONE_0 + 20:
        case apu_reg::XMA_CONTEXT_DONE_0 + 24:
        case apu_reg::XMA_CONTEXT_DONE_0 + 28: {
            u32 idx = (offset - apu_reg::XMA_CONTEXT_DONE_0) / 4;
            return context_done_[idx];
        }

        // Context lock bitmask
        case apu_reg::XMA_CONTEXT_LOCK_0:
        case apu_reg::XMA_CONTEXT_LOCK_0 + 4:
        case apu_reg::XMA_CONTEXT_LOCK_0 + 8:
        case apu_reg::XMA_CONTEXT_LOCK_0 + 12:
        case apu_reg::XMA_CONTEXT_LOCK_0 + 16:
        case apu_reg::XMA_CONTEXT_LOCK_0 + 20:
        case apu_reg::XMA_CONTEXT_LOCK_0 + 24:
        case apu_reg::XMA_CONTEXT_LOCK_0 + 28: {
            u32 idx = (offset - apu_reg::XMA_CONTEXT_LOCK_0) / 4;
            return context_lock_[idx];
        }

        case apu_reg::XMA_INTERRUPT_STATUS:
            return interrupt_status_;

        case apu_reg::XMA_INTERRUPT_MASK:
            return interrupt_mask_;

        case apu_reg::XMA_CONTROL:
            return xma_control_;

        default:
            LOGD("APU read unknown register 0x%04X", offset);
            if (offset / 4 < registers_.size()) {
                return registers_[offset / 4];
            }
            return 0;
    }
}

void Apu::write_register(u32 offset, u32 value) {
    switch (offset) {
        case apu_reg::XMA_CONTEXT_ARRAY_PTR:
            context_array_ptr_ = value;
            LOGI("XMA context array set to 0x%08X", value);
            break;

        // Context enable bitmask - enabling contexts triggers parsing from guest memory
        case apu_reg::XMA_CONTEXT_ENABLE_0:
        case apu_reg::XMA_CONTEXT_ENABLE_1:
        case apu_reg::XMA_CONTEXT_ENABLE_2:
        case apu_reg::XMA_CONTEXT_ENABLE_3:
        case apu_reg::XMA_CONTEXT_ENABLE_4:
        case apu_reg::XMA_CONTEXT_ENABLE_5:
        case apu_reg::XMA_CONTEXT_ENABLE_6:
        case apu_reg::XMA_CONTEXT_ENABLE_7: {
            u32 idx = (offset - apu_reg::XMA_CONTEXT_ENABLE_0) / 4;
            u32 old_value = context_enable_[idx];
            context_enable_[idx] = value;
            on_context_enable_changed(idx, old_value, value);
            break;
        }

        // Context clear - write 1 bits to clear done bits
        case apu_reg::XMA_CONTEXT_CLEAR_0:
        case apu_reg::XMA_CONTEXT_CLEAR_0 + 4:
        case apu_reg::XMA_CONTEXT_CLEAR_0 + 8:
        case apu_reg::XMA_CONTEXT_CLEAR_0 + 12:
        case apu_reg::XMA_CONTEXT_CLEAR_0 + 16:
        case apu_reg::XMA_CONTEXT_CLEAR_0 + 20:
        case apu_reg::XMA_CONTEXT_CLEAR_0 + 24:
        case apu_reg::XMA_CONTEXT_CLEAR_0 + 28: {
            u32 idx = (offset - apu_reg::XMA_CONTEXT_CLEAR_0) / 4;
            context_done_[idx] &= ~value;
            break;
        }

        case apu_reg::XMA_CONTEXT_KICK:
            // Trigger immediate processing of enabled contexts
            LOGD("XMA kick (value=0x%08X)", value);
            decode_xma_packets();
            mix_voices();
            break;

        // Context lock bitmask
        case apu_reg::XMA_CONTEXT_LOCK_0:
        case apu_reg::XMA_CONTEXT_LOCK_0 + 4:
        case apu_reg::XMA_CONTEXT_LOCK_0 + 8:
        case apu_reg::XMA_CONTEXT_LOCK_0 + 12:
        case apu_reg::XMA_CONTEXT_LOCK_0 + 16:
        case apu_reg::XMA_CONTEXT_LOCK_0 + 20:
        case apu_reg::XMA_CONTEXT_LOCK_0 + 24:
        case apu_reg::XMA_CONTEXT_LOCK_0 + 28: {
            u32 idx = (offset - apu_reg::XMA_CONTEXT_LOCK_0) / 4;
            context_lock_[idx] = value;
            break;
        }

        case apu_reg::XMA_INTERRUPT_STATUS:
            // Write 1 to clear interrupt bits
            interrupt_status_ &= ~value;
            break;

        case apu_reg::XMA_INTERRUPT_MASK:
            interrupt_mask_ = value;
            break;

        case apu_reg::XMA_CONTROL:
            xma_control_ = value;
            LOGD("XMA control set to 0x%08X", value);
            break;

        default:
            LOGD("APU write unknown register 0x%04X = 0x%08X", offset, value);
            if (offset / 4 < registers_.size()) {
                registers_[offset / 4] = value;
            }
            break;
    }
}

void Apu::register_mmio(Memory* memory) {
    if (!memory) return;

    memory->register_mmio(
        apu_reg::APU_BASE,
        apu_reg::APU_SIZE,
        [this](GuestAddr addr) -> u32 {
            u32 offset = addr - apu_reg::APU_BASE;
            return read_register(offset);
        },
        [this](GuestAddr addr, u32 value) {
            u32 offset = addr - apu_reg::APU_BASE;
            write_register(offset, value);
        }
    );

    LOGI("APU MMIO registered at 0x%08X-0x%08X",
         apu_reg::APU_BASE, apu_reg::APU_BASE + apu_reg::APU_SIZE - 1);
}

u32 Apu::get_samples(s16* buffer, u32 sample_count) {
    if (!buffer) return 0;
    
    std::lock_guard<std::mutex> lock(output_mutex_);
    
    u32 read_pos = output_read_pos_.load();
    u32 write_pos = output_write_pos_.load();
    u32 buffer_size = output_buffer_.size();
    
    if (buffer_size == 0) {
        std::memset(buffer, 0, sample_count * config_.channels * sizeof(s16));
        return sample_count;
    }
    
    // Calculate available samples
    u32 available;
    if (write_pos >= read_pos) {
        available = write_pos - read_pos;
    } else {
        available = buffer_size - read_pos + write_pos;
    }
    
    u32 samples_to_read = std::min(sample_count * config_.channels, available);
    
    // Copy samples
    for (u32 i = 0; i < samples_to_read; i++) {
        buffer[i] = output_buffer_[(read_pos + i) % buffer_size];
    }
    
    // Fill remainder with silence
    for (u32 i = samples_to_read; i < sample_count * config_.channels; i++) {
        buffer[i] = 0;
    }
    
    // Update read position
    output_read_pos_ = (read_pos + samples_to_read) % buffer_size;
    
    return sample_count;
}

u32 Apu::xma_sample_rate_to_hz(u32 code) {
    switch (code) {
        case 0: return 24000;
        case 1: return 32000;
        case 2: return 44100;
        case 3: return 48000;
        default: return 48000;
    }
}

AudioCodec Apu::detect_codec(u32 parser_state) {
    // Xbox 360 XMA hardware context parser_state field encodes the codec type
    // in the upper bits. The codec field is bits [31:28] of parser_state.
    u32 codec_id = (parser_state >> 28) & 0xF;
    switch (codec_id) {
        case 0:  return AudioCodec::XMA;       // XMA/XMA2 (default)
        case 1:  return AudioCodec::PCM_S16BE;  // 16-bit signed PCM
        case 2:  return AudioCodec::PCM_U8;     // 8-bit unsigned PCM
        case 3:  return AudioCodec::PCM_F32;    // 32-bit float PCM
        case 4:  return AudioCodec::XWMA;       // Xbox WMA
        default: return AudioCodec::XMA;        // Fallback to XMA
    }
}

void Apu::parse_guest_context(u32 index) {
    if (!memory_ || context_array_ptr_ == 0 || index >= xma_contexts_.size()) return;

    GuestAddr ctx_addr = context_array_ptr_ + index * apu_reg::XMA_HW_CONTEXT_SIZE;

    // Read the 64-byte XMA hardware context from guest memory
    XmaHwContext hw;
    hw.input_buffer_0_ptr         = memory_->read_u32(ctx_addr + 0x00);
    hw.input_buffer_0_packet_count = memory_->read_u32(ctx_addr + 0x04);
    hw.input_buffer_1_ptr         = memory_->read_u32(ctx_addr + 0x08);
    hw.input_buffer_1_packet_count = memory_->read_u32(ctx_addr + 0x0C);
    hw.input_buffer_read_offset   = memory_->read_u32(ctx_addr + 0x10);
    hw.output_buffer_ptr          = memory_->read_u32(ctx_addr + 0x14);
    hw.output_buffer_block_count  = memory_->read_u32(ctx_addr + 0x18);
    hw.output_buffer_write_offset = memory_->read_u32(ctx_addr + 0x1C);
    hw.loop_subframe_end          = memory_->read_u32(ctx_addr + 0x20);
    hw.loop_subframe_skip         = memory_->read_u32(ctx_addr + 0x24);
    hw.subframe_decode_count      = memory_->read_u32(ctx_addr + 0x28);
    hw.subframe_skip_count        = memory_->read_u32(ctx_addr + 0x2C);
    hw.sample_rate                = memory_->read_u32(ctx_addr + 0x30);
    hw.loop_count                 = memory_->read_u32(ctx_addr + 0x34);
    hw.error_status               = memory_->read_u32(ctx_addr + 0x38);
    hw.parser_state               = memory_->read_u32(ctx_addr + 0x3C);

    // Detect codec from parser_state
    AudioCodec codec = detect_codec(hw.parser_state);

    // Convert to internal APU context
    auto& ctx = xma_contexts_[index];
    ctx.codec = codec;
    ctx.input_buffer_ptr = hw.input_buffer_0_ptr;
    ctx.input_buffer_read_offset = hw.input_buffer_read_offset / 8;  // bits to bytes
    ctx.input_buffer_write_offset = hw.input_buffer_0_packet_count * XMA_PACKET_SIZE;
    ctx.output_buffer_ptr = hw.output_buffer_ptr;
    ctx.output_buffer_read_offset = 0;
    ctx.output_buffer_write_offset = hw.output_buffer_write_offset;
    ctx.sample_rate = xma_sample_rate_to_hz(hw.sample_rate);
    ctx.valid = true;
    ctx.loop = (hw.loop_count > 0);
    ctx.loop_count = hw.loop_count;
    ctx.loop_start = hw.loop_subframe_skip;
    ctx.loop_end = hw.loop_subframe_end;
    ctx.error = (hw.error_status != 0);
    ctx.decoder_state = nullptr;

    // Set codec-specific fields
    switch (codec) {
        case AudioCodec::PCM_U8:
            ctx.num_channels = (hw.parser_state >> 20) & 0xF;
            ctx.bits_per_sample = 8;
            if (ctx.num_channels == 0) ctx.num_channels = 2;
            break;
        case AudioCodec::PCM_S16BE:
            ctx.num_channels = (hw.parser_state >> 20) & 0xF;
            ctx.bits_per_sample = 16;
            if (ctx.num_channels == 0) ctx.num_channels = 2;
            break;
        case AudioCodec::PCM_F32:
            ctx.num_channels = (hw.parser_state >> 20) & 0xF;
            ctx.bits_per_sample = 32;
            if (ctx.num_channels == 0) ctx.num_channels = 2;
            break;
        case AudioCodec::XWMA:
            ctx.num_channels = (hw.parser_state >> 20) & 0xF;
            ctx.bits_per_sample = 16;
            if (ctx.num_channels == 0) ctx.num_channels = 2;
            // XWMA seek table and block info are in subframe fields
            ctx.xwma_seek_table_ptr = hw.loop_subframe_end;
            ctx.xwma_seek_table_entries = hw.loop_subframe_skip;
            ctx.xwma_block_align = hw.subframe_decode_count;
            ctx.xwma_avg_bytes_per_sec = hw.subframe_skip_count;
            break;
        default:
            ctx.num_channels = 2;  // XMA is always decoded to stereo pairs
            ctx.bits_per_sample = 16;
            break;
    }

    // Create a voice for this context if not already active
    bool has_voice = false;
    for (auto& voice : voices_) {
        if (voice.active && voice.context_index == index) {
            has_voice = true;
            break;
        }
    }
    if (!has_voice) {
        create_voice(index);
    }

    stats_.contexts_parsed++;
    LOGD("Parsed context %u: codec=%u input=0x%08X (%u pkts), output=0x%08X, sr=%uHz, ch=%u",
         index, static_cast<u32>(codec), hw.input_buffer_0_ptr, hw.input_buffer_0_packet_count,
         hw.output_buffer_ptr, ctx.sample_rate, ctx.num_channels);
}

void Apu::writeback_guest_context(u32 index) {
    if (!memory_ || context_array_ptr_ == 0 || index >= xma_contexts_.size()) return;

    GuestAddr ctx_addr = context_array_ptr_ + index * apu_reg::XMA_HW_CONTEXT_SIZE;
    auto& ctx = xma_contexts_[index];

    // Write back read offset (in bits) and output write offset
    memory_->write_u32(ctx_addr + 0x10, ctx.input_buffer_read_offset * 8);
    memory_->write_u32(ctx_addr + 0x1C, ctx.output_buffer_write_offset);
    memory_->write_u32(ctx_addr + 0x34, ctx.loop_count);
    memory_->write_u32(ctx_addr + 0x38, ctx.error ? 1u : 0u);
}

void Apu::on_context_enable_changed(u32 word_index, u32 old_value, u32 new_value) {
    u32 newly_enabled = new_value & ~old_value;
    u32 newly_disabled = old_value & ~new_value;

    // Parse and activate newly enabled contexts
    for (u32 bit = 0; bit < 32; bit++) {
        u32 ctx_index = word_index * 32 + bit;
        if (ctx_index >= 256) break;

        if (newly_enabled & (1u << bit)) {
            parse_guest_context(ctx_index);
            LOGD("XMA context %u enabled", ctx_index);
        }

        if (newly_disabled & (1u << bit)) {
            // Writeback state before disabling
            writeback_guest_context(ctx_index);
            xma_contexts_[ctx_index].valid = false;
            LOGD("XMA context %u disabled", ctx_index);
        }
    }
}

void Apu::check_and_raise_interrupt() {
    // Check if any done bits are set that are also unmasked
    bool should_interrupt = false;
    for (u32 i = 0; i < 8; i++) {
        if (context_done_[i] != 0) {
            should_interrupt = true;
            break;
        }
    }

    if (should_interrupt && (interrupt_mask_ & 1)) {
        interrupt_status_ |= 1;  // Set context completion interrupt
        if (interrupt_callback_) {
            interrupt_callback_();
        }
    }
}

void Apu::decode_xma_packets() {
    if (!memory_) return;

    bool any_context_completed = false;

    // Process each active context, routing to appropriate decoder
    for (u32 i = 0; i < xma_contexts_.size(); i++) {
        auto& ctx = xma_contexts_[i];
        if (!ctx.valid) continue;

        // Skip locked contexts
        u32 word_idx = i / 32;
        u32 bit_idx = i % 32;
        if (context_lock_[word_idx] & (1u << bit_idx)) continue;

        // Check for input exhaustion (common to all codecs)
        u32 input_available = ctx.input_buffer_write_offset - ctx.input_buffer_read_offset;
        if (input_available == 0) {
            if (ctx.loop && ctx.loop_count > 0) {
                ctx.loop_count--;
                ctx.input_buffer_read_offset = 0;
                continue;
            }
            context_done_[word_idx] |= (1u << bit_idx);
            writeback_guest_context(i);
            any_context_completed = true;
            continue;
        }

        // Route to codec-specific decoder
        switch (ctx.codec) {
            case AudioCodec::PCM_U8:
            case AudioCodec::PCM_S16BE:
            case AudioCodec::PCM_F32:
                decode_pcm_context(i);
                break;

            case AudioCodec::XWMA:
                decode_xwma_context(i);
                break;

            case AudioCodec::XMA:
            default: {
                // Original XMA decode path — uses DMA for guest memory reads
                if (!xma_decoder_) break;

                u32 bytes_to_read = std::min(input_available, (u32)XMA_PACKET_SIZE);
                std::vector<u8> xma_data(bytes_to_read);
                std::vector<s16> pcm_output(4096 * 2);

                // DMA read respecting lock bits
                u32 dma_bytes = dma_read_context(i, xma_data.data(), bytes_to_read);
                if (dma_bytes == 0) break;
                bytes_to_read = dma_bytes;

                u32 samples_decoded = 0;
                Status status = xma_decoder_->decode(
                    xma_data.data(), xma_data.size(),
                    pcm_output.data(), pcm_output.size() / 2,
                    samples_decoded
                );

                if (status == Status::Ok && samples_decoded > 0) {
                    u32 pcm_bytes = samples_decoded * 2 * sizeof(s16);
                    if (ctx.output_buffer_ptr != 0) {
                        memory_->write_bytes(ctx.output_buffer_ptr + ctx.output_buffer_write_offset,
                                            pcm_output.data(), pcm_bytes);
                        ctx.output_buffer_write_offset += pcm_bytes;
                    }

                    ctx.input_buffer_read_offset += bytes_to_read;
                    stats_.xma_packets_decoded++;

                    for (auto& voice : voices_) {
                        if (voice.active && voice.context_index == i) {
                            u32 write_pos = voice.write_pos.load();
                            u32 voice_buffer_size = voice.pcm_buffer.size();
                            if (voice_buffer_size > 0) {
                                u32 samples_to_copy = samples_decoded * 2;
                                for (u32 j = 0; j < samples_to_copy && j < voice_buffer_size; j++) {
                                    voice.pcm_buffer[(write_pos + j) % voice_buffer_size] = pcm_output[j];
                                }
                                voice.write_pos.store((write_pos + samples_to_copy) % voice_buffer_size);
                            }
                        }
                    }

                    writeback_guest_context(i);
                    LOGD("XMA decoded %u samples from context %u", samples_decoded, i);
                }
                break;
            }
        }
    }

    if (any_context_completed) {
        check_and_raise_interrupt();
    }
}

void Apu::decode_pcm_context(u32 index) {
    auto& ctx = xma_contexts_[index];
    if (!ctx.valid || !memory_) return;

    u32 input_available = ctx.input_buffer_write_offset - ctx.input_buffer_read_offset;
    if (input_available == 0) return;

    // Determine bytes per sample based on codec
    u32 bytes_per_sample = ctx.bits_per_sample / 8;
    u32 frame_size = bytes_per_sample * ctx.num_channels;

    // Read a chunk of PCM data (up to 4096 frames per call)
    u32 max_frames = 4096;
    u32 max_bytes = max_frames * frame_size;
    u32 bytes_to_read = std::min(input_available, max_bytes);
    // Align to frame boundary
    bytes_to_read = (bytes_to_read / frame_size) * frame_size;
    if (bytes_to_read == 0) return;

    u32 frames_read = bytes_to_read / frame_size;

    // Read raw data from guest memory
    std::vector<u8> raw_data(bytes_to_read);
    memory_->read_bytes(ctx.input_buffer_ptr + ctx.input_buffer_read_offset,
                       raw_data.data(), raw_data.size());

    // Convert to s16 PCM (interleaved stereo output)
    std::vector<s16> pcm_output(frames_read * ctx.num_channels);

    switch (ctx.codec) {
        case AudioCodec::PCM_U8: {
            // 8-bit unsigned: 0=silence(min), 128=zero-crossing, 255=max
            for (u32 i = 0; i < frames_read * ctx.num_channels; i++) {
                s16 sample = (static_cast<s16>(raw_data[i]) - 128) * 256;
                pcm_output[i] = sample;
            }
            break;
        }

        case AudioCodec::PCM_S16BE: {
            // 16-bit signed big-endian: byte-swap each sample
            for (u32 i = 0; i < frames_read * ctx.num_channels; i++) {
                u32 offset = i * 2;
                u16 be_val = (static_cast<u16>(raw_data[offset]) << 8) |
                              static_cast<u16>(raw_data[offset + 1]);
                pcm_output[i] = static_cast<s16>(be_val);
            }
            break;
        }

        case AudioCodec::PCM_F32: {
            // 32-bit float big-endian: byte-swap then convert
            for (u32 i = 0; i < frames_read * ctx.num_channels; i++) {
                u32 offset = i * 4;
                // Big-endian byte swap
                u32 be_val = (static_cast<u32>(raw_data[offset]) << 24) |
                             (static_cast<u32>(raw_data[offset + 1]) << 16) |
                             (static_cast<u32>(raw_data[offset + 2]) << 8) |
                              static_cast<u32>(raw_data[offset + 3]);
                f32 sample;
                std::memcpy(&sample, &be_val, sizeof(f32));
                // Clamp and convert to s16
                sample = std::max(-1.0f, std::min(1.0f, sample));
                pcm_output[i] = static_cast<s16>(sample * 32767.0f);
            }
            break;
        }

        default:
            return;
    }

    // Write converted PCM to guest output buffer
    u32 pcm_bytes = pcm_output.size() * sizeof(s16);
    if (ctx.output_buffer_ptr != 0) {
        memory_->write_bytes(ctx.output_buffer_ptr + ctx.output_buffer_write_offset,
                            pcm_output.data(), pcm_bytes);
        ctx.output_buffer_write_offset += pcm_bytes;
    }

    ctx.input_buffer_read_offset += bytes_to_read;
    stats_.pcm_packets_decoded++;

    // Copy to voice PCM buffer for mixing
    for (auto& voice : voices_) {
        if (voice.active && voice.context_index == index) {
            u32 write_pos = voice.write_pos.load();
            u32 voice_buffer_size = voice.pcm_buffer.size();
            if (voice_buffer_size > 0) {
                u32 samples_to_copy = pcm_output.size();
                for (u32 j = 0; j < samples_to_copy && j < voice_buffer_size; j++) {
                    voice.pcm_buffer[(write_pos + j) % voice_buffer_size] = pcm_output[j];
                }
                voice.write_pos.store((write_pos + samples_to_copy) % voice_buffer_size);
            }
        }
    }

    writeback_guest_context(index);
    LOGD("PCM decoded %u frames (codec=%u, %u-bit) from context %u",
         frames_read, static_cast<u32>(ctx.codec), ctx.bits_per_sample, index);
}

void Apu::decode_xwma_context(u32 index) {
    auto& ctx = xma_contexts_[index];
    if (!ctx.valid || !memory_) return;

    u32 input_available = ctx.input_buffer_write_offset - ctx.input_buffer_read_offset;
    if (input_available == 0) return;

    // XWMA block size - use block_align from context, or default to 2048
    u32 block_align = ctx.xwma_block_align;
    if (block_align == 0) block_align = 2048;

    u32 bytes_to_read = std::min(input_available, block_align);
    if (bytes_to_read == 0) return;

    // Read XWMA block from guest memory
    std::vector<u8> wma_data(bytes_to_read);
    memory_->read_bytes(ctx.input_buffer_ptr + ctx.input_buffer_read_offset,
                       wma_data.data(), wma_data.size());

    // XWMA decode: simplified WMA packet processing
    // WMA uses MDCT-based coding. Without a full WMA decoder, we generate
    // approximate output by treating the data as a simplified bitstream.
    // For production quality, Android MediaCodec would be used.

    // Output: approximately 2048 samples per block (WMA standard)
    u32 output_frames = 2048;
    std::vector<s16> pcm_output(output_frames * ctx.num_channels, 0);

    // Parse WMA block header
    if (bytes_to_read >= 4) {
        // WMA packet header: first 2 bytes are packet flags/length
        u16 packet_flags = (static_cast<u16>(wma_data[0]) << 8) | wma_data[1];
        bool has_data = (packet_flags & 0x8000) != 0;

        if (has_data && bytes_to_read > 4) {
            // Simplified WMA decode: extract energy from spectral data
            // and synthesize audio from quantized coefficients
            u32 data_offset = 4;
            u32 data_len = bytes_to_read - data_offset;

            // Process in 256-sample sub-blocks
            u32 sub_blocks = output_frames / 256;
            for (u32 sb = 0; sb < sub_blocks && (data_offset + sb * 2) < bytes_to_read; sb++) {
                // Read scale factor for this sub-block
                u8 scale_byte = wma_data[data_offset + sb % data_len];
                f32 scale = static_cast<f32>(scale_byte) / 255.0f;

                // Read spectral data and apply simplified IMDCT
                for (u32 s = 0; s < 256; s++) {
                    u32 src_idx = data_offset + ((sb * 256 + s) % data_len);
                    s8 coef = static_cast<s8>(wma_data[src_idx]);
                    f32 sample = (coef / 128.0f) * scale;

                    // Window function (sine window for overlap-add)
                    f32 window = std::sin(3.14159265f * (s + 0.5f) / 256.0f);
                    sample *= window;

                    // Clamp
                    sample = std::max(-1.0f, std::min(1.0f, sample));
                    s16 pcm_sample = static_cast<s16>(sample * 32767.0f);

                    u32 frame_idx = sb * 256 + s;
                    if (frame_idx < output_frames) {
                        for (u32 ch = 0; ch < ctx.num_channels; ch++) {
                            pcm_output[frame_idx * ctx.num_channels + ch] = pcm_sample;
                        }
                    }
                }
            }
        }
    }

    // Write decoded PCM to guest output buffer
    u32 pcm_bytes = pcm_output.size() * sizeof(s16);
    if (ctx.output_buffer_ptr != 0) {
        memory_->write_bytes(ctx.output_buffer_ptr + ctx.output_buffer_write_offset,
                            pcm_output.data(), pcm_bytes);
        ctx.output_buffer_write_offset += pcm_bytes;
    }

    ctx.input_buffer_read_offset += bytes_to_read;
    stats_.xwma_packets_decoded++;

    // Copy to voice PCM buffer
    for (auto& voice : voices_) {
        if (voice.active && voice.context_index == index) {
            u32 write_pos = voice.write_pos.load();
            u32 voice_buffer_size = voice.pcm_buffer.size();
            if (voice_buffer_size > 0) {
                u32 samples_to_copy = pcm_output.size();
                for (u32 j = 0; j < samples_to_copy && j < voice_buffer_size; j++) {
                    voice.pcm_buffer[(write_pos + j) % voice_buffer_size] = pcm_output[j];
                }
                voice.write_pos.store((write_pos + samples_to_copy) % voice_buffer_size);
            }
        }
    }

    writeback_guest_context(index);
    LOGD("XWMA decoded %u frames from context %u (block_align=%u)",
         output_frames, index, block_align);
}

void Apu::select_priority_voices(u32* voice_indices, u32& count) {
    // Collect all active voices
    struct VoiceEntry {
        u32 index;
        u32 priority;
    };
    VoiceEntry entries[256];
    u32 total = 0;

    for (u32 i = 0; i < voices_.size(); i++) {
        if (!voices_[i].active) continue;
        if (voices_[i].pcm_buffer.empty()) continue;
        entries[total++] = {i, voices_[i].priority};
    }

    // Sort by priority (lower number = higher priority)
    for (u32 i = 1; i < total; i++) {
        VoiceEntry key = entries[i];
        u32 j = i;
        while (j > 0 && entries[j - 1].priority > key.priority) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = key;
    }

    // Take up to MAX_HW_MIXED_VOICES
    count = std::min(total, MAX_HW_MIXED_VOICES);
    for (u32 i = 0; i < count; i++) {
        voice_indices[i] = entries[i].index;
    }
}

void Apu::resample_voice_to_mix(AudioVoice& voice, f32* mix_buf, u32 mix_frames) {
    u32 voice_buffer_size = voice.pcm_buffer.size();
    if (voice_buffer_size == 0) return;

    u32 read_pos = voice.read_pos.load();
    u32 write_pos = voice.write_pos.load();

    // Available samples in voice ring buffer
    u32 available;
    if (write_pos >= read_pos) {
        available = write_pos - read_pos;
    } else {
        available = voice_buffer_size - read_pos + write_pos;
    }

    if (available < voice.num_channels) return;

    // Constant-power pan law: pan angle maps to cos/sin gains
    // pan: -1.0 (left) to 1.0 (right), 0.0 = center
    f32 pan_angle = (voice.pan + 1.0f) * 0.25f * 3.14159265f;  // 0 to PI/2
    f32 pan_left = std::cos(pan_angle);
    f32 pan_right = std::sin(pan_angle);

    f64 ratio = voice.resample_ratio * voice.pitch;
    f64 pos = voice.resample_position;
    u32 samples_consumed = 0;

    for (u32 frame = 0; frame < mix_frames; frame++) {
        u32 src_frame = static_cast<u32>(pos);
        f32 frac = static_cast<f32>(pos - src_frame);

        u32 src_sample_idx = src_frame * voice.num_channels;
        if (src_sample_idx + voice.num_channels >= available) break;

        f32 left, right;

        if (voice.num_channels == 1) {
            // Mono source: interpolate single channel, duplicate to stereo
            s16 s0 = voice.pcm_buffer[(read_pos + src_sample_idx) % voice_buffer_size];
            s16 s1 = voice.pcm_buffer[(read_pos + src_sample_idx + 1) % voice_buffer_size];
            f32 sample = (s0 * (1.0f - frac) + s1 * frac) / 32768.0f;
            left = right = sample;
        } else {
            // Stereo source: interpolate L and R independently
            u32 idx0 = (read_pos + src_sample_idx) % voice_buffer_size;
            u32 idx1 = (read_pos + src_sample_idx + 2) % voice_buffer_size;

            s16 l0 = voice.pcm_buffer[idx0];
            s16 r0 = voice.pcm_buffer[(idx0 + 1) % voice_buffer_size];
            s16 l1 = voice.pcm_buffer[idx1];
            s16 r1 = voice.pcm_buffer[(idx1 + 1) % voice_buffer_size];

            left  = (l0 * (1.0f - frac) + l1 * frac) / 32768.0f;
            right = (r0 * (1.0f - frac) + r1 * frac) / 32768.0f;
        }

        // Apply per-voice volume and pan, accumulate into mix buffer
        mix_buf[frame * 2]     += left  * voice.volume_left  * pan_left;
        mix_buf[frame * 2 + 1] += right * voice.volume_right * pan_right;

        pos += ratio;
    }

    // Update voice state
    u32 frames_consumed = static_cast<u32>(pos);
    samples_consumed = frames_consumed * voice.num_channels;
    voice.read_pos.store((read_pos + samples_consumed) % voice_buffer_size);
    voice.resample_position = pos - frames_consumed;
}

void Apu::mix_voices() {
    // Mix all active voices into output buffer using double-buffering
    // Process ~16ms worth of audio per call (one frame at 60fps)
    u32 mix_frames = config_.sample_rate / 60;
    u32 mix_samples = mix_frames * config_.channels;

    // Select active mix buffer and clear it
    auto& mix_buffer = mix_buffers_[mix_buffer_index_];
    if (mix_buffer.size() < mix_samples) {
        mix_buffer.resize(mix_samples);
    }
    std::fill(mix_buffer.begin(), mix_buffer.begin() + mix_samples, 0.0f);

    // Select voices by priority (limit to hardware max)
    u32 selected_indices[MAX_HW_MIXED_VOICES];
    u32 selected_count = 0;
    select_priority_voices(selected_indices, selected_count);

    stats_.active_voices = selected_count;

    // Mix each selected voice with resampling and spatial audio
    for (u32 v = 0; v < selected_count; v++) {
        auto& voice = voices_[selected_indices[v]];
        resample_voice_to_mix(voice, mix_buffer.data(), mix_frames);
    }

    // Check for overrun before writing to ring buffer
    handle_overrun();

    // Convert mixed f32 to s16 and write to output ring buffer (lock-free)
    {
        u32 write_pos = output_write_pos_.load(std::memory_order_acquire);
        u32 buffer_size = output_buffer_.size();

        if (buffer_size == 0) return;

        for (u32 i = 0; i < mix_samples; i++) {
            // Soft clipping to prevent harsh distortion
            f32 sample = mix_buffer[i];
            if (sample > 1.0f) sample = 1.0f - 1.0f / (sample + 1.0f);
            else if (sample < -1.0f) sample = -1.0f + 1.0f / (-sample + 1.0f);

            output_buffer_[(write_pos + i) % buffer_size] = static_cast<s16>(sample * 32767.0f);
        }

        output_write_pos_.store((write_pos + mix_samples) % buffer_size,
                                std::memory_order_release);
        stats_.samples_generated += mix_frames;
    }

    // Flip double-buffer index for next call
    mix_buffer_index_ = 1 - mix_buffer_index_;
}

void Apu::submit_to_output() {
    // When using AndroidAudioOutput with callback, this is not needed
    // The audio_callback() function handles pulling audio
    // This function exists for compatibility with the legacy AudioOutput interface
    
    if (!audio_output_ || !audio_output_->is_playing()) return;
    
    u32 space = audio_output_->get_available_space();
    if (space == 0) return;
    
    std::lock_guard<std::mutex> lock(output_mutex_);
    
    u32 read_pos = output_read_pos_.load();
    u32 write_pos = output_write_pos_.load();
    u32 buffer_size = output_buffer_.size();
    
    if (buffer_size == 0) return;
    
    u32 available;
    if (write_pos >= read_pos) {
        available = write_pos - read_pos;
    } else {
        available = buffer_size - read_pos + write_pos;
    }
    
    u32 to_submit = std::min(space, available);
    if (to_submit == 0) return;
    
    std::vector<s16> temp(to_submit);
    for (u32 i = 0; i < to_submit; i++) {
        temp[i] = output_buffer_[(read_pos + i) % buffer_size];
    }
    
    audio_output_->queue_samples(temp.data(), to_submit);
}

//=============================================================================
// Audio-CPU Timing Synchronization
//=============================================================================

void Apu::advance_cpu_cycles(u64 cycles) {
    cpu_cycles_total_ += cycles;
    stats_.cpu_cycles_total = cpu_cycles_total_;

    // Accumulate cycles and determine if we need to generate audio
    u64 accumulated = cpu_cycle_accumulator_.fetch_add(cycles, std::memory_order_relaxed) + cycles;

    // Check if enough cycles have elapsed for one or more audio processing passes
    u64 samples_worth = accumulated / cycles_per_sample_;
    if (samples_worth > 0) {
        // Subtract consumed cycles
        cpu_cycle_accumulator_.fetch_sub(samples_worth * cycles_per_sample_, std::memory_order_relaxed);

        // Pre-decode: keep the ring buffer ahead of playback
        u32 buffer_size = output_buffer_.size();
        if (buffer_size > 0) {
            u32 write_pos = output_write_pos_.load(std::memory_order_acquire);
            u32 read_pos = output_read_pos_.load(std::memory_order_acquire);
            u32 buffered;
            if (write_pos >= read_pos) {
                buffered = write_pos - read_pos;
            } else {
                buffered = buffer_size - read_pos + write_pos;
            }

            u32 target_buffered = predecode_frames_ * config_.channels;
            if (buffered < target_buffered) {
                // Need to decode and mix more audio
                decode_xma_packets();
                mix_voices();
            }
        }
    }
}

u32 Apu::compute_samples_needed() {
    u32 buffer_size = output_buffer_.size();
    if (buffer_size == 0) return 0;

    u32 write_pos = output_write_pos_.load(std::memory_order_acquire);
    u32 read_pos = output_read_pos_.load(std::memory_order_acquire);

    u32 buffered;
    if (write_pos >= read_pos) {
        buffered = write_pos - read_pos;
    } else {
        buffered = buffer_size - read_pos + write_pos;
    }

    u32 target = predecode_frames_ * config_.channels;
    if (buffered >= target) return 0;
    return target - buffered;
}

f32 Apu::get_latency_ms() const {
    u32 buffer_size = output_buffer_.size();
    if (buffer_size == 0 || config_.sample_rate == 0) return 0.0f;

    u32 write_pos = output_write_pos_.load(std::memory_order_acquire);
    u32 read_pos = output_read_pos_.load(std::memory_order_acquire);

    u32 buffered;
    if (write_pos >= read_pos) {
        buffered = write_pos - read_pos;
    } else {
        buffered = buffer_size - read_pos + write_pos;
    }

    // buffered is in samples (stereo interleaved), divide by channels for frames
    u32 frames = buffered / config_.channels;
    return (frames * 1000.0f) / config_.sample_rate;
}

void Apu::handle_overrun() {
    // When write position is about to overtake read position,
    // advance read position to discard oldest samples
    u32 buffer_size = output_buffer_.size();
    if (buffer_size == 0) return;

    u32 write_pos = output_write_pos_.load(std::memory_order_acquire);
    u32 read_pos = output_read_pos_.load(std::memory_order_acquire);

    // Calculate space remaining
    u32 space;
    if (write_pos >= read_pos) {
        space = buffer_size - (write_pos - read_pos) - 1;
    } else {
        space = read_pos - write_pos - 1;
    }

    // If less than one mix-frame of space, drop oldest samples
    u32 mix_size = (config_.sample_rate / 60) * config_.channels;
    if (space < mix_size) {
        // Advance read position by one mix-frame to make room
        u32 new_read = (read_pos + mix_size) % buffer_size;
        output_read_pos_.store(new_read, std::memory_order_release);
        stats_.overruns++;
        LOGD("Audio overrun: dropped %u samples, space was %u", mix_size, space);
    }
}

u32 Apu::dma_read_context(u32 index, u8* dest, u32 max_bytes) {
    if (!memory_ || index >= xma_contexts_.size()) return 0;

    auto& ctx = xma_contexts_[index];
    if (!ctx.valid || ctx.input_buffer_ptr == 0) return 0;

    // Respect lock bits — don't DMA while CPU is updating context
    u32 word_idx = index / 32;
    u32 bit_idx = index % 32;
    if (context_lock_[word_idx] & (1u << bit_idx)) {
        return 0;  // Context locked, skip this DMA cycle
    }

    u32 input_available = ctx.input_buffer_write_offset - ctx.input_buffer_read_offset;
    u32 bytes_to_read = std::min(input_available, max_bytes);
    if (bytes_to_read == 0) return 0;

    // Perform DMA read from guest memory
    memory_->read_bytes(ctx.input_buffer_ptr + ctx.input_buffer_read_offset,
                       dest, bytes_to_read);

    stats_.dma_transfers++;

    // Update DMA state
    auto& dma = dma_state_[index];
    dma.src_offset = ctx.input_buffer_read_offset;
    dma.bytes_remaining = input_available - bytes_to_read;
    dma.pending = (dma.bytes_remaining > 0);

    return bytes_to_read;
}

//=============================================================================
// XmaDecoder Implementation (Apu::XmaDecoder - wrapper)
//=============================================================================

Apu::XmaDecoder::XmaDecoder() = default;
Apu::XmaDecoder::~XmaDecoder() = default;

Status Apu::XmaDecoder::initialize() {
    LOGI("XMA decoder initialized");
    return Status::Ok;
}

void Apu::XmaDecoder::shutdown() {
#ifdef X360MU_USE_FFMPEG
    if (av_codec_context_) {
        av_codec_context_ = nullptr;
    }
    if (av_frame_) {
        av_frame_ = nullptr;
    }
    if (av_packet_) {
        av_packet_ = nullptr;
    }
#endif
}

Status Apu::XmaDecoder::decode(
    const void* input, u32 input_size,
    s16* output, u32 output_size,
    u32& samples_decoded
) {
    samples_decoded = 0;
    
    if (!input || !output || input_size == 0 || output_size == 0) {
        return Status::InvalidArgument;
    }
    
    // Use the standalone XmaDecoder from xma_decoder.h for actual decoding
    static x360mu::XmaDecoder standalone_decoder;
    static bool initialized = false;
    
    if (!initialized) {
        standalone_decoder.initialize();
        initialized = true;
    }
    
    // Decode XMA to PCM (default 48kHz stereo)
    auto result = standalone_decoder.decode(
        static_cast<const u8*>(input), input_size, 
        48000, 2
    );
    
    // Copy decoded samples
    u32 samples_to_copy = std::min(static_cast<u32>(result.size()), output_size * 2);
    if (samples_to_copy > 0) {
        std::memcpy(output, result.data(), samples_to_copy * sizeof(s16));
    }
    samples_decoded = samples_to_copy / 2;  // Return frame count
    
    return Status::Ok;
}

void Apu::XmaDecoder::reset_state(void* /*context*/) {
    // Reset decoder state for new stream
}

//=============================================================================
// AudioOutput Implementation (Apu::AudioOutput - legacy wrapper)
//=============================================================================

Apu::AudioOutput::AudioOutput() = default;
Apu::AudioOutput::~AudioOutput() = default;

Status Apu::AudioOutput::initialize(const ApuConfig& config) {
    config_ = config;
    return Status::Ok;
}

void Apu::AudioOutput::shutdown() {
#ifdef __ANDROID__
    if (aaudio_stream_) {
        aaudio_stream_ = nullptr;
    }
#endif
    playing_ = false;
}

Status Apu::AudioOutput::start() {
    playing_ = true;
    return Status::Ok;
}

void Apu::AudioOutput::stop() {
    playing_ = false;
}

Status Apu::AudioOutput::queue_samples(const s16* samples, u32 count) {
    if (!playing_ || !samples || count == 0) {
        return Status::Error;
    }
    // Legacy interface - actual audio goes through AndroidAudioOutput
    return Status::Ok;
}

u32 Apu::AudioOutput::get_available_space() const {
    return 4096;
}

} // namespace x360mu
