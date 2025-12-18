# Stream D: Audio Output

**Priority**: LOW (game works without audio)  
**Estimated Time**: 2 hours  
**Dependencies**: None (can start immediately)  
**Blocks**: No game audio

## Overview

The emulator has:

- `XmaDecoder` - Decodes Xbox 360 XMA audio format
- `AndroidAudio` - AAudio output backend (~550 lines)

These need to be connected so decoded audio plays through the device speakers.

## Files to Modify

- `native/src/apu/android_audio.cpp` - Connect XMA decoder output
- `native/src/apu/xma_decoder.cpp` - Ensure decoder outputs PCM
- `native/src/apu/audio.h` - Audio interface definitions

## Architecture

```
Xbox 360 Game
     ↓
XMA Audio Buffers (in guest memory)
     ↓
XMA Decoder (converts to PCM)
     ↓
Android Audio (AAudio stream)
     ↓
Device Speakers/Headphones
```

---

## Task D.1: XMA Decoder Integration

**File**: `native/src/apu/xma_decoder.cpp`

### D.1.1: Ensure PCM Output

XMA decoder should output standard PCM audio:

```cpp
struct XmaContext {
    u32 input_buffer_ptr;     // Guest address of XMA data
    u32 input_buffer_size;
    u32 output_buffer_ptr;    // Guest address for PCM output
    u32 output_buffer_size;
    u32 sample_rate;          // Usually 44100 or 48000
    u32 channels;             // 1, 2, or 6
    // ... other state
};

struct DecodedAudio {
    std::vector<s16> samples;  // Interleaved PCM
    u32 sample_rate;
    u32 channels;
};

DecodedAudio XmaDecoder::decode(const XmaContext& ctx, Memory* memory) {
    DecodedAudio result;
    result.sample_rate = ctx.sample_rate;
    result.channels = ctx.channels;

    // Read XMA data from guest memory
    std::vector<u8> xma_data(ctx.input_buffer_size);
    memory->read_bytes(ctx.input_buffer_ptr, xma_data.data(), xma_data.size());

    // Decode XMA to PCM
    // XMA is based on WMA Pro - may need FFmpeg or custom decoder
    result.samples = decode_xma_block(xma_data, ctx.channels, ctx.sample_rate);

    return result;
}
```

### D.1.2: XMA Format Basics

XMA (Xbox Media Audio) is Microsoft's proprietary format based on WMA Pro:

```cpp
// XMA packet header
struct XmaPacketHeader {
    u32 frame_count : 6;
    u32 frame_offset_in_bits : 15;
    u32 metadata : 3;
    u32 packet_skip_count : 8;
};

// Decoding approach options:
// 1. Use FFmpeg with WMA Pro decoder (requires linking FFmpeg)
// 2. Port Xenia's XMA decoder
// 3. Use a simplified approach for common formats
```

---

## Task D.2: Android Audio Connection

**File**: `native/src/apu/android_audio.cpp`

### D.2.1: Audio Stream Setup

```cpp
class AndroidAudio {
private:
    AAudioStream* stream_ = nullptr;
    XmaDecoder decoder_;
    std::mutex buffer_mutex_;
    std::vector<s16> audio_buffer_;

public:
    bool initialize(u32 sample_rate, u32 channels) {
        AAudioStreamBuilder* builder;
        AAudio_createStreamBuilder(&builder);

        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setSampleRate(builder, sample_rate);
        AAudioStreamBuilder_setChannelCount(builder, channels);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setPerformanceMode(builder,
                                                AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setDataCallback(builder, audio_callback, this);

        aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream_);
        AAudioStreamBuilder_delete(builder);

        if (result != AAUDIO_OK) {
            return false;
        }

        AAudioStream_requestStart(stream_);
        return true;
    }

    void shutdown() {
        if (stream_) {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
    }
};
```

### D.2.2: Audio Callback

```cpp
static aaudio_data_callback_result_t audio_callback(
    AAudioStream* stream,
    void* user_data,
    void* audio_data,
    int32_t num_frames) {

    AndroidAudio* audio = static_cast<AndroidAudio*>(user_data);
    s16* output = static_cast<s16*>(audio_data);

    std::lock_guard<std::mutex> lock(audio->buffer_mutex_);

    u32 channels = AAudioStream_getChannelCount(stream);
    u32 samples_needed = num_frames * channels;

    if (audio->audio_buffer_.size() >= samples_needed) {
        // Copy from buffer
        std::copy(audio->audio_buffer_.begin(),
                  audio->audio_buffer_.begin() + samples_needed,
                  output);
        audio->audio_buffer_.erase(audio->audio_buffer_.begin(),
                                    audio->audio_buffer_.begin() + samples_needed);
    } else {
        // Underrun - output silence
        std::fill(output, output + samples_needed, 0);
    }

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
```

### D.2.3: Submit Decoded Audio

```cpp
void AndroidAudio::submit_audio(const DecodedAudio& decoded) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    // Resample if needed
    if (decoded.sample_rate != current_sample_rate_) {
        auto resampled = resample(decoded.samples,
                                   decoded.sample_rate,
                                   current_sample_rate_,
                                   decoded.channels);
        audio_buffer_.insert(audio_buffer_.end(),
                             resampled.begin(),
                             resampled.end());
    } else {
        audio_buffer_.insert(audio_buffer_.end(),
                             decoded.samples.begin(),
                             decoded.samples.end());
    }

    // Prevent buffer from growing too large
    const size_t max_buffer = current_sample_rate_ * 2;  // ~1 second
    if (audio_buffer_.size() > max_buffer) {
        audio_buffer_.erase(audio_buffer_.begin(),
                            audio_buffer_.end() - max_buffer);
    }
}
```

---

## Task D.3: APU Processing Loop

**File**: `native/src/apu/android_audio.cpp` (or new file)

### D.3.1: Process XMA Contexts

The Xbox 360 APU has multiple XMA contexts (up to 96):

```cpp
void Apu::process() {
    // Check each active XMA context
    for (u32 i = 0; i < kMaxXmaContexts; i++) {
        if (!contexts_[i].active) continue;

        XmaContext& ctx = contexts_[i];

        // Check if context needs more input or has output ready
        if (ctx.has_output_ready()) {
            DecodedAudio audio = decoder_.decode(ctx, memory_);
            android_audio_.submit_audio(audio);
            ctx.clear_output();
        }
    }
}
```

### D.3.2: XMA Context Memory Layout

Xbox 360 games write XMA context structures to specific memory addresses:

```cpp
// XMA context structure (from Xbox 360 SDK)
struct XmaContextMemory {
    u32 input_buffer_0_ptr;
    u32 input_buffer_0_size;
    u32 input_buffer_1_ptr;
    u32 input_buffer_1_size;
    u32 output_buffer_ptr;
    u32 output_buffer_size;
    u32 work_buffer_ptr;
    u8  input_buffer_read_offset;
    u8  current_buffer;  // 0 or 1
    u8  output_buffer_write_offset;
    u8  output_buffer_read_offset;
    // ... more fields
};

void Apu::update_context(u32 index) {
    // Read context from guest memory
    u32 context_addr = kXmaContextBase + index * sizeof(XmaContextMemory);
    XmaContextMemory mem;
    memory_->read_bytes(context_addr, &mem, sizeof(mem));

    // Update internal state
    contexts_[index].input_ptr = mem.input_buffer_0_ptr;
    contexts_[index].input_size = mem.input_buffer_0_size;
    // ... etc
}
```

---

## Testing

### Unit Test

Create a simple test that decodes a known XMA file:

```cpp
// In test_audio.cpp
TEST(Audio, XmaDecodeBasic) {
    // Load test XMA data
    std::vector<u8> xma_data = load_test_xma();

    XmaDecoder decoder;
    XmaContext ctx;
    ctx.sample_rate = 44100;
    ctx.channels = 2;
    // ... setup

    auto result = decoder.decode(ctx, &test_memory);

    EXPECT_GT(result.samples.size(), 0);
    EXPECT_EQ(result.sample_rate, 44100);
}
```

### Integration Test

On device with a game:

1. Add logging to `submit_audio()` to see if audio is being submitted
2. Check AAudio stream state
3. Verify audio callback is being called

```bash
adb logcat | grep -i "audio\|xma\|apu"
```

## Reference Files

- `native/src/apu/xma_decoder.h` - See existing decoder interface
- `native/src/apu/android_audio.h` - See Android audio interface
- `native/src/apu/audio.h` - See APU interface
- Xenia source code - Reference XMA decoder implementation

## Notes

- XMA decoding is CPU-intensive; consider running on separate thread
- Audio latency matters for gameplay - target < 50ms
- Some games use raw PCM instead of XMA - handle both
- Xbox 360 also has XMA2 format - similar but improved
- Consider using Oboe library instead of raw AAudio for easier setup
