/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Android AAudio Output Implementation
 */

#include "android_audio.h"
#include <cstring>
#include <algorithm>
#include <cmath>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-audio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[AUDIO] " __VA_ARGS__); printf("\n")
#define LOGW(...) printf("[AUDIO WARN] " __VA_ARGS__); printf("\n")
#define LOGE(...) printf("[AUDIO ERROR] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#endif

namespace x360mu {

//=============================================================================
// AudioRingBuffer Implementation
//=============================================================================

AudioRingBuffer::AudioRingBuffer(u32 frame_count, u32 channels)
    : channels_(channels)
    , capacity_(frame_count)
    , read_pos_(0)
    , write_pos_(0) {
    buffer_.resize(frame_count * channels);
}

u32 AudioRingBuffer::write(const f32* data, u32 frame_count) {
    u32 available = available_write();
    u32 to_write = std::min(frame_count, available);
    
    if (to_write == 0) return 0;
    
    u32 write_idx = write_pos_.load(std::memory_order_relaxed);
    
    for (u32 i = 0; i < to_write; i++) {
        u32 idx = ((write_idx + i) % capacity_) * channels_;
        for (u32 c = 0; c < channels_; c++) {
            buffer_[idx + c] = data[i * channels_ + c];
        }
    }
    
    write_pos_.store((write_idx + to_write) % capacity_, std::memory_order_release);
    return to_write;
}

u32 AudioRingBuffer::read(f32* data, u32 frame_count) {
    u32 available = available_read();
    u32 to_read = std::min(frame_count, available);
    
    if (to_read == 0) return 0;
    
    u32 read_idx = read_pos_.load(std::memory_order_relaxed);
    
    for (u32 i = 0; i < to_read; i++) {
        u32 idx = ((read_idx + i) % capacity_) * channels_;
        for (u32 c = 0; c < channels_; c++) {
            data[i * channels_ + c] = buffer_[idx + c];
        }
    }
    
    read_pos_.store((read_idx + to_read) % capacity_, std::memory_order_release);
    return to_read;
}

u32 AudioRingBuffer::available_read() const {
    u32 write_idx = write_pos_.load(std::memory_order_acquire);
    u32 read_idx = read_pos_.load(std::memory_order_relaxed);
    
    if (write_idx >= read_idx) {
        return write_idx - read_idx;
    } else {
        return capacity_ - read_idx + write_idx;
    }
}

u32 AudioRingBuffer::available_write() const {
    return capacity_ - available_read() - 1;  // -1 to distinguish full from empty
}

void AudioRingBuffer::clear() {
    read_pos_.store(0, std::memory_order_release);
    write_pos_.store(0, std::memory_order_release);
}

//=============================================================================
// AndroidAudioOutput Implementation
//=============================================================================

AndroidAudioOutput::AndroidAudioOutput() = default;

AndroidAudioOutput::~AndroidAudioOutput() {
    shutdown();
}

Status AndroidAudioOutput::initialize(const AudioConfig& config) {
    if (initialized_) {
        shutdown();
    }
    
    config_ = config;
    
#ifdef __ANDROID__
    // Create AAudio stream builder
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    
    if (result != AAUDIO_OK) {
        LOGE("Failed to create AAudio stream builder: %d", result);
        return Status::Error;
    }
    
    // Configure the stream
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, config.channels);
    AAudioStreamBuilder_setSampleRate(builder, config.sample_rate);
    AAudioStreamBuilder_setFramesPerDataCallback(builder, config.buffer_frames);
    AAudioStreamBuilder_setBufferCapacityInFrames(builder, 
        config.buffer_frames * config.buffer_count);
    
    // Set callbacks
    AAudioStreamBuilder_setDataCallback(builder, audio_callback_static, this);
    AAudioStreamBuilder_setErrorCallback(builder, error_callback_static, this);
    
    // Open the stream
    result = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    
    if (result != AAUDIO_OK) {
        LOGE("Failed to open AAudio stream: %d", result);
        stream_ = nullptr;
        return Status::Error;
    }
    
    // Get actual sample rate (may differ from requested)
    actual_sample_rate_ = AAudioStream_getSampleRate(stream_);
    
    LOGI("AAudio stream opened: %d Hz, %d channels, buffer %d frames",
         actual_sample_rate_, AAudioStream_getChannelCount(stream_),
         AAudioStream_getBufferCapacityInFrames(stream_));
    
#else
    // Non-Android fallback - just store config
    actual_sample_rate_ = config.sample_rate;
    LOGI("Audio initialized (non-Android): %d Hz, %d channels",
         actual_sample_rate_, config.channels);
#endif
    
    // Create ring buffer
    u32 ring_size = config.buffer_frames * config.buffer_count * 2;
    ring_buffer_ = std::make_unique<AudioRingBuffer>(ring_size, config.channels);
    
    // Create temp buffer
    temp_buffer_.resize(config.buffer_frames * config.channels);
    
    initialized_ = true;
    return Status::Ok;
}

void AndroidAudioOutput::shutdown() {
    if (!initialized_) return;
    
    stop();
    
#ifdef __ANDROID__
    if (stream_) {
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }
#endif
    
    ring_buffer_.reset();
    temp_buffer_.clear();
    initialized_ = false;
    
    LOGI("Audio shutdown");
}

Status AndroidAudioOutput::start() {
    if (!initialized_) return Status::Error;
    
#ifdef __ANDROID__
    if (stream_) {
        aaudio_result_t result = AAudioStream_requestStart(stream_);
        if (result != AAUDIO_OK) {
            LOGE("Failed to start AAudio stream: %d", result);
            return Status::Error;
        }
    }
#endif
    
    playing_ = true;
    LOGI("Audio playback started");
    return Status::Ok;
}

void AndroidAudioOutput::stop() {
    if (!playing_) return;
    
#ifdef __ANDROID__
    if (stream_) {
        AAudioStream_requestStop(stream_);
        AAudioStream_waitForStateChange(stream_, AAUDIO_STREAM_STATE_STOPPING,
                                        nullptr, 1000000000);  // 1 second timeout
    }
#endif
    
    playing_ = false;
    if (ring_buffer_) {
        ring_buffer_->clear();
    }
    
    LOGI("Audio playback stopped");
}

void AndroidAudioOutput::pause() {
#ifdef __ANDROID__
    if (stream_ && playing_) {
        AAudioStream_requestPause(stream_);
    }
#endif
}

void AndroidAudioOutput::resume() {
#ifdef __ANDROID__
    if (stream_ && playing_) {
        AAudioStream_requestStart(stream_);
    }
#endif
}

void AndroidAudioOutput::set_callback(AudioCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_ = std::move(callback);
}

u32 AndroidAudioOutput::queue_samples(const f32* samples, u32 frame_count) {
    if (!ring_buffer_) return 0;
    return ring_buffer_->write(samples, frame_count);
}

f32 AndroidAudioOutput::get_latency_ms() const {
#ifdef __ANDROID__
    if (stream_) {
        int32_t buffer_size = AAudioStream_getBufferSizeInFrames(stream_);
        return (buffer_size * 1000.0f) / actual_sample_rate_;
    }
#endif
    return (config_.buffer_frames * config_.buffer_count * 1000.0f) / actual_sample_rate_;
}

void AndroidAudioOutput::set_volume(f32 volume) {
    volume_ = std::clamp(volume, 0.0f, 1.0f);
}

#ifdef __ANDROID__
aaudio_data_callback_result_t AndroidAudioOutput::audio_callback_static(
    AAudioStream* stream,
    void* user_data,
    void* audio_data,
    int32_t num_frames) {
    
    auto* self = static_cast<AndroidAudioOutput*>(user_data);
    return self->on_audio_callback(audio_data, num_frames);
}

void AndroidAudioOutput::error_callback_static(
    AAudioStream* stream,
    void* user_data,
    aaudio_result_t error) {
    
    auto* self = static_cast<AndroidAudioOutput*>(user_data);
    self->on_error(error);
}
#endif

aaudio_data_callback_result_t AndroidAudioOutput::on_audio_callback(
    void* audio_data, int32_t num_frames) {
    
    f32* output = static_cast<f32*>(audio_data);
    u32 frames_written = 0;
    
    // Try callback first
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (callback_) {
            frames_written = callback_(output, num_frames);
        }
    }
    
    // If callback didn't provide all frames, try ring buffer
    if (frames_written < static_cast<u32>(num_frames) && ring_buffer_) {
        u32 remaining = num_frames - frames_written;
        u32 from_buffer = ring_buffer_->read(
            output + frames_written * config_.channels, remaining);
        frames_written += from_buffer;
    }
    
    // Fill remaining with silence
    if (frames_written < static_cast<u32>(num_frames)) {
        u32 silence_frames = num_frames - frames_written;
        memset(output + frames_written * config_.channels, 0,
               silence_frames * config_.channels * sizeof(f32));
        
        if (playing_) {
            underrun_count_++;
        }
    }
    
    // Apply volume
    f32 vol = volume_.load(std::memory_order_relaxed);
    if (vol < 1.0f) {
        u32 sample_count = num_frames * config_.channels;
        for (u32 i = 0; i < sample_count; i++) {
            output[i] *= vol;
        }
    }
    
#ifdef __ANDROID__
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
#else
    return 0;
#endif
}

void AndroidAudioOutput::on_error(int32_t error) {
    LOGE("AAudio error: %d", error);
    
#ifdef __ANDROID__
    // Try to restart the stream on disconnect
    if (error == AAUDIO_ERROR_DISCONNECTED) {
        LOGW("Audio device disconnected, attempting restart...");
        
        // Stop and close the old stream
        if (stream_) {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
        
        // Reinitialize
        if (initialize(config_) == Status::Ok && playing_) {
            start();
        }
    }
#endif
}

//=============================================================================
// AudioResampler Implementation
//=============================================================================

AudioResampler::AudioResampler() = default;

void AudioResampler::configure(u32 input_rate, u32 output_rate, u32 channels) {
    input_rate_ = input_rate;
    output_rate_ = output_rate;
    channels_ = channels;
    
    prev_sample_.resize(channels, 0.0f);
    position_ = 0.0;
}

u32 AudioResampler::process(const f32* input, u32 input_frames,
                             f32* output, u32 max_output_frames) {
    if (input_rate_ == output_rate_) {
        // No resampling needed
        u32 frames = std::min(input_frames, max_output_frames);
        memcpy(output, input, frames * channels_ * sizeof(f32));
        return frames;
    }
    
    f64 ratio = static_cast<f64>(input_rate_) / output_rate_;
    u32 output_frames = 0;
    
    while (output_frames < max_output_frames) {
        u32 input_idx = static_cast<u32>(position_);
        
        if (input_idx >= input_frames) {
            break;
        }
        
        f64 frac = position_ - input_idx;
        
        // Linear interpolation
        for (u32 c = 0; c < channels_; c++) {
            f32 s0, s1;
            
            if (input_idx == 0) {
                s0 = prev_sample_[c];
            } else {
                s0 = input[(input_idx - 1) * channels_ + c];
            }
            
            s1 = input[input_idx * channels_ + c];
            
            output[output_frames * channels_ + c] = 
                static_cast<f32>(s0 + (s1 - s0) * frac);
        }
        
        output_frames++;
        position_ += ratio;
    }
    
    // Save last sample for next call
    if (input_frames > 0) {
        for (u32 c = 0; c < channels_; c++) {
            prev_sample_[c] = input[(input_frames - 1) * channels_ + c];
        }
    }
    
    // Adjust position for consumed input
    position_ -= input_frames;
    if (position_ < 0.0) {
        position_ = 0.0;
    }
    
    return output_frames;
}

void AudioResampler::reset() {
    position_ = 0.0;
    std::fill(prev_sample_.begin(), prev_sample_.end(), 0.0f);
}

u32 AudioResampler::get_output_frames(u32 input_frames) const {
    if (input_rate_ == output_rate_) {
        return input_frames;
    }
    
    return static_cast<u32>(
        (static_cast<f64>(input_frames) * output_rate_) / input_rate_ + 0.5);
}

//=============================================================================
// SimpleAudioMixer Implementation
//=============================================================================

SimpleAudioMixer::SimpleAudioMixer() {
    for (auto& source : sources_) {
        source = {};
    }
}

void SimpleAudioMixer::configure(u32 sample_rate, u32 channels) {
    sample_rate_ = sample_rate;
    channels_ = channels;
}

int SimpleAudioMixer::add_source(const f32* samples, u32 frame_count, f32 volume, bool loop) {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    
    for (u32 i = 0; i < MAX_SOURCES; i++) {
        if (!sources_[i].active) {
            sources_[i].active = true;
            sources_[i].samples = samples;
            sources_[i].frame_count = frame_count;
            sources_[i].volume = volume;
            sources_[i].pan = 0.0f;
            sources_[i].position = 0;
            sources_[i].loop = loop;
            return static_cast<int>(i);
        }
    }
    
    return -1;  // No free slots
}

void SimpleAudioMixer::remove_source(int index) {
    if (index < 0 || index >= static_cast<int>(MAX_SOURCES)) return;
    
    std::lock_guard<std::mutex> lock(sources_mutex_);
    sources_[index].active = false;
}

void SimpleAudioMixer::set_source_volume(int index, f32 volume) {
    if (index < 0 || index >= static_cast<int>(MAX_SOURCES)) return;
    
    std::lock_guard<std::mutex> lock(sources_mutex_);
    sources_[index].volume = std::clamp(volume, 0.0f, 2.0f);
}

void SimpleAudioMixer::set_source_pan(int index, f32 pan) {
    if (index < 0 || index >= static_cast<int>(MAX_SOURCES)) return;
    
    std::lock_guard<std::mutex> lock(sources_mutex_);
    sources_[index].pan = std::clamp(pan, -1.0f, 1.0f);
}

void SimpleAudioMixer::mix(f32* output, u32 frame_count) {
    // Clear output
    memset(output, 0, frame_count * channels_ * sizeof(f32));
    
    std::lock_guard<std::mutex> lock(sources_mutex_);
    
    for (auto& source : sources_) {
        if (!source.active || !source.samples) continue;
        
        for (u32 i = 0; i < frame_count; i++) {
            if (source.position >= source.frame_count) {
                if (source.loop) {
                    source.position = 0;
                } else {
                    source.active = false;
                    break;
                }
            }
            
            // Get source sample (assume stereo)
            f32 left = source.samples[source.position * 2];
            f32 right = source.samples[source.position * 2 + 1];
            
            // Apply pan (constant power)
            f32 pan_angle = (source.pan + 1.0f) * 0.25f * 3.14159f;  // 0 to PI/2
            f32 pan_left = std::cos(pan_angle);
            f32 pan_right = std::sin(pan_angle);
            
            // Apply volume and accumulate
            f32 vol = source.volume;
            output[i * 2] += left * vol * pan_left;
            output[i * 2 + 1] += right * vol * pan_right;
            
            source.position++;
        }
    }
    
    // Apply master volume and clamp
    for (u32 i = 0; i < frame_count * channels_; i++) {
        output[i] = std::clamp(output[i] * master_volume_, -1.0f, 1.0f);
    }
}

} // namespace x360mu

