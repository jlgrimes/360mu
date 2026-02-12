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
#include <functional>

namespace x360mu {

class Memory;
class AndroidAudioOutput;

/**
 * Xbox 360 APU MMIO register offsets
 * Base address: 0x7FEA0000
 */
namespace apu_reg {
    // APU base physical address
    constexpr GuestAddr APU_BASE = 0x7FEA0000;
    constexpr u64 APU_SIZE = 0x10000;  // 64KB register space

    // XMA context array pointer (games write guest address of context array here)
    constexpr u32 XMA_CONTEXT_ARRAY_PTR    = 0x0000;

    // XMA context enable bitmask (256 contexts = 8 x 32-bit words)
    constexpr u32 XMA_CONTEXT_ENABLE_0     = 0x0004;
    constexpr u32 XMA_CONTEXT_ENABLE_1     = 0x0008;
    constexpr u32 XMA_CONTEXT_ENABLE_2     = 0x000C;
    constexpr u32 XMA_CONTEXT_ENABLE_3     = 0x0010;
    constexpr u32 XMA_CONTEXT_ENABLE_4     = 0x0014;
    constexpr u32 XMA_CONTEXT_ENABLE_5     = 0x0018;
    constexpr u32 XMA_CONTEXT_ENABLE_6     = 0x001C;
    constexpr u32 XMA_CONTEXT_ENABLE_7     = 0x0020;

    // XMA context done/completion bitmask (set by hardware when input consumed)
    constexpr u32 XMA_CONTEXT_DONE_0       = 0x0024;
    constexpr u32 XMA_CONTEXT_DONE_1       = 0x0028;
    constexpr u32 XMA_CONTEXT_DONE_2       = 0x002C;
    constexpr u32 XMA_CONTEXT_DONE_3       = 0x0030;
    constexpr u32 XMA_CONTEXT_DONE_4       = 0x0034;
    constexpr u32 XMA_CONTEXT_DONE_5       = 0x0038;
    constexpr u32 XMA_CONTEXT_DONE_6       = 0x003C;
    constexpr u32 XMA_CONTEXT_DONE_7       = 0x0040;

    // XMA context clear (write 1 bits to clear corresponding done bits)
    constexpr u32 XMA_CONTEXT_CLEAR_0      = 0x0044;

    // XMA kick register (write to trigger processing)
    constexpr u32 XMA_CONTEXT_KICK         = 0x0064;

    // XMA context lock bitmask (prevents hardware from modifying contexts)
    constexpr u32 XMA_CONTEXT_LOCK_0       = 0x0068;
    constexpr u32 XMA_CONTEXT_LOCK_1       = 0x006C;
    constexpr u32 XMA_CONTEXT_LOCK_2       = 0x0070;
    constexpr u32 XMA_CONTEXT_LOCK_3       = 0x0074;
    constexpr u32 XMA_CONTEXT_LOCK_4       = 0x0078;
    constexpr u32 XMA_CONTEXT_LOCK_5       = 0x007C;
    constexpr u32 XMA_CONTEXT_LOCK_6       = 0x0080;
    constexpr u32 XMA_CONTEXT_LOCK_7       = 0x0084;

    // Interrupt control
    constexpr u32 XMA_INTERRUPT_STATUS      = 0x0088;
    constexpr u32 XMA_INTERRUPT_MASK        = 0x008C;

    // Global XMA control
    constexpr u32 XMA_CONTROL              = 0x0090;

    // Size of one XMA hardware context in guest memory (64 bytes)
    constexpr u32 XMA_HW_CONTEXT_SIZE      = 64;
}

/**
 * XMA hardware context structure as stored in guest memory (64 bytes)
 * Games allocate an array of these and point XMA_CONTEXT_ARRAY_PTR at it.
 */
struct XmaHwContext {
    u32 input_buffer_0_ptr;           // 0x00: Guest addr of first input buffer
    u32 input_buffer_0_packet_count;  // 0x04: Number of 2048-byte packets in buffer 0
    u32 input_buffer_1_ptr;           // 0x08: Guest addr of second input buffer
    u32 input_buffer_1_packet_count;  // 0x0C: Number of 2048-byte packets in buffer 1
    u32 input_buffer_read_offset;     // 0x10: Current read position (in bits)
    u32 output_buffer_ptr;            // 0x14: Guest addr of output PCM buffer
    u32 output_buffer_block_count;    // 0x18: Output buffer size in 256-sample blocks
    u32 output_buffer_write_offset;   // 0x1C: Current write position (in bytes)
    u32 loop_subframe_end;            // 0x20: Loop end position
    u32 loop_subframe_skip;           // 0x24: Samples to skip at loop point
    u32 subframe_decode_count;        // 0x28: Total subframes decoded
    u32 subframe_skip_count;          // 0x2C: Subframes to skip (for seek)
    u32 sample_rate;                  // 0x30: 0=24kHz, 1=32kHz, 2=44.1kHz, 3=48kHz
    u32 loop_count;                   // 0x34: Remaining loop iterations
    u32 error_status;                 // 0x38: Error flags
    u32 parser_state;                 // 0x3C: Internal parser state
};

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
 * Audio codec types supported by Xbox 360 APU
 */
enum class AudioCodec : u32 {
    XMA = 0,        // Xbox Media Audio (default)
    PCM_U8 = 1,     // 8-bit unsigned PCM
    PCM_S16BE = 2,  // 16-bit signed PCM (big-endian in guest memory)
    PCM_F32 = 3,    // 32-bit IEEE float PCM
    XWMA = 4,       // Xbox WMA (Windows Media Audio variant)
    Unknown = 0xFF
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

    // Audio format
    AudioCodec codec = AudioCodec::XMA;  // Codec type for this context
    u32 sample_rate;        // Context sample rate in Hz (24000/32000/44100/48000)
    u32 num_channels;       // 1 (mono) or 2 (stereo)
    u32 bits_per_sample;    // 8, 16, or 32

    // State
    bool valid;
    bool loop;
    bool error;
    u32 loop_count;
    u32 loop_start;
    u32 loop_end;

    // Decoder state (internal)
    void* decoder_state;

    // XWMA-specific: seek table pointer and entry count
    u32 xwma_seek_table_ptr;
    u32 xwma_seek_table_entries;
    u32 xwma_block_align;       // WMA block alignment
    u32 xwma_avg_bytes_per_sec; // Average bitrate
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
    f32 pan;                // -1.0 (full left) to 1.0 (full right), 0.0 = center
    u32 priority;           // 0 = highest priority, 255 = lowest
    u32 source_sample_rate; // Sample rate of source audio (may differ from output)
    u32 num_channels;       // 1 = mono, 2 = stereo

    // Resampling state (for non-48kHz sources)
    f64 resample_position;  // Fractional sample position for interpolation
    f64 resample_ratio;     // source_rate / output_rate

    // Ring buffer for decoded PCM
    std::vector<s16> pcm_buffer;
    std::atomic<u32> read_pos{0};
    std::atomic<u32> write_pos{0};
};

// Maximum voices hardware-mixed simultaneously (Xbox 360 limit)
static constexpr u32 MAX_HW_MIXED_VOICES = 64;

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
     * Register MMIO handlers with memory subsystem
     */
    void register_mmio(Memory* memory);

    /**
     * Get output samples for playback
     * Returns number of samples written
     */
    u32 get_samples(s16* buffer, u32 sample_count);

    /**
     * Set interrupt callback (called when XMA contexts complete)
     */
    using InterruptCallback = std::function<void()>;
    void set_interrupt_callback(InterruptCallback cb) { interrupt_callback_ = std::move(cb); }

    /**
     * Advance audio timing by CPU cycles elapsed
     * Call from emulation loop after executing CPU instructions
     */
    void advance_cpu_cycles(u64 cycles);

    /**
     * Get current audio latency in milliseconds
     */
    f32 get_latency_ms() const;

    // Statistics
    struct Stats {
        u64 samples_generated;
        u64 xma_packets_decoded;
        u64 pcm_packets_decoded;
        u64 xwma_packets_decoded;
        u32 active_voices;
        f32 buffer_usage; // 0.0 - 1.0
        u32 underruns;
        u32 overruns;
        u32 contexts_parsed;
        u64 dma_transfers;
        u64 cpu_cycles_total;
        u64 audio_samples_total;
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

    // XMA MMIO register state
    std::array<u32, 256> registers_{};     // Raw register storage
    GuestAddr context_array_ptr_ = 0;      // Guest addr of XMA context array
    std::array<u32, 8> context_enable_{};  // Enable bitmask (256 bits)
    std::array<u32, 8> context_done_{};    // Done/completion bitmask
    std::array<u32, 8> context_lock_{};    // Lock bitmask
    u32 interrupt_status_ = 0;
    u32 interrupt_mask_ = 0;
    u32 xma_control_ = 0;

    // Interrupt callback
    InterruptCallback interrupt_callback_;

    // Statistics
    Stats stats_{};

    // Processing
    void decode_xma_packets();
    void decode_pcm_context(u32 index);
    void decode_xwma_context(u32 index);
    void mix_voices();
    void submit_to_output();

    // Detect codec type from guest memory context data
    static AudioCodec detect_codec(u32 parser_state);

    // Parse XMA context from guest memory into internal state
    void parse_guest_context(u32 index);

    // Write back context state to guest memory
    void writeback_guest_context(u32 index);

    // Handle context enable/disable changes
    void on_context_enable_changed(u32 word_index, u32 old_value, u32 new_value);

    // Raise audio interrupt if conditions met
    void check_and_raise_interrupt();

    // Convert XMA sample rate code to Hz
    static u32 xma_sample_rate_to_hz(u32 code);

    // Select voices to mix based on priority (up to MAX_HW_MIXED_VOICES)
    void select_priority_voices(u32* voice_indices, u32& count);

    // Resample a voice's PCM from source rate to output rate with linear interpolation
    void resample_voice_to_mix(AudioVoice& voice, f32* mix_buf, u32 mix_frames);

    // Double-buffer index for output (alternates 0/1 each process() call)
    u32 mix_buffer_index_ = 0;
    // Two mix buffers for double-buffering
    std::vector<f32> mix_buffers_[2];

    // Audio callback for AndroidAudioOutput
    u32 audio_callback(f32* output, u32 frame_count);

    // ── Audio-CPU timing synchronization ──

    // Xbox 360 Xenon CPU clock: 3.2 GHz
    static constexpr u64 CPU_CLOCK_HZ = 3200000000ULL;

    // CPU cycles per audio sample at output rate (e.g. 48kHz → 66666 cycles/sample)
    u64 cycles_per_sample_ = CPU_CLOCK_HZ / 48000;

    // Accumulated CPU cycles since last audio drain
    std::atomic<u64> cpu_cycle_accumulator_{0};

    // Total CPU cycles elapsed (for absolute positioning)
    u64 cpu_cycles_total_ = 0;

    // Audio sample position (total samples output since start)
    u64 audio_sample_position_ = 0;

    // Pre-decode target: how many samples ahead of playback to decode
    u32 predecode_frames_ = 0;

    // ── DMA state per context ──
    struct DmaState {
        bool pending;           // DMA transfer in progress
        u32 src_offset;         // Current source read position
        u32 bytes_remaining;    // Bytes left in current DMA
    };
    std::array<DmaState, 256> dma_state_{};

    // DMA a chunk of data from guest memory for a context, respecting lock bits
    // Returns bytes actually transferred
    u32 dma_read_context(u32 index, u8* dest, u32 max_bytes);

    // Handle overrun: drop oldest samples when write catches read
    void handle_overrun();

    // Sync timing: compute how many samples to generate based on CPU cycles
    u32 compute_samples_needed();
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

