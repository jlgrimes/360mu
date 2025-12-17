/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XMA Audio Decoder
 * 
 * Xbox Media Audio (XMA) is Xbox 360's proprietary audio codec.
 * This decoder converts XMA streams to PCM for playback on Android.
 */

#pragma once

#include "x360mu/types.h"
#include <vector>
#include <memory>
#include <queue>
#include <mutex>
#include <atomic>
#include <array>

namespace x360mu {

// Forward declarations
class Memory;
class AudioMixer;

/**
 * XMA frame header
 */
struct XmaFrameHeader {
    u16 frame_length;      // Length of frame data in bits
    bool skip_bits;        // Has skip bits at start
    u8 skip_bits_count;    // Number of bits to skip
    bool output_pcm;       // Frame produces PCM output
    u16 loop_start;        // Loop start sub-frame
    u16 loop_end;          // Loop end sub-frame
    u8 num_subframes;      // Number of sub-frames in frame
};

// Forward declaration for FFmpeg decoder
#ifdef X360MU_USE_FFMPEG
class FFmpegXmaDecoder;
#endif

/**
 * XMA stream context (one per channel pair)
 */
struct XmaContext {
    // Stream configuration
    u32 sample_rate;
    u32 num_channels;      // 1 or 2
    u32 bits_per_sample;   // Always 16 for XMA
    
    // Buffer pointers (in Xbox 360 memory)
    GuestAddr input_buffer_0;
    GuestAddr input_buffer_1;
    u32 input_buffer_0_size;
    u32 input_buffer_1_size;
    
    GuestAddr output_buffer;
    u32 output_buffer_size;
    
    // State
    bool active;
    u32 input_buffer_index;     // Which input buffer is current
    u32 input_buffer_read_offset;
    u32 output_buffer_write_offset;
    bool loop_enabled;
    u32 loop_count;
    u32 loop_start_offset;
    u32 loop_end_offset;
    
    // Decoder state (fallback software decoder)
    std::array<s16, 2048> history;
    u32 history_index;
    std::array<f32, 128> predictor_coefs;
    
    // FFmpeg decoder (when available)
#ifdef X360MU_USE_FFMPEG
    std::unique_ptr<FFmpegXmaDecoder> ffmpeg_decoder;
#endif
    
    // Statistics
    u32 samples_decoded;
    u32 frames_decoded;
};

/**
 * XMA sub-frame packet
 */
struct XmaSubframe {
    static constexpr u32 SAMPLES_PER_SUBFRAME = 128;
    std::array<s16, SAMPLES_PER_SUBFRAME * 2> samples; // Stereo
};

/**
 * XMA Decoder
 * 
 * Decodes Xbox 360 XMA audio to PCM.
 * XMA is based on WMA Pro but with custom extensions for games.
 */
class XmaDecoder {
public:
    XmaDecoder();
    ~XmaDecoder();
    
    /**
     * Initialize decoder
     */
    Status initialize();
    
    /**
     * Shutdown decoder
     */
    void shutdown();
    
    /**
     * Create a new XMA decoding context
     */
    u32 create_context(u32 sample_rate, u32 num_channels);
    
    /**
     * Destroy a context
     */
    void destroy_context(u32 context_id);
    
    /**
     * Set input buffer for a context
     */
    void set_input_buffer(u32 context_id, GuestAddr buffer, u32 size, u32 buffer_index);
    
    /**
     * Set output buffer for a context
     */
    void set_output_buffer(u32 context_id, GuestAddr buffer, u32 size);
    
    /**
     * Start decoding
     */
    void start_context(u32 context_id);
    
    /**
     * Stop decoding
     */
    void stop_context(u32 context_id);
    
    /**
     * Check if context has finished current buffer
     */
    bool is_buffer_done(u32 context_id, u32 buffer_index) const;
    
    /**
     * Get number of samples decoded
     */
    u32 get_samples_decoded(u32 context_id) const;
    
    /**
     * Process all active contexts (call from audio thread)
     */
    void process(class Memory* memory);
    
    /**
     * Decode XMA data directly (for testing)
     */
    std::vector<s16> decode(const u8* data, u32 size, u32 sample_rate, u32 num_channels);
    
private:
    static constexpr u32 MAX_CONTEXTS = 256;
    
    std::array<std::unique_ptr<XmaContext>, MAX_CONTEXTS> contexts_;
    std::mutex contexts_mutex_;
    std::atomic<bool> running_{false};
    
    // Bitstream reader for XMA frames
    class BitReader {
    public:
        BitReader(const u8* data, u32 size);
        
        u32 read_bits(u32 count);
        s32 read_signed_bits(u32 count);
        void skip_bits(u32 count);
        bool has_bits(u32 count) const;
        u32 position() const { return bit_position_; }
        void seek(u32 bit_offset);
        
    private:
        const u8* data_;
        u32 size_;
        u32 bit_position_;
    };
    
    // Decode a single XMA frame
    bool decode_frame(XmaContext& ctx, BitReader& reader, s16* output, u32& samples_written);
    
    // Decode a sub-frame
    bool decode_subframe(XmaContext& ctx, BitReader& reader, s16* output);
    
    // Apply predictor filter
    void apply_predictor(XmaContext& ctx, f32* samples, u32 count);
    
    // Quantization tables
    static const f32 quantization_table[];
    static const f32 scale_factor_table[];
    static const s16 huffman_table[];
};

/**
 * XMA Processor
 * 
 * Manages multiple XMA hardware contexts for parallel audio decoding.
 * Xbox 360 games typically use multiple XMA contexts for different audio streams.
 */
class XmaProcessor {
public:
    XmaProcessor();
    ~XmaProcessor();
    
    /**
     * Initialize the XMA processor
     */
    Status initialize(class Memory* memory, class AudioMixer* mixer);
    
    /**
     * Shutdown and release all resources
     */
    void shutdown();
    
    /**
     * Create a new XMA hardware context
     * Returns context ID or UINT32_MAX on failure
     */
    u32 create_context();
    
    /**
     * Destroy an XMA context
     */
    void destroy_context(u32 context_id);
    
    /**
     * Get context for modification
     */
    XmaContext* get_context(u32 context_id);
    
    /**
     * Get context (const)
     */
    const XmaContext* get_context(u32 context_id) const;
    
    /**
     * Set input buffer for a context (double buffering supported)
     */
    void set_input_buffer(u32 context_id, GuestAddr buffer, u32 size, u32 buffer_index);
    
    /**
     * Set output buffer for a context
     */
    void set_output_buffer(u32 context_id, GuestAddr buffer, u32 size);
    
    /**
     * Configure context parameters
     */
    void set_context_sample_rate(u32 context_id, u32 sample_rate);
    void set_context_channels(u32 context_id, u32 num_channels);
    void set_context_loop(u32 context_id, bool enabled, u32 loop_start, u32 loop_end);
    
    /**
     * Enable (start) context decoding
     */
    void enable_context(u32 context_id);
    
    /**
     * Disable (stop) context decoding
     */
    void disable_context(u32 context_id);
    
    /**
     * Check if context is active
     */
    bool is_context_active(u32 context_id) const;
    
    /**
     * Check if input buffer has been consumed
     */
    bool is_input_buffer_consumed(u32 context_id, u32 buffer_index) const;
    
    /**
     * Get current output write position
     */
    u32 get_output_write_offset(u32 context_id) const;
    
    /**
     * Process all active contexts
     * Should be called regularly from the emulation loop
     */
    void process();
    
    /**
     * Process a specific number of packets for a context
     */
    u32 process_context(u32 context_id, u32 max_packets);
    
    /**
     * Get statistics
     */
    struct Stats {
        u32 active_contexts;
        u64 total_packets_decoded;
        u64 total_samples_decoded;
        u32 decode_errors;
    };
    Stats get_stats() const;
    
private:
    static constexpr u32 MAX_CONTEXTS = 256;
    static constexpr u32 XMA_PACKET_SIZE = 2048;
    
    struct HardwareContext {
        std::unique_ptr<XmaContext> ctx;
        std::unique_ptr<XmaDecoder> decoder;
        u32 voice_id;           // Associated mixer voice
        bool buffer_0_consumed;
        bool buffer_1_consumed;
    };
    
    std::array<std::unique_ptr<HardwareContext>, MAX_CONTEXTS> contexts_;
    std::mutex contexts_mutex_;
    
    Memory* memory_ = nullptr;
    AudioMixer* mixer_ = nullptr;
    
    std::atomic<bool> running_{false};
    u32 next_context_id_ = 0;
    
    // Statistics
    Stats stats_{};
    
    // Decode a single packet from context
    u32 decode_packet(HardwareContext& hw_ctx, u32 context_id);
    
    // Read XMA packet from guest memory
    bool read_xma_packet(HardwareContext& hw_ctx, u8* packet_data);
    
    // Write decoded PCM to output buffer
    void write_pcm_output(HardwareContext& hw_ctx, const s16* pcm_data, u32 sample_count);
};

/**
 * Audio mixer
 * 
 * Mixes multiple audio streams and outputs to Android audio device.
 */
class AudioMixer {
public:
    AudioMixer();
    ~AudioMixer();
    
    /**
     * Initialize mixer with sample rate and buffer size
     */
    Status initialize(u32 sample_rate, u32 buffer_frames);
    
    /**
     * Shutdown mixer
     */
    void shutdown();
    
    /**
     * Create a voice for mixing
     */
    u32 create_voice(u32 sample_rate, u32 num_channels);
    
    /**
     * Destroy a voice
     */
    void destroy_voice(u32 voice_id);
    
    /**
     * Submit samples to a voice
     */
    void submit_samples(u32 voice_id, const s16* samples, u32 sample_count);
    
    /**
     * Set voice volume (0.0 - 1.0)
     */
    void set_voice_volume(u32 voice_id, f32 volume);
    
    /**
     * Set voice pan (-1.0 left to 1.0 right)
     */
    void set_voice_pan(u32 voice_id, f32 pan);
    
    /**
     * Set master volume
     */
    void set_master_volume(f32 volume);
    
    /**
     * Get mixed output (call from audio callback)
     */
    u32 get_output(s16* output, u32 frame_count);
    
    /**
     * Pause/resume all audio
     */
    void pause();
    void resume();
    
    /**
     * Get current audio latency in samples
     */
    u32 get_latency() const;
    
private:
    struct Voice {
        u32 sample_rate;
        u32 num_channels;
        f32 volume;
        f32 pan;
        bool active;
        
        // Ring buffer for samples
        std::vector<s16> buffer;
        std::atomic<u32> read_pos{0};
        std::atomic<u32> write_pos{0};
        
        // Resampling state
        f32 sample_position;
        f32 sample_increment;
    };
    
    static constexpr u32 MAX_VOICES = 64;
    
    std::array<std::unique_ptr<Voice>, MAX_VOICES> voices_;
    std::mutex voices_mutex_;
    
    u32 output_sample_rate_ = 48000;
    u32 buffer_frames_ = 1024;
    f32 master_volume_ = 1.0f;
    std::atomic<bool> paused_{false};
    
    // Mix buffer
    std::vector<f32> mix_buffer_;
    
    // Resample voice to output sample rate
    void resample_voice(Voice& voice, f32* output, u32 frame_count);
};

} // namespace x360mu

