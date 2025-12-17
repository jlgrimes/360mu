/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XMA Audio Decoder Implementation
 * 
 * Decodes Xbox Media Audio (XMA/XMA2) to PCM.
 * XMA is a lossy audio codec based on WMA Pro.
 * 
 * When FFmpeg is available, uses libavcodec's WMAPRO decoder.
 * Otherwise falls back to a simplified custom decoder.
 */

#include "xma_decoder.h"
#include "../memory/memory.h"
#include <cmath>
#include <cstring>
#include <algorithm>

#ifdef X360MU_USE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#endif

#ifdef __ANDROID__
#include <android/log.h>
#include <aaudio/AAudio.h>
#define LOG_TAG "360mu-audio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[AUDIO] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[AUDIO ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

//=============================================================================
// FFmpeg XMA Decoder (when available)
//=============================================================================

#ifdef X360MU_USE_FFMPEG

/**
 * FFmpeg-based XMA decoder
 * Uses WMAPRO codec with XMA-specific configuration
 */
class FFmpegXmaDecoder {
public:
    FFmpegXmaDecoder() = default;
    ~FFmpegXmaDecoder() { shutdown(); }
    
    Status initialize(u32 sample_rate, u32 num_channels) {
        // Find WMAPRO decoder (XMA is based on it)
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_WMAPRO);
        if (!codec) {
            LOGE("FFmpeg WMAPRO decoder not found");
            return Status::NotFound;
        }
        
        codec_ctx_ = avcodec_alloc_context3(codec);
        if (!codec_ctx_) {
            LOGE("Failed to allocate codec context");
            return Status::OutOfMemory;
        }
        
        // Configure for XMA
        codec_ctx_->sample_rate = sample_rate;
        codec_ctx_->channels = num_channels;
        codec_ctx_->channel_layout = (num_channels == 2) ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
        codec_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        codec_ctx_->bits_per_coded_sample = 16;
        codec_ctx_->block_align = 2048;  // XMA packet size
        codec_ctx_->bit_rate = 192000;   // Typical XMA bitrate
        
        // XMA extradata (simplified)
        static const u8 xma_extradata[] = {
            0x00, 0x00, 0x00, 0x00,  // Format tag
            0x02, 0x00,              // Channels
            0x00, 0x00, 0xBB, 0x80,  // Sample rate (48000)
            0x00, 0x00, 0x00, 0x00,  // Bytes per second
            0x00, 0x08,              // Block align (2048)
            0x10, 0x00,              // Bits per sample
            0x00, 0x00               // Extra data size
        };
        codec_ctx_->extradata = static_cast<u8*>(av_malloc(sizeof(xma_extradata)));
        if (codec_ctx_->extradata) {
            memcpy(codec_ctx_->extradata, xma_extradata, sizeof(xma_extradata));
            codec_ctx_->extradata_size = sizeof(xma_extradata);
        }
        
        if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
            LOGE("Failed to open WMAPRO codec");
            avcodec_free_context(&codec_ctx_);
            return Status::Error;
        }
        
        // Allocate frame
        frame_ = av_frame_alloc();
        if (!frame_) {
            avcodec_free_context(&codec_ctx_);
            return Status::OutOfMemory;
        }
        
        // Allocate packet
        packet_ = av_packet_alloc();
        if (!packet_) {
            av_frame_free(&frame_);
            avcodec_free_context(&codec_ctx_);
            return Status::OutOfMemory;
        }
        
        // Initialize resampler (FLTP to S16)
        swr_ctx_ = swr_alloc_set_opts(
            nullptr,
            codec_ctx_->channel_layout,
            AV_SAMPLE_FMT_S16,
            sample_rate,
            codec_ctx_->channel_layout,
            AV_SAMPLE_FMT_FLTP,
            sample_rate,
            0, nullptr
        );
        
        if (!swr_ctx_ || swr_init(swr_ctx_) < 0) {
            LOGW("Resampler init failed, will do manual conversion");
            if (swr_ctx_) {
                swr_free(&swr_ctx_);
                swr_ctx_ = nullptr;
            }
        }
        
        sample_rate_ = sample_rate;
        num_channels_ = num_channels;
        initialized_ = true;
        
        LOGI("FFmpeg XMA decoder initialized: %uHz, %u channels", sample_rate, num_channels);
        return Status::Ok;
    }
    
    void shutdown() {
        if (swr_ctx_) {
            swr_free(&swr_ctx_);
            swr_ctx_ = nullptr;
        }
        if (packet_) {
            av_packet_free(&packet_);
            packet_ = nullptr;
        }
        if (frame_) {
            av_frame_free(&frame_);
            frame_ = nullptr;
        }
        if (codec_ctx_) {
            avcodec_free_context(&codec_ctx_);
            codec_ctx_ = nullptr;
        }
        initialized_ = false;
    }
    
    /**
     * Decode XMA packet to PCM
     * Returns number of samples decoded
     */
    u32 decode(const u8* input, u32 input_size, s16* output, u32 max_samples) {
        if (!initialized_ || !input || !output || input_size == 0) {
            return 0;
        }
        
        // Parse XMA packet header
        u32 frame_count = (input[0] >> 2) & 0x3F;
        u32 skip_samples = ((input[0] & 0x03) << 13) | (input[1] << 5) | (input[2] >> 3);
        
        (void)frame_count;  // Used for validation
        (void)skip_samples; // Used for start of stream
        
        // Skip header (4 bytes) for actual data
        const u8* xma_data = input + 4;
        u32 xma_size = input_size - 4;
        
        // Feed data to decoder
        packet_->data = const_cast<u8*>(xma_data);
        packet_->size = xma_size;
        
        int ret = avcodec_send_packet(codec_ctx_, packet_);
        if (ret < 0) {
            LOGD("avcodec_send_packet failed: %d", ret);
            return 0;
        }
        
        u32 total_samples = 0;
        
        while (total_samples < max_samples) {
            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                LOGD("avcodec_receive_frame failed: %d", ret);
                break;
            }
            
            u32 samples_to_write = std::min(static_cast<u32>(frame_->nb_samples),
                                           max_samples - total_samples);
            
            // Convert FLTP to S16 interleaved
            if (swr_ctx_) {
                u8* out_ptr = reinterpret_cast<u8*>(output + total_samples * num_channels_);
                const u8* const* in_ptr = const_cast<const u8* const*>(frame_->extended_data);
                swr_convert(swr_ctx_, &out_ptr, samples_to_write,
                           in_ptr, frame_->nb_samples);
            } else {
                // Manual conversion
                for (u32 i = 0; i < samples_to_write; i++) {
                    for (u32 ch = 0; ch < num_channels_; ch++) {
                        float sample = reinterpret_cast<float*>(frame_->data[ch])[i];
                        sample = std::max(-1.0f, std::min(1.0f, sample));
                        output[(total_samples + i) * num_channels_ + ch] = 
                            static_cast<s16>(sample * 32767.0f);
                    }
                }
            }
            
            total_samples += samples_to_write;
        }
        
        return total_samples;
    }
    
    void reset() {
        if (codec_ctx_) {
            avcodec_flush_buffers(codec_ctx_);
        }
    }
    
    bool is_initialized() const { return initialized_; }
    
private:
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwrContext* swr_ctx_ = nullptr;
    
    u32 sample_rate_ = 48000;
    u32 num_channels_ = 2;
    bool initialized_ = false;
};

#endif // X360MU_USE_FFMPEG

//=============================================================================
// XMA Constants and Tables
//=============================================================================

// XMA quantization table (simplified - real XMA uses more complex tables)
const f32 XmaDecoder::quantization_table[] = {
    0.000000f, 0.015625f, 0.031250f, 0.046875f, 0.062500f, 0.078125f, 0.093750f, 0.109375f,
    0.125000f, 0.140625f, 0.156250f, 0.171875f, 0.187500f, 0.203125f, 0.218750f, 0.234375f,
    0.250000f, 0.281250f, 0.312500f, 0.343750f, 0.375000f, 0.406250f, 0.437500f, 0.468750f,
    0.500000f, 0.562500f, 0.625000f, 0.687500f, 0.750000f, 0.812500f, 0.875000f, 0.937500f,
    1.000000f
};

// Scale factor table for XMA (2^(n/4))
const f32 XmaDecoder::scale_factor_table[] = {
    1.0000000f, 1.1892071f, 1.4142135f, 1.6817928f,
    2.0000000f, 2.3784142f, 2.8284271f, 3.3635856f,
    4.0000000f, 4.7568284f, 5.6568542f, 6.7271712f,
    8.0000000f, 9.5136569f, 11.313708f, 13.454342f,
    16.000000f, 19.027314f, 22.627417f, 26.908685f,
    32.000000f, 38.054627f, 45.254834f, 53.817370f,
    64.000000f
};

//=============================================================================
// BitReader Implementation
//=============================================================================

XmaDecoder::BitReader::BitReader(const u8* data, u32 size)
    : data_(data)
    , size_(size * 8)
    , bit_position_(0)
{
}

u32 XmaDecoder::BitReader::read_bits(u32 count) {
    if (!has_bits(count)) return 0;
    
    u32 result = 0;
    while (count > 0) {
        u32 byte_offset = bit_position_ / 8;
        u32 bit_offset = bit_position_ % 8;
        u32 bits_in_byte = std::min(8u - bit_offset, count);
        
        u32 mask = (1 << bits_in_byte) - 1;
        u32 value = (data_[byte_offset] >> (8 - bit_offset - bits_in_byte)) & mask;
        
        result = (result << bits_in_byte) | value;
        bit_position_ += bits_in_byte;
        count -= bits_in_byte;
    }
    
    return result;
}

s32 XmaDecoder::BitReader::read_signed_bits(u32 count) {
    u32 value = read_bits(count);
    // Sign extend
    if (count > 0 && (value & (1 << (count - 1)))) {
        value |= ~((1 << count) - 1);
    }
    return static_cast<s32>(value);
}

void XmaDecoder::BitReader::skip_bits(u32 count) {
    bit_position_ = std::min(bit_position_ + count, size_);
}

bool XmaDecoder::BitReader::has_bits(u32 count) const {
    return bit_position_ + count <= size_;
}

void XmaDecoder::BitReader::seek(u32 bit_offset) {
    bit_position_ = std::min(bit_offset, size_);
}

//=============================================================================
// XMA Decoder Implementation
//=============================================================================

XmaDecoder::XmaDecoder() = default;
XmaDecoder::~XmaDecoder() = default;

Status XmaDecoder::initialize() {
    running_ = true;
    LOGI("XMA decoder initialized");
    return Status::Ok;
}

void XmaDecoder::shutdown() {
    running_ = false;
    
    std::lock_guard<std::mutex> lock(contexts_mutex_);
    for (auto& ctx : contexts_) {
        ctx.reset();
    }
    
    LOGI("XMA decoder shutdown");
}

u32 XmaDecoder::create_context(u32 sample_rate, u32 num_channels) {
    std::lock_guard<std::mutex> lock(contexts_mutex_);
    
    for (u32 i = 0; i < MAX_CONTEXTS; i++) {
        if (!contexts_[i]) {
            contexts_[i] = std::make_unique<XmaContext>();
            contexts_[i]->sample_rate = sample_rate;
            contexts_[i]->num_channels = num_channels;
            contexts_[i]->bits_per_sample = 16;
            contexts_[i]->active = false;
            contexts_[i]->history.fill(0);
            contexts_[i]->history_index = 0;
            
            // Initialize predictor coefficients
            for (int j = 0; j < 128; j++) {
                contexts_[i]->predictor_coefs[j] = 0.0f;
            }
            
#ifdef X360MU_USE_FFMPEG
            // Try to create FFmpeg decoder
            contexts_[i]->ffmpeg_decoder = std::make_unique<FFmpegXmaDecoder>();
            if (contexts_[i]->ffmpeg_decoder->initialize(sample_rate, num_channels) != Status::Ok) {
                LOGW("FFmpeg decoder init failed, using fallback for context %u", i);
                contexts_[i]->ffmpeg_decoder.reset();
            }
#endif
            
            LOGD("Created XMA context %u: %uHz, %u channels", i, sample_rate, num_channels);
            return i;
        }
    }
    
    LOGE("No free XMA contexts");
    return UINT32_MAX;
}

void XmaDecoder::destroy_context(u32 context_id) {
    if (context_id >= MAX_CONTEXTS) return;
    
    std::lock_guard<std::mutex> lock(contexts_mutex_);
    contexts_[context_id].reset();
}

void XmaDecoder::set_input_buffer(u32 context_id, GuestAddr buffer, u32 size, u32 buffer_index) {
    if (context_id >= MAX_CONTEXTS || !contexts_[context_id]) return;
    
    auto& ctx = *contexts_[context_id];
    if (buffer_index == 0) {
        ctx.input_buffer_0 = buffer;
        ctx.input_buffer_0_size = size;
    } else {
        ctx.input_buffer_1 = buffer;
        ctx.input_buffer_1_size = size;
    }
}

void XmaDecoder::set_output_buffer(u32 context_id, GuestAddr buffer, u32 size) {
    if (context_id >= MAX_CONTEXTS || !contexts_[context_id]) return;
    
    auto& ctx = *contexts_[context_id];
    ctx.output_buffer = buffer;
    ctx.output_buffer_size = size;
    ctx.output_buffer_write_offset = 0;
}

void XmaDecoder::start_context(u32 context_id) {
    if (context_id >= MAX_CONTEXTS || !contexts_[context_id]) return;
    
    auto& ctx = *contexts_[context_id];
    ctx.active = true;
    ctx.input_buffer_read_offset = 0;
    ctx.samples_decoded = 0;
    ctx.frames_decoded = 0;
    
    LOGD("Started XMA context %u", context_id);
}

void XmaDecoder::stop_context(u32 context_id) {
    if (context_id >= MAX_CONTEXTS || !contexts_[context_id]) return;
    
    auto& ctx = *contexts_[context_id];
    ctx.active = false;
    
    LOGD("Stopped XMA context %u", context_id);
}

bool XmaDecoder::is_buffer_done(u32 context_id, u32 buffer_index) const {
    if (context_id >= MAX_CONTEXTS || !contexts_[context_id]) return true;
    
    const auto& ctx = *contexts_[context_id];
    if (!ctx.active) return true;
    
    if (buffer_index == 0) {
        return ctx.input_buffer_read_offset >= ctx.input_buffer_0_size;
    } else {
        return ctx.input_buffer_read_offset >= ctx.input_buffer_1_size;
    }
}

u32 XmaDecoder::get_samples_decoded(u32 context_id) const {
    if (context_id >= MAX_CONTEXTS || !contexts_[context_id]) return 0;
    return contexts_[context_id]->samples_decoded;
}

void XmaDecoder::process(Memory* memory) {
    if (!running_ || !memory) return;
    
    std::lock_guard<std::mutex> lock(contexts_mutex_);
    
    for (auto& ctx_ptr : contexts_) {
        if (!ctx_ptr || !ctx_ptr->active) continue;
        
        auto& ctx = *ctx_ptr;
        
        // Get input buffer
        GuestAddr input_addr;
        u32 input_size;
        if (ctx.input_buffer_index == 0) {
            input_addr = ctx.input_buffer_0;
            input_size = ctx.input_buffer_0_size;
        } else {
            input_addr = ctx.input_buffer_1;
            input_size = ctx.input_buffer_1_size;
        }
        
        if (ctx.input_buffer_read_offset >= input_size) {
            // Switch buffers or stop
            ctx.input_buffer_index = 1 - ctx.input_buffer_index;
            ctx.input_buffer_read_offset = 0;
            
            // Check if other buffer is ready
            if (ctx.input_buffer_index == 0) {
                if (ctx.input_buffer_0_size == 0) {
                    ctx.active = false;
                    continue;
                }
            } else {
                if (ctx.input_buffer_1_size == 0) {
                    ctx.active = false;
                    continue;
                }
            }
        }
        
        // Read XMA data from memory (one packet at a time - 2048 bytes typical)
        constexpr u32 XMA_PACKET_SIZE = 2048;
        u32 bytes_to_read = std::min(XMA_PACKET_SIZE, input_size - ctx.input_buffer_read_offset);
        std::vector<u8> input_data(bytes_to_read);
        
        for (u32 i = 0; i < bytes_to_read; i++) {
            input_data[i] = memory->read_u8(input_addr + ctx.input_buffer_read_offset + i);
        }
        
        // Output buffer
        std::vector<s16> output_samples;
        output_samples.reserve(4096);
        
#ifdef X360MU_USE_FFMPEG
        // Try FFmpeg decoder first
        if (ctx.ffmpeg_decoder && ctx.ffmpeg_decoder->is_initialized()) {
            s16 ffmpeg_output[4096 * 2];
            u32 samples = ctx.ffmpeg_decoder->decode(input_data.data(), bytes_to_read,
                                                      ffmpeg_output, 4096);
            if (samples > 0) {
                for (u32 i = 0; i < samples * ctx.num_channels; i++) {
                    output_samples.push_back(ffmpeg_output[i]);
                }
                ctx.frames_decoded++;
                ctx.input_buffer_read_offset += bytes_to_read;
            } else {
                // FFmpeg failed, fall through to software decoder
                ctx.ffmpeg_decoder.reset();
            }
        }
#endif
        
        // Fallback: Use software decoder if FFmpeg not available or failed
        if (output_samples.empty()) {
            BitReader reader(input_data.data(), bytes_to_read);
            
            while (reader.has_bits(32)) {
                s16 frame_output[256 * 2]; // Max samples per frame
                u32 samples_written = 0;
                
                if (!decode_frame(ctx, reader, frame_output, samples_written)) {
                    break;
                }
                
                // Append to output
                for (u32 i = 0; i < samples_written; i++) {
                    output_samples.push_back(frame_output[i]);
                }
                
                ctx.frames_decoded++;
            }
            
            // Update read offset
            ctx.input_buffer_read_offset += reader.position() / 8;
        }
        
        // Write output to memory
        u32 output_bytes = output_samples.size() * sizeof(s16);
        u32 bytes_to_write = std::min(output_bytes, 
                                       ctx.output_buffer_size - ctx.output_buffer_write_offset);
        
        for (u32 i = 0; i < bytes_to_write; i += 2) {
            u32 sample_idx = i / 2;
            if (sample_idx < output_samples.size()) {
                memory->write_u16(ctx.output_buffer + ctx.output_buffer_write_offset + i,
                                  static_cast<u16>(output_samples[sample_idx]));
            }
        }
        
        ctx.output_buffer_write_offset += bytes_to_write;
        ctx.samples_decoded += output_samples.size() / ctx.num_channels;
    }
}

bool XmaDecoder::decode_frame(XmaContext& ctx, BitReader& reader, 
                               s16* output, u32& samples_written) {
    samples_written = 0;
    
    // Read frame header
    // XMA frames start with packet header
    if (!reader.has_bits(15)) return false;
    
    // Frame length in bits
    u32 frame_length_bits = reader.read_bits(15);
    if (frame_length_bits == 0x7FFF) {
        // End of stream marker
        return false;
    }
    
    // Skip frame length check for now (simplified decoder)
    
    // Read sub-frame count (typically 1-4)
    u32 num_subframes = std::min(4u, std::max(1u, frame_length_bits / 2048));
    
    for (u32 sf = 0; sf < num_subframes; sf++) {
        if (!decode_subframe(ctx, reader, output + samples_written)) {
            break;
        }
        samples_written += XmaSubframe::SAMPLES_PER_SUBFRAME * ctx.num_channels;
    }
    
    return samples_written > 0;
}

bool XmaDecoder::decode_subframe(XmaContext& ctx, BitReader& reader, s16* output) {
    constexpr u32 NUM_SAMPLES = XmaSubframe::SAMPLES_PER_SUBFRAME;
    
    if (!reader.has_bits(32)) return false;
    
    // Simplified XMA decoding - real XMA is much more complex
    // This provides a basic structure that produces output
    
    // Read scale factors (4 bits each for 8 bands)
    std::array<u8, 8> scale_factors;
    for (int i = 0; i < 8; i++) {
        scale_factors[i] = reader.has_bits(4) ? reader.read_bits(4) : 0;
    }
    
    // Decode samples for each channel
    for (u32 ch = 0; ch < ctx.num_channels; ch++) {
        std::array<f32, NUM_SAMPLES> samples_f;
        samples_f.fill(0.0f);
        
        // Read quantized coefficients
        for (u32 band = 0; band < 8; band++) {
            u32 band_start = band * NUM_SAMPLES / 8;
            u32 band_end = (band + 1) * NUM_SAMPLES / 8;
            
            f32 scale = scale_factor_table[scale_factors[band]];
            
            for (u32 i = band_start; i < band_end; i++) {
                if (!reader.has_bits(4)) break;
                
                s32 quant = reader.read_signed_bits(4);
                samples_f[i] = quant * scale * 0.001f; // Scale down for audio range
            }
        }
        
        // Apply predictor filter (simplified LPC)
        apply_predictor(ctx, samples_f.data(), NUM_SAMPLES);
        
        // Convert to 16-bit PCM and interleave
        for (u32 i = 0; i < NUM_SAMPLES; i++) {
            f32 sample = samples_f[i];
            
            // Clamp to [-1, 1]
            sample = std::max(-1.0f, std::min(1.0f, sample));
            
            // Convert to s16
            s16 pcm = static_cast<s16>(sample * 32767.0f);
            
            // Store interleaved
            output[i * ctx.num_channels + ch] = pcm;
        }
    }
    
    return true;
}

void XmaDecoder::apply_predictor(XmaContext& ctx, f32* samples, u32 count) {
    // Simple low-pass filter to smooth decoded audio
    // Real XMA uses adaptive LPC prediction
    
    for (u32 i = 1; i < count; i++) {
        // Simple 2-tap predictor
        f32 predicted = samples[i] + 0.95f * ctx.history[ctx.history_index];
        samples[i] = predicted * 0.5f;
        
        ctx.history[ctx.history_index] = static_cast<s16>(samples[i] * 32767.0f);
        ctx.history_index = (ctx.history_index + 1) % ctx.history.size();
    }
}

std::vector<s16> XmaDecoder::decode(const u8* data, u32 size, u32 sample_rate, u32 num_channels) {
    std::vector<s16> output;
    
#ifdef X360MU_USE_FFMPEG
    // Try FFmpeg first for better quality
    FFmpegXmaDecoder ffmpeg;
    if (ffmpeg.initialize(sample_rate, num_channels) == Status::Ok) {
        constexpr u32 XMA_PACKET_SIZE = 2048;
        u32 offset = 0;
        
        while (offset < size) {
            u32 packet_size = std::min(XMA_PACKET_SIZE, size - offset);
            s16 packet_output[4096 * 2];
            
            u32 samples = ffmpeg.decode(data + offset, packet_size, packet_output, 4096);
            for (u32 i = 0; i < samples * num_channels; i++) {
                output.push_back(packet_output[i]);
            }
            
            offset += packet_size;
        }
        
        if (!output.empty()) {
            return output;
        }
        // Fall through to software decoder if FFmpeg produced no output
    }
#endif
    
    // Fallback software decoder
    XmaContext ctx = {};
    ctx.sample_rate = sample_rate;
    ctx.num_channels = num_channels;
    ctx.bits_per_sample = 16;
    ctx.history.fill(0);
    ctx.history_index = 0;
    
    BitReader reader(data, size);
    
    while (reader.has_bits(32)) {
        s16 frame_output[256 * 2];
        u32 samples_written = 0;
        
        if (!decode_frame(ctx, reader, frame_output, samples_written)) {
            break;
        }
        
        for (u32 i = 0; i < samples_written; i++) {
            output.push_back(frame_output[i]);
        }
    }
    
    return output;
}

//=============================================================================
// Audio Mixer Implementation
//=============================================================================

AudioMixer::AudioMixer() = default;
AudioMixer::~AudioMixer() = default;

Status AudioMixer::initialize(u32 sample_rate, u32 buffer_frames) {
    output_sample_rate_ = sample_rate;
    buffer_frames_ = buffer_frames;
    
    // Allocate mix buffer (stereo)
    mix_buffer_.resize(buffer_frames * 2);
    
    LOGI("Audio mixer initialized: %uHz, %u frames", sample_rate, buffer_frames);
    return Status::Ok;
}

void AudioMixer::shutdown() {
    std::lock_guard<std::mutex> lock(voices_mutex_);
    for (auto& voice : voices_) {
        voice.reset();
    }
    mix_buffer_.clear();
}

u32 AudioMixer::create_voice(u32 sample_rate, u32 num_channels) {
    std::lock_guard<std::mutex> lock(voices_mutex_);
    
    for (u32 i = 0; i < MAX_VOICES; i++) {
        if (!voices_[i]) {
            voices_[i] = std::make_unique<Voice>();
            voices_[i]->sample_rate = sample_rate;
            voices_[i]->num_channels = num_channels;
            voices_[i]->volume = 1.0f;
            voices_[i]->pan = 0.0f;
            voices_[i]->active = true;
            
            // Ring buffer size (1 second of audio)
            voices_[i]->buffer.resize(sample_rate * num_channels);
            voices_[i]->read_pos = 0;
            voices_[i]->write_pos = 0;
            
            // Resampling
            voices_[i]->sample_position = 0.0f;
            voices_[i]->sample_increment = static_cast<f32>(sample_rate) / output_sample_rate_;
            
            return i;
        }
    }
    
    return UINT32_MAX;
}

void AudioMixer::destroy_voice(u32 voice_id) {
    if (voice_id >= MAX_VOICES) return;
    
    std::lock_guard<std::mutex> lock(voices_mutex_);
    voices_[voice_id].reset();
}

void AudioMixer::submit_samples(u32 voice_id, const s16* samples, u32 sample_count) {
    if (voice_id >= MAX_VOICES || !voices_[voice_id]) return;
    
    auto& voice = *voices_[voice_id];
    
    u32 buffer_size = voice.buffer.size();
    u32 write_pos = voice.write_pos.load();
    
    for (u32 i = 0; i < sample_count; i++) {
        voice.buffer[write_pos] = samples[i];
        write_pos = (write_pos + 1) % buffer_size;
    }
    
    voice.write_pos.store(write_pos);
}

void AudioMixer::set_voice_volume(u32 voice_id, f32 volume) {
    if (voice_id >= MAX_VOICES || !voices_[voice_id]) return;
    voices_[voice_id]->volume = std::max(0.0f, std::min(1.0f, volume));
}

void AudioMixer::set_voice_pan(u32 voice_id, f32 pan) {
    if (voice_id >= MAX_VOICES || !voices_[voice_id]) return;
    voices_[voice_id]->pan = std::max(-1.0f, std::min(1.0f, pan));
}

void AudioMixer::set_master_volume(f32 volume) {
    master_volume_ = std::max(0.0f, std::min(1.0f, volume));
}

u32 AudioMixer::get_output(s16* output, u32 frame_count) {
    if (paused_) {
        std::memset(output, 0, frame_count * 2 * sizeof(s16));
        return frame_count;
    }
    
    // Clear mix buffer
    std::fill(mix_buffer_.begin(), mix_buffer_.begin() + frame_count * 2, 0.0f);
    
    // Mix all active voices
    {
        std::lock_guard<std::mutex> lock(voices_mutex_);
        
        for (auto& voice_ptr : voices_) {
            if (!voice_ptr || !voice_ptr->active) continue;
            
            resample_voice(*voice_ptr, mix_buffer_.data(), frame_count);
        }
    }
    
    // Apply master volume and convert to s16
    for (u32 i = 0; i < frame_count * 2; i++) {
        f32 sample = mix_buffer_[i] * master_volume_;
        
        // Soft clipping
        if (sample > 1.0f) sample = 1.0f - 1.0f / (sample + 1.0f);
        else if (sample < -1.0f) sample = -1.0f + 1.0f / (-sample + 1.0f);
        
        output[i] = static_cast<s16>(sample * 32767.0f);
    }
    
    return frame_count;
}

void AudioMixer::resample_voice(Voice& voice, f32* output, u32 frame_count) {
    u32 buffer_size = voice.buffer.size();
    u32 read_pos = voice.read_pos.load();
    u32 write_pos = voice.write_pos.load();
    
    // Calculate samples available
    u32 samples_available;
    if (write_pos >= read_pos) {
        samples_available = write_pos - read_pos;
    } else {
        samples_available = buffer_size - read_pos + write_pos;
    }
    
    if (samples_available < voice.num_channels) return;
    
    // Calculate pan gains
    f32 left_gain = voice.volume * (1.0f - std::max(0.0f, voice.pan));
    f32 right_gain = voice.volume * (1.0f + std::min(0.0f, voice.pan));
    
    for (u32 i = 0; i < frame_count; i++) {
        // Get sample position
        u32 pos0 = static_cast<u32>(voice.sample_position) * voice.num_channels;
        f32 frac = voice.sample_position - std::floor(voice.sample_position);
        
        // Bounds check
        if (pos0 + voice.num_channels >= samples_available) break;
        
        // Linear interpolation
        f32 left, right;
        
        if (voice.num_channels == 1) {
            // Mono
            s16 s0 = voice.buffer[(read_pos + pos0) % buffer_size];
            s16 s1 = voice.buffer[(read_pos + pos0 + 1) % buffer_size];
            f32 sample = (s0 * (1.0f - frac) + s1 * frac) / 32768.0f;
            left = right = sample;
        } else {
            // Stereo
            s16 l0 = voice.buffer[(read_pos + pos0) % buffer_size];
            s16 r0 = voice.buffer[(read_pos + pos0 + 1) % buffer_size];
            s16 l1 = voice.buffer[(read_pos + pos0 + 2) % buffer_size];
            s16 r1 = voice.buffer[(read_pos + pos0 + 3) % buffer_size];
            
            left = (l0 * (1.0f - frac) + l1 * frac) / 32768.0f;
            right = (r0 * (1.0f - frac) + r1 * frac) / 32768.0f;
        }
        
        // Apply pan and add to output
        output[i * 2] += left * left_gain;
        output[i * 2 + 1] += right * right_gain;
        
        // Advance position
        voice.sample_position += voice.sample_increment;
    }
    
    // Update read position
    u32 samples_consumed = static_cast<u32>(voice.sample_position) * voice.num_channels;
    voice.read_pos.store((read_pos + samples_consumed) % buffer_size);
    voice.sample_position -= std::floor(voice.sample_position);
}

void AudioMixer::pause() {
    paused_ = true;
}

void AudioMixer::resume() {
    paused_ = false;
}

u32 AudioMixer::get_latency() const {
    return buffer_frames_;
}

//=============================================================================
// APU Implementation
//=============================================================================

Apu::Apu() = default;
Apu::~Apu() = default;

Status Apu::initialize(Memory* memory) {
    memory_ = memory;
    
    Status status = xma_decoder_.initialize();
    if (status != Status::Ok) return status;
    
    status = mixer_.initialize(48000, 1024);
    if (status != Status::Ok) return status;
    
    registers_.fill(0);
    running_ = true;
    
    LOGI("APU initialized");
    return Status::Ok;
}

void Apu::shutdown() {
    running_ = false;
    xma_decoder_.shutdown();
    mixer_.shutdown();
}

void Apu::process() {
    if (!running_) return;
    xma_decoder_.process(memory_);
}

u32 Apu::get_output(s16* output, u32 frame_count) {
    return mixer_.get_output(output, frame_count);
}

void Apu::write_register(u32 offset, u32 value) {
    if (offset < registers_.size()) {
        registers_[offset] = value;
        
        // Handle specific registers
        switch (offset) {
            case 0x00: // Control register
                if (value & 1) {
                    // Enable APU
                }
                break;
            case 0x10: // Interrupt enable
                interrupt_mask_ = value;
                break;
            case 0x14: // Interrupt acknowledge
                interrupt_status_ &= ~value;
                break;
        }
    }
}

u32 Apu::read_register(u32 offset) const {
    if (offset < registers_.size()) {
        switch (offset) {
            case 0x10: return interrupt_mask_;
            case 0x14: return interrupt_status_;
            default: return registers_[offset];
        }
    }
    return 0;
}

} // namespace x360mu

