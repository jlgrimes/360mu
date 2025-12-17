# Task: XMA Audio Decoding

## Priority: 🟡 MEDIUM (For Audio)
## Estimated Time: 2-3 weeks
## Dependencies: None (audio mixer already working)

---

## Objective

Decode Xbox 360 XMA compressed audio to PCM for playback through the existing audio mixer.

---

## What To Build

### Location
- `native/src/apu/xma_decoder.cpp`
- `native/src/apu/xma_decoder.h`

---

## Background: XMA Format

XMA (Xbox Media Audio) is Microsoft's proprietary audio codec:
- Based on WMA Pro
- Supports 1-6 channels
- Sample rates: 24000, 32000, 44100, 48000 Hz
- Uses MDCT (Modified Discrete Cosine Transform)
- Packet-based with 2048-byte packets

---

## Implementation Options

### Option A: FFmpeg Integration (Recommended)

FFmpeg's libavcodec supports XMA2 decoding:

```cpp
class XmaDecoder {
public:
    Status initialize() {
        // Register XMA decoder
        codec_ = avcodec_find_decoder(AV_CODEC_ID_WMAPRO);
        if (!codec_) {
            // Try XMA2 codec
            codec_ = avcodec_find_decoder(AV_CODEC_ID_XMA2);
        }
        
        ctx_ = avcodec_alloc_context3(codec_);
        ctx_->sample_rate = 48000;
        ctx_->channels = 2;
        ctx_->channel_layout = AV_CH_LAYOUT_STEREO;
        
        if (avcodec_open2(ctx_, codec_, nullptr) < 0) {
            return Status::Error;
        }
        
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        
        return Status::Ok;
    }
    
    // Decode XMA packet to PCM
    size_t decode(const u8* xma_data, size_t xma_size,
                  s16* pcm_out, size_t pcm_max_samples) {
        packet_->data = const_cast<u8*>(xma_data);
        packet_->size = xma_size;
        
        int ret = avcodec_send_packet(ctx_, packet_);
        if (ret < 0) return 0;
        
        size_t total_samples = 0;
        while (ret >= 0) {
            ret = avcodec_receive_frame(ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            
            // Convert to s16 if needed
            size_t samples = frame_->nb_samples * frame_->channels;
            if (frame_->format == AV_SAMPLE_FMT_FLT) {
                for (size_t i = 0; i < samples; i++) {
                    float f = ((float*)frame_->data[0])[i];
                    pcm_out[total_samples + i] = (s16)(f * 32767.0f);
                }
            } else if (frame_->format == AV_SAMPLE_FMT_S16) {
                memcpy(pcm_out + total_samples, frame_->data[0], 
                       samples * sizeof(s16));
            }
            total_samples += samples;
        }
        
        return total_samples;
    }
    
private:
    AVCodec* codec_;
    AVCodecContext* ctx_;
    AVFrame* frame_;
    AVPacket* packet_;
};
```

### Option B: Native Implementation

If FFmpeg is not available, implement basic XMA decoding:

```cpp
class XmaDecoderNative {
public:
    // XMA packet header
    struct XmaPacketHeader {
        u32 frame_count : 6;
        u32 frame_offset_in_bits : 15;
        u32 packet_metadata : 3;
        u32 packet_skip_count : 8;
    };
    
    // Decode single XMA packet
    size_t decode_packet(const u8* packet, s16* output) {
        XmaPacketHeader header;
        memcpy(&header, packet, 4);
        
        // Skip to first frame
        BitReader reader(packet + 4, 2044);
        reader.skip(header.frame_offset_in_bits);
        
        size_t total_samples = 0;
        for (u32 i = 0; i < header.frame_count; i++) {
            total_samples += decode_frame(reader, output + total_samples);
        }
        
        return total_samples;
    }
    
private:
    // MDCT-based frame decoder
    size_t decode_frame(BitReader& reader, s16* output) {
        // Read frame header
        // Apply MDCT
        // Window and overlap-add
        // Return decoded samples
    }
    
    // MDCT coefficients, windows, etc.
    std::array<float, 512> mdct_cos_table_;
    std::array<float, 256> window_;
};
```

---

## XMA Context Management

Xbox 360 games use XMA hardware contexts:

```cpp
struct XmaContext {
    u32 id;
    
    // Input buffer (ring buffer)
    u32 input_buffer_addr;
    u32 input_buffer_size;
    u32 input_read_offset;
    u32 input_write_offset;
    
    // Output buffer
    u32 output_buffer_addr;
    u32 output_buffer_size;
    u32 output_write_offset;
    
    // State
    u32 sample_rate;
    u32 channels;
    bool loop_enabled;
    u32 loop_start;
    u32 loop_end;
    
    // Decoder state (preserved between calls)
    std::unique_ptr<XmaDecoder> decoder;
};

class XmaProcessor {
public:
    // Create a hardware context
    u32 create_context() {
        u32 id = next_context_id_++;
        contexts_[id] = std::make_unique<XmaContext>();
        contexts_[id]->id = id;
        contexts_[id]->decoder = std::make_unique<XmaDecoder>();
        return id;
    }
    
    // Process all active contexts
    void process() {
        for (auto& [id, ctx] : contexts_) {
            if (!ctx->active) continue;
            
            // Read XMA packets from input buffer
            while (has_input_data(ctx.get())) {
                u8 packet[2048];
                read_packet(ctx.get(), packet);
                
                // Decode to PCM
                s16 pcm[4096];
                size_t samples = ctx->decoder->decode(packet, 2048, pcm, 4096);
                
                // Write to output buffer
                write_output(ctx.get(), pcm, samples);
            }
        }
    }
    
private:
    std::map<u32, std::unique_ptr<XmaContext>> contexts_;
    u32 next_context_id_ = 1;
};
```

---

## HLE Functions

```cpp
// XMACreateContext - Create XMA decode context
DWORD HLE_XMACreateContext(DWORD* ContextIndex) {
    *ContextIndex = xma_processor_->create_context();
    return ERROR_SUCCESS;
}

// XMASetInputBuffer - Set input data source
DWORD HLE_XMASetInputBuffer(
    DWORD ContextIndex,
    void* InputBuffer,
    DWORD InputBufferSize
) {
    auto ctx = xma_processor_->get_context(ContextIndex);
    ctx->input_buffer_addr = memory_->host_to_guest(InputBuffer);
    ctx->input_buffer_size = InputBufferSize;
    return ERROR_SUCCESS;
}

// XMASetOutputBuffer - Set output destination
DWORD HLE_XMASetOutputBuffer(
    DWORD ContextIndex,
    void* OutputBuffer,
    DWORD OutputBufferSize
) {
    auto ctx = xma_processor_->get_context(ContextIndex);
    ctx->output_buffer_addr = memory_->host_to_guest(OutputBuffer);
    ctx->output_buffer_size = OutputBufferSize;
    return ERROR_SUCCESS;
}

// XMAEnableContext - Start decoding
DWORD HLE_XMAEnableContext(DWORD ContextIndex) {
    auto ctx = xma_processor_->get_context(ContextIndex);
    ctx->active = true;
    return ERROR_SUCCESS;
}

// XMADisableContext - Stop decoding
DWORD HLE_XMADisableContext(DWORD ContextIndex) {
    auto ctx = xma_processor_->get_context(ContextIndex);
    ctx->active = false;
    return ERROR_SUCCESS;
}
```

---

## CMake Integration

```cmake
# Option to use FFmpeg
option(X360MU_USE_FFMPEG "Use FFmpeg for XMA decoding" ON)

if(X360MU_USE_FFMPEG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(AVCODEC REQUIRED libavcodec)
    pkg_check_modules(AVUTIL REQUIRED libavutil)
    
    target_include_directories(x360mu_core PRIVATE 
        ${AVCODEC_INCLUDE_DIRS}
        ${AVUTIL_INCLUDE_DIRS}
    )
    target_link_libraries(x360mu_core 
        ${AVCODEC_LIBRARIES}
        ${AVUTIL_LIBRARIES}
    )
    target_compile_definitions(x360mu_core PRIVATE X360MU_USE_FFMPEG)
endif()
```

---

## Test Cases

```cpp
TEST(XmaTest, DecodePacket) {
    XmaDecoder decoder;
    decoder.initialize();
    
    // Test XMA packet (would need actual test data)
    u8 xma_packet[2048] = { /* ... */ };
    s16 pcm[4096];
    
    size_t samples = decoder.decode(xma_packet, 2048, pcm, 4096);
    EXPECT_GT(samples, 0);
}

TEST(XmaTest, ContextCreation) {
    XmaProcessor processor;
    
    u32 ctx1 = processor.create_context();
    u32 ctx2 = processor.create_context();
    
    EXPECT_NE(ctx1, ctx2);
    EXPECT_NE(processor.get_context(ctx1), nullptr);
}
```

---

## Do NOT Touch

- Audio mixer (`mixer.cpp`) - already working
- Android audio output (`android_audio.cpp`) - already working
- GPU code
- CPU code

---

## Success Criteria

1. ✅ FFmpeg-based XMA decoding works
2. ✅ XMA contexts can be created/destroyed
3. ✅ Decoded audio plays through mixer
4. ✅ Streaming audio works (continuous decode)
5. ✅ Multiple simultaneous contexts work

---

*This task adds XMA decoding. The audio mixer and output are already complete.*

