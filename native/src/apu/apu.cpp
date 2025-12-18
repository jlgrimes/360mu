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
        voice.pcm_buffer.clear();
        voice.read_pos.store(0);
        voice.write_pos.store(0);
    }
    
    stats_ = {};
    
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
        voice.pcm_buffer.clear();
        voice.read_pos.store(0);
        voice.write_pos.store(0);
    }
    
    output_read_pos_ = 0;
    output_write_pos_ = 0;
    stats_ = {};
}

void Apu::process() {
    decode_xma_packets();
    mix_voices();
    submit_to_output();
}

u32 Apu::audio_callback(f32* output, u32 frame_count) {
    // This is called from the audio thread when AAudio needs more samples
    
    std::lock_guard<std::mutex> lock(output_mutex_);
    
    u32 read_pos = output_read_pos_.load();
    u32 write_pos = output_write_pos_.load();
    u32 buffer_size = output_buffer_.size();
    
    if (buffer_size == 0) {
        std::memset(output, 0, frame_count * config_.channels * sizeof(f32));
        return frame_count;
    }
    
    // Calculate available samples
    u32 available;
    if (write_pos >= read_pos) {
        available = write_pos - read_pos;
    } else {
        available = buffer_size - read_pos + write_pos;
    }
    
    u32 samples_needed = frame_count * config_.channels;
    u32 samples_to_read = std::min(samples_needed, available);
    
    // Convert s16 to f32 and copy
    for (u32 i = 0; i < samples_to_read; i++) {
        output[i] = output_buffer_[(read_pos + i) % buffer_size] / 32768.0f;
    }
    
    // Fill remainder with silence if underrun
    if (samples_to_read < samples_needed) {
        for (u32 i = samples_to_read; i < samples_needed; i++) {
            output[i] = 0.0f;
        }
        stats_.buffer_usage = 0.0f;  // Underrun indicator
    }
    
    // Update read position
    output_read_pos_ = (read_pos + samples_to_read) % buffer_size;
    
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
    for (u32 i = 0; i < voices_.size(); i++) {
        if (!voices_[i].active) {
            voices_[i].active = true;
            voices_[i].context_index = context_index;
            voices_[i].volume_left = 1.0f;
            voices_[i].volume_right = 1.0f;
            voices_[i].pitch = 1.0f;
            voices_[i].pcm_buffer.resize(config_.sample_rate * config_.channels * 2);  // 2 second buffer
            voices_[i].read_pos.store(0);
            voices_[i].write_pos.store(0);
            LOGD("Created voice %u for context %u", i, context_index);
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

u32 Apu::read_register(u32 /*offset*/) {
    return 0;
}

void Apu::write_register(u32 /*offset*/, u32 /*value*/) {
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

void Apu::decode_xma_packets() {
    if (!memory_ || !xma_decoder_) return;
    
    // Process each active context
    for (u32 i = 0; i < xma_contexts_.size(); i++) {
        auto& ctx = xma_contexts_[i];
        if (!ctx.valid) continue;
        
        // Read XMA data from guest memory
        u32 input_available = ctx.input_buffer_write_offset - ctx.input_buffer_read_offset;
        if (input_available == 0) continue;
        
        // Allocate buffers
        u32 bytes_to_read = std::min(input_available, (u32)XMA_PACKET_SIZE);
        std::vector<u8> xma_data(bytes_to_read);
        std::vector<s16> pcm_output(4096 * 2);  // Stereo output
        
        // Read XMA data from guest memory
        memory_->read_bytes(ctx.input_buffer_ptr + ctx.input_buffer_read_offset,
                           xma_data.data(), xma_data.size());
        
        // Decode XMA to PCM
        u32 samples_decoded = 0;
        Status status = xma_decoder_->decode(
            xma_data.data(), xma_data.size(),
            pcm_output.data(), pcm_output.size() / 2,  // frames, not samples
            samples_decoded
        );
        
        if (status == Status::Ok && samples_decoded > 0) {
            // Write decoded PCM to guest output buffer
            u32 pcm_bytes = samples_decoded * 2 * sizeof(s16);  // stereo s16
            if (ctx.output_buffer_ptr != 0) {
                memory_->write_bytes(ctx.output_buffer_ptr + ctx.output_buffer_write_offset,
                                    pcm_output.data(), pcm_bytes);
                ctx.output_buffer_write_offset += pcm_bytes;
            }
            
            ctx.input_buffer_read_offset += bytes_to_read;
            stats_.xma_packets_decoded++;
            
            // Also copy to voice PCM buffer for audio output
            for (auto& voice : voices_) {
                if (voice.active && voice.context_index == i) {
                    u32 write_pos = voice.write_pos.load();
                    u32 voice_buffer_size = voice.pcm_buffer.size();
                    
                    if (voice_buffer_size > 0) {
                        // Copy stereo samples to voice buffer
                        u32 samples_to_copy = samples_decoded * 2;  // stereo
                        for (u32 j = 0; j < samples_to_copy && j < voice_buffer_size; j++) {
                            voice.pcm_buffer[(write_pos + j) % voice_buffer_size] = pcm_output[j];
                        }
                        voice.write_pos.store((write_pos + samples_to_copy) % voice_buffer_size);
                    }
                }
            }
            
            LOGD("Decoded %u samples from context %u", samples_decoded, i);
        }
    }
}

void Apu::mix_voices() {
    // Mix all active voices into output buffer
    // Process ~16ms worth of audio per call (one frame at 60fps)
    u32 mix_frames = config_.sample_rate / 60;
    u32 mix_samples = mix_frames * config_.channels;
    std::vector<f32> mix_buffer(mix_samples, 0.0f);
    
    u32 active_voice_count = 0;
    
    for (auto& voice : voices_) {
        if (!voice.active) continue;
        
        u32 voice_buffer_size = voice.pcm_buffer.size();
        if (voice_buffer_size == 0) continue;
        
        active_voice_count++;
        
        u32 read_pos = voice.read_pos.load();
        u32 write_pos = voice.write_pos.load();
        
        // Calculate available samples in voice buffer
        u32 available;
        if (write_pos >= read_pos) {
            available = write_pos - read_pos;
        } else {
            available = voice_buffer_size - read_pos + write_pos;
        }
        
        u32 samples_to_mix = std::min(mix_samples, available);
        
        // Mix voice into output buffer with volume
        for (u32 i = 0; i + 1 < samples_to_mix; i += 2) {
            u32 idx = (read_pos + i) % voice_buffer_size;
            s16 left = voice.pcm_buffer[idx];
            s16 right = voice.pcm_buffer[(idx + 1) % voice_buffer_size];
            
            // Apply per-voice volume and accumulate
            mix_buffer[i] += (left / 32768.0f) * voice.volume_left;
            mix_buffer[i + 1] += (right / 32768.0f) * voice.volume_right;
        }
        
        // Update voice read position
        voice.read_pos.store((read_pos + samples_to_mix) % voice_buffer_size);
    }
    
    stats_.active_voices = active_voice_count;
    
    // Convert mixed f32 to s16 and write to output buffer
    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        u32 write_pos = output_write_pos_.load();
        u32 buffer_size = output_buffer_.size();
        
        if (buffer_size == 0) return;
        
        for (u32 i = 0; i < mix_buffer.size(); i++) {
            // Soft clipping to prevent harsh distortion
            f32 sample = mix_buffer[i];
            if (sample > 1.0f) sample = 1.0f - 1.0f / (sample + 1.0f);
            else if (sample < -1.0f) sample = -1.0f + 1.0f / (-sample + 1.0f);
            
            output_buffer_[(write_pos + i) % buffer_size] = static_cast<s16>(sample * 32767.0f);
        }
        
        output_write_pos_ = (write_pos + mix_buffer.size()) % buffer_size;
        stats_.samples_generated += mix_buffer.size() / config_.channels;
        
        // Calculate buffer usage
        u32 read_pos = output_read_pos_.load();
        u32 used;
        if (output_write_pos_ >= read_pos) {
            used = output_write_pos_ - read_pos;
        } else {
            used = buffer_size - read_pos + output_write_pos_;
        }
        stats_.buffer_usage = static_cast<f32>(used) / buffer_size;
    }
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
