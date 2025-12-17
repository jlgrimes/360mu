# Task: Audio/XMA Decoder Implementation

## Project Context
You are working on 360μ, an Xbox 360 emulator for Android. Xbox 360 games use XMA (Xbox Media Audio) format which is a modified WMA Pro codec.

## Your Assignment
Implement XMA audio decoding and Android audio output using AAudio.

## Current State
- Stub implementation at `native/src/apu/stub/apu_stub.cpp`
- APU header at `native/src/apu/audio.h`
- XMA decoder header at `native/src/apu/xma_decoder.h`
- Android audio header at `native/src/apu/android_audio.h`

## Files to Implement

### 1. `native/src/apu/xma_decoder.cpp`
```cpp
// XMA decoding using FFmpeg:
class XmaDecoder {
    // Initialize FFmpeg decoder
    Status initialize() {
        // Find WMAPro decoder (XMA is based on it)
        codec = avcodec_find_decoder(AV_CODEC_ID_WMAPRO);
        // Configure for XMA specifics
    }
    
    // Decode XMA packet to PCM
    Status decode(const void* xma_data, u32 size,
                  s16* pcm_out, u32 max_samples,
                  u32& samples_decoded) {
        // Parse XMA packet header
        // Feed to FFmpeg
        // Get PCM output
    }
};
```

### 2. `native/src/apu/android_audio.cpp`
```cpp
// AAudio output for Android:
class AndroidAudioOutput {
    AAudioStream* stream;
    
    Status initialize(u32 sample_rate, u32 channels) {
        AAudioStreamBuilder* builder;
        AAudio_createStreamBuilder(&builder);
        AAudioStreamBuilder_setSampleRate(builder, sample_rate);
        AAudioStreamBuilder_setChannelCount(builder, channels);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setPerformanceMode(builder, 
            AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_openStream(builder, &stream);
    }
    
    Status write(const s16* samples, u32 count) {
        AAudioStream_write(stream, samples, count, timeout_ns);
    }
};
```

### 3. `native/src/apu/mixer.cpp`
```cpp
// Audio mixing:
class AudioMixer {
    // Mix multiple voices to stereo output
    void mix(const std::vector<Voice>& voices,
             s16* output, u32 sample_count) {
        // For each voice:
        //   Apply volume
        //   Resample if needed
        //   Mix to output
    }
};
```

## XMA Format Details
```
XMA Packet Header (4 bytes):
  bits 0-5:   Frame count (1-64)
  bits 6-7:   Unknown
  bits 8-22:  Skip samples
  bits 23-25: Metadata
  bits 26-31: Packet skip count

XMA is essentially WMA Pro with:
- Different container format
- Hardware-specific optimizations
- Loop point support
```

## Build & Test
```bash
# Build with FFmpeg:
cd native/build
cmake .. -DX360MU_USE_FFMPEG=ON
make -j4

# Need FFmpeg installed:
# macOS: brew install ffmpeg
# Linux: apt install libavcodec-dev libavformat-dev

# Test:
./x360mu_tests --gtest_filter=Audio*
```

## FFmpeg Integration
```cpp
// Required FFmpeg includes:
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

// Link flags:
// -lavcodec -lavformat -lavutil -lswresample
```

## Reference
- XMA format: https://wiki.multimedia.cx/index.php/XMA
- Xenia XMA: https://github.com/xenia-project/xenia/tree/master/src/xenia/apu
- AAudio docs: https://developer.android.com/ndk/guides/audio/aaudio

## Success Criteria
1. Can decode XMA packets to PCM
2. Can output audio on Android via AAudio
3. Low latency (<50ms)
4. Multiple simultaneous voices mixed correctly

