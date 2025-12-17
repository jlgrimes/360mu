/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Android AAudio Output
 * 
 * High-performance audio output using Android's AAudio API.
 * AAudio provides the lowest latency audio path on Android.
 */

#pragma once

#include "x360mu/types.h"
#include <atomic>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#endif

namespace x360mu {

/**
 * Audio format configuration
 */
struct AudioConfig {
    u32 sample_rate = 48000;
    u32 channels = 2;           // Stereo
    u32 buffer_frames = 256;    // Frames per buffer (affects latency)
    u32 buffer_count = 4;       // Number of buffers
};

/**
 * Audio callback function type
 * Called when audio output needs more samples
 * Returns number of frames written
 */
using AudioCallback = std::function<u32(f32* output, u32 frame_count)>;

/**
 * Ring buffer for audio samples
 */
class AudioRingBuffer {
public:
    AudioRingBuffer(u32 frame_count, u32 channels);
    ~AudioRingBuffer() = default;
    
    /**
     * Write samples to buffer
     * Returns number of frames actually written
     */
    u32 write(const f32* data, u32 frame_count);
    
    /**
     * Read samples from buffer
     * Returns number of frames actually read
     */
    u32 read(f32* data, u32 frame_count);
    
    /**
     * Get available frames to read
     */
    u32 available_read() const;
    
    /**
     * Get available frames to write
     */
    u32 available_write() const;
    
    /**
     * Clear the buffer
     */
    void clear();
    
private:
    std::vector<f32> buffer_;
    u32 channels_;
    u32 capacity_;  // In frames
    std::atomic<u32> read_pos_;
    std::atomic<u32> write_pos_;
};

/**
 * Android AAudio Output
 * 
 * Uses AAudio for low-latency audio playback on Android.
 * Falls back to OpenSL ES on older devices if needed.
 */
class AndroidAudioOutput {
public:
    AndroidAudioOutput();
    ~AndroidAudioOutput();
    
    /**
     * Initialize audio output
     */
    Status initialize(const AudioConfig& config);
    
    /**
     * Shutdown audio output
     */
    void shutdown();
    
    /**
     * Start audio playback
     */
    Status start();
    
    /**
     * Stop audio playback
     */
    void stop();
    
    /**
     * Pause audio playback
     */
    void pause();
    
    /**
     * Resume audio playback
     */
    void resume();
    
    /**
     * Check if audio is playing
     */
    bool is_playing() const { return playing_; }
    
    /**
     * Set audio callback
     * The callback is called from the audio thread
     */
    void set_callback(AudioCallback callback);
    
    /**
     * Queue samples directly (alternative to callback)
     */
    u32 queue_samples(const f32* samples, u32 frame_count);
    
    /**
     * Get current latency in milliseconds
     */
    f32 get_latency_ms() const;
    
    /**
     * Get actual sample rate (may differ from requested)
     */
    u32 get_actual_sample_rate() const { return actual_sample_rate_; }
    
    /**
     * Get underrun count (audio stutters)
     */
    u32 get_underrun_count() const { return underrun_count_; }
    
    /**
     * Set volume (0.0 - 1.0)
     */
    void set_volume(f32 volume);
    
    /**
     * Get current volume
     */
    f32 get_volume() const { return volume_; }
    
private:
#ifdef __ANDROID__
    // AAudio stream
    AAudioStream* stream_ = nullptr;
    
    // AAudio callback (static, calls member function)
    static aaudio_data_callback_result_t audio_callback_static(
        AAudioStream* stream,
        void* user_data,
        void* audio_data,
        int32_t num_frames);
    
    // Error callback
    static void error_callback_static(
        AAudioStream* stream,
        void* user_data,
        aaudio_result_t error);
#endif
    
    // Audio callback handler
    aaudio_data_callback_result_t on_audio_callback(void* audio_data, int32_t num_frames);
    
    // Error handler
    void on_error(int32_t error);
    
    // Configuration
    AudioConfig config_;
    u32 actual_sample_rate_ = 0;
    
    // Ring buffer for queued samples
    std::unique_ptr<AudioRingBuffer> ring_buffer_;
    
    // User callback
    AudioCallback callback_;
    std::mutex callback_mutex_;
    
    // State
    std::atomic<bool> playing_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<f32> volume_{1.0f};
    std::atomic<u32> underrun_count_{0};
    
    // Temporary buffer for callback
    std::vector<f32> temp_buffer_;
};

/**
 * Audio Resampler
 * 
 * Converts between sample rates using linear interpolation.
 * Used when emulated audio rate doesn't match device rate.
 */
class AudioResampler {
public:
    AudioResampler();
    ~AudioResampler() = default;
    
    /**
     * Configure resampler
     */
    void configure(u32 input_rate, u32 output_rate, u32 channels);
    
    /**
     * Process samples
     * Returns number of output frames written
     */
    u32 process(const f32* input, u32 input_frames,
                f32* output, u32 max_output_frames);
    
    /**
     * Reset resampler state
     */
    void reset();
    
    /**
     * Get number of output frames for given input frames
     */
    u32 get_output_frames(u32 input_frames) const;
    
private:
    u32 input_rate_ = 48000;
    u32 output_rate_ = 48000;
    u32 channels_ = 2;
    
    // Fractional position for interpolation
    f64 position_ = 0.0;
    
    // Previous sample for interpolation
    std::vector<f32> prev_sample_;
};

/**
 * Audio Mixer
 * 
 * Mixes multiple audio sources into a single output.
 */
class AudioMixer {
public:
    static constexpr u32 MAX_SOURCES = 16;
    
    /**
     * Audio source info
     */
    struct Source {
        bool active = false;
        f32 volume = 1.0f;
        f32 pan = 0.0f;  // -1.0 left, 0.0 center, 1.0 right
        const f32* samples = nullptr;
        u32 frame_count = 0;
        u32 position = 0;
        bool loop = false;
    };
    
    AudioMixer();
    ~AudioMixer() = default;
    
    /**
     * Configure mixer
     */
    void configure(u32 sample_rate, u32 channels);
    
    /**
     * Add a source
     * Returns source index, or -1 if full
     */
    int add_source(const f32* samples, u32 frame_count, f32 volume = 1.0f, bool loop = false);
    
    /**
     * Remove a source
     */
    void remove_source(int index);
    
    /**
     * Set source volume
     */
    void set_source_volume(int index, f32 volume);
    
    /**
     * Set source pan
     */
    void set_source_pan(int index, f32 pan);
    
    /**
     * Mix all active sources
     */
    void mix(f32* output, u32 frame_count);
    
    /**
     * Set master volume
     */
    void set_master_volume(f32 volume) { master_volume_ = volume; }
    
    /**
     * Get master volume
     */
    f32 get_master_volume() const { return master_volume_; }
    
private:
    std::array<Source, MAX_SOURCES> sources_;
    std::mutex sources_mutex_;
    
    u32 sample_rate_ = 48000;
    u32 channels_ = 2;
    f32 master_volume_ = 1.0f;
};

} // namespace x360mu

