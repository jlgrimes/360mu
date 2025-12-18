/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Audio Processing Unit (APU) emulation
 */

#pragma once

#include "x360mu/types.h"
#include <memory>
#include <vector>
#include <array>
#include <atomic>
#include <mutex>

namespace x360mu {

class Memory;
class AndroidAudioOutput;

/**
 * APU configuration
 */
struct ApuConfig {
    u32 buffer_size_ms = 20;
    u32 sample_rate = 48000;
    u32 channels = 2;
};

/**
 * XMA packet header
 */
struct XmaPacketHeader {
    u32 frame_count : 6;
    u32 unknown : 2;
    u32 skip_samples : 15;
    u32 metadata : 3;
    u32 packet_skip_count : 6;
};

/**
 * APU XMA context (per-voice state for APU internal use)
 */
struct ApuXmaContext {
    // Control registers
    u32 input_buffer_ptr;
    u32 input_buffer_read_offset;
    u32 input_buffer_write_offset;
    u32 output_buffer_ptr;
    u32 output_buffer_read_offset;
    u32 output_buffer_write_offset;
    
    // State
    bool valid;
    bool loop;
    bool error;
    u32 loop_count;
    u32 loop_start;
    u32 loop_end;
    
    // Decoder state (internal)
    void* decoder_state;
};

/**
 * Audio voice
 */
struct AudioVoice {
    bool active;
    u32 context_index;
    f32 volume_left;
    f32 volume_right;
    f32 pitch;
    
    // Ring buffer for decoded PCM
    std::vector<s16> pcm_buffer;
    std::atomic<u32> read_pos{0};
    std::atomic<u32> write_pos{0};
};

/**
 * APU emulation class
 */
class Apu {
public:
    Apu();
    ~Apu();
    
    /**
     * Initialize audio subsystem
     */
    Status initialize(Memory* memory, const ApuConfig& config);
    
    /**
     * Shutdown and release resources
     */
    void shutdown();
    
    /**
     * Reset audio state
     */
    void reset();
    
    /**
     * Process audio (called from emulation loop)
     */
    void process();
    
    /**
     * XMA context management
     */
    Status create_context(u32 index, const ApuXmaContext& ctx);
    void destroy_context(u32 index);
    ApuXmaContext* get_context(u32 index);
    
    /**
     * Voice management
     */
    u32 create_voice(u32 context_index);
    void destroy_voice(u32 voice_id);
    void set_voice_volume(u32 voice_id, f32 left, f32 right);
    void set_voice_pitch(u32 voice_id, f32 pitch);
    void start_voice(u32 voice_id);
    void stop_voice(u32 voice_id);
    
    /**
     * Register read/write (for MMIO)
     */
    u32 read_register(u32 offset);
    void write_register(u32 offset, u32 value);
    
    /**
     * Get output samples for playback
     * Returns number of samples written
     */
    u32 get_samples(s16* buffer, u32 sample_count);
    
    // Statistics
    struct Stats {
        u64 samples_generated;
        u64 xma_packets_decoded;
        u32 active_voices;
        f32 buffer_usage; // 0.0 - 1.0
    };
    Stats get_stats() const { return stats_; }
    
private:
    Memory* memory_ = nullptr;
    ApuConfig config_;
    
    // XMA contexts (up to 256)
    std::array<ApuXmaContext, 256> xma_contexts_;
    
    // Audio voices (up to 256)
    std::array<AudioVoice, 256> voices_;
    
    // Mixed output buffer
    std::vector<s16> output_buffer_;
    std::atomic<u32> output_read_pos_{0};
    std::atomic<u32> output_write_pos_{0};
    std::mutex output_mutex_;
    
    // XMA decoder
    class XmaDecoder;
    std::unique_ptr<XmaDecoder> xma_decoder_;
    
    // Audio output (platform-specific legacy interface)
    class AudioOutput;
    std::unique_ptr<AudioOutput> audio_output_;
    
    // Android audio output (real AAudio connection - from android_audio.h)
    std::unique_ptr<class ::x360mu::AndroidAudioOutput> android_audio_;
    
    // Statistics
    Stats stats_{};
    
    // Processing
    void decode_xma_packets();
    void mix_voices();
    void submit_to_output();
    
    // Audio callback for AndroidAudioOutput
    u32 audio_callback(f32* output, u32 frame_count);
};

/**
 * XMA decoder (wraps FFmpeg or custom implementation)
 */
class Apu::XmaDecoder {
public:
    XmaDecoder();
    ~XmaDecoder();
    
    Status initialize();
    void shutdown();
    
    /**
     * Decode XMA packet to PCM
     * 
     * @param input XMA packet data
     * @param input_size Size of input data
     * @param output Output buffer for PCM samples
     * @param output_size Size of output buffer (in samples)
     * @param samples_decoded Number of samples actually decoded
     * @return Status
     */
    Status decode(
        const void* input, u32 input_size,
        s16* output, u32 output_size,
        u32& samples_decoded
    );
    
    /**
     * Reset decoder state for new stream
     */
    void reset_state(void* context);
    
private:
#ifdef X360MU_USE_FFMPEG
    void* av_codec_context_ = nullptr;
    void* av_frame_ = nullptr;
    void* av_packet_ = nullptr;
#endif
};

/**
 * Platform-specific audio output
 */
class Apu::AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    
    Status initialize(const ApuConfig& config);
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
     * Queue samples for playback
     */
    Status queue_samples(const s16* samples, u32 count);
    
    /**
     * Get number of samples that can be queued
     */
    u32 get_available_space() const;
    
    /**
     * Is audio playing?
     */
    bool is_playing() const { return playing_; }
    
private:
    ApuConfig config_;
    bool playing_ = false;
    
#ifdef __ANDROID__
    // AAudio implementation
    void* aaudio_stream_ = nullptr;
#endif
};

} // namespace x360mu

