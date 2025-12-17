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

namespace x360mu {

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
    
    // Decoder state
    std::array<s16, 2048> history;
    u32 history_index;
    std::array<f32, 128> predictor_coefs;
    
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

/**
 * Audio Processing Unit (APU) emulation
 * 
 * Coordinates XMA decoding and audio mixing for Xbox 360 audio.
 */
class Apu {
public:
    Apu();
    ~Apu();
    
    /**
     * Initialize APU
     */
    Status initialize(Memory* memory);
    
    /**
     * Shutdown APU
     */
    void shutdown();
    
    /**
     * Process audio (call from audio thread)
     */
    void process();
    
    /**
     * Get mixed audio output
     */
    u32 get_output(s16* output, u32 frame_count);
    
    // Component access
    XmaDecoder& xma_decoder() { return xma_decoder_; }
    AudioMixer& mixer() { return mixer_; }
    
    /**
     * Write to APU register (MMIO)
     */
    void write_register(u32 offset, u32 value);
    
    /**
     * Read from APU register (MMIO)
     */
    u32 read_register(u32 offset) const;
    
private:
    Memory* memory_ = nullptr;
    XmaDecoder xma_decoder_;
    AudioMixer mixer_;
    
    // APU registers
    std::array<u32, 256> registers_;
    
    // Interrupt handling
    u32 interrupt_status_ = 0;
    u32 interrupt_mask_ = 0;
    
    std::atomic<bool> running_{false};
};

} // namespace x360mu

