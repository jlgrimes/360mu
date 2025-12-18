/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Audio subsystem unit tests
 * 
 * Tests XMA decoder, audio mixer, Android audio output, and full APU pipeline.
 */

#include <gtest/gtest.h>
#include "apu/audio.h"
#include "apu/xma_decoder.h"
#include "apu/android_audio.h"
#include <cmath>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>

namespace x360mu {
namespace test {

//=============================================================================
// XMA Decoder Tests
//=============================================================================

class XmaDecoderTest : public ::testing::Test {
protected:
    void SetUp() override {
        decoder_ = std::make_unique<XmaDecoder>();
        ASSERT_EQ(decoder_->initialize(), Status::Ok);
    }
    
    void TearDown() override {
        decoder_->shutdown();
        decoder_.reset();
    }
    
    std::unique_ptr<XmaDecoder> decoder_;
};

TEST_F(XmaDecoderTest, Initialize) {
    // Test already done in SetUp
    SUCCEED();
}

TEST_F(XmaDecoderTest, CreateContext) {
    u32 ctx_id = decoder_->create_context(48000, 2);
    ASSERT_NE(ctx_id, UINT32_MAX);
    
    decoder_->destroy_context(ctx_id);
}

TEST_F(XmaDecoderTest, CreateMultipleContexts) {
    std::vector<u32> contexts;
    
    // Create several contexts
    for (int i = 0; i < 10; i++) {
        u32 ctx_id = decoder_->create_context(48000, 2);
        ASSERT_NE(ctx_id, UINT32_MAX);
        contexts.push_back(ctx_id);
    }
    
    // Destroy them
    for (u32 ctx_id : contexts) {
        decoder_->destroy_context(ctx_id);
    }
}

TEST_F(XmaDecoderTest, DecodeEmptyData) {
    // Should handle empty data gracefully
    std::vector<s16> output = decoder_->decode(nullptr, 0, 48000, 2);
    EXPECT_TRUE(output.empty());
}

TEST_F(XmaDecoderTest, DecodeInvalidData) {
    // Should handle invalid data gracefully
    u8 garbage[128];
    memset(garbage, 0xAB, sizeof(garbage));
    
    std::vector<s16> output = decoder_->decode(garbage, sizeof(garbage), 48000, 2);
    // May produce output or not, but shouldn't crash
}

TEST_F(XmaDecoderTest, ContextBufferManagement) {
    u32 ctx_id = decoder_->create_context(48000, 2);
    ASSERT_NE(ctx_id, UINT32_MAX);
    
    // Set input buffers
    decoder_->set_input_buffer(ctx_id, 0x10000, 2048, 0);
    decoder_->set_input_buffer(ctx_id, 0x20000, 2048, 1);
    
    // Set output buffer
    decoder_->set_output_buffer(ctx_id, 0x30000, 8192);
    
    // Start and stop
    decoder_->start_context(ctx_id);
    EXPECT_FALSE(decoder_->is_buffer_done(ctx_id, 0));
    decoder_->stop_context(ctx_id);
    
    decoder_->destroy_context(ctx_id);
}

//=============================================================================
// Audio Mixer Tests
//=============================================================================

class AudioMixerTest : public ::testing::Test {
protected:
    void SetUp() override {
        mixer_ = std::make_unique<AudioMixer>();
        ASSERT_EQ(mixer_->initialize(48000, 1024), Status::Ok);
    }
    
    void TearDown() override {
        mixer_->shutdown();
        mixer_.reset();
    }
    
    std::unique_ptr<AudioMixer> mixer_;
};

TEST_F(AudioMixerTest, Initialize) {
    SUCCEED();
}

TEST_F(AudioMixerTest, CreateVoice) {
    u32 voice_id = mixer_->create_voice(48000, 2);
    ASSERT_NE(voice_id, UINT32_MAX);
    
    mixer_->destroy_voice(voice_id);
}

TEST_F(AudioMixerTest, CreateMultipleVoices) {
    std::vector<u32> voices;
    
    for (int i = 0; i < 32; i++) {
        u32 voice_id = mixer_->create_voice(48000, 2);
        ASSERT_NE(voice_id, UINT32_MAX);
        voices.push_back(voice_id);
    }
    
    for (u32 voice_id : voices) {
        mixer_->destroy_voice(voice_id);
    }
}

TEST_F(AudioMixerTest, SubmitSamples) {
    u32 voice_id = mixer_->create_voice(48000, 2);
    ASSERT_NE(voice_id, UINT32_MAX);
    
    // Generate test tone (440Hz sine wave)
    std::vector<s16> test_samples(4800); // 100ms at 48kHz mono
    for (size_t i = 0; i < test_samples.size(); i++) {
        f32 t = static_cast<f32>(i) / 48000.0f;
        test_samples[i] = static_cast<s16>(std::sin(2.0f * 3.14159f * 440.0f * t) * 16000.0f);
    }
    
    mixer_->submit_samples(voice_id, test_samples.data(), test_samples.size());
    
    mixer_->destroy_voice(voice_id);
}

TEST_F(AudioMixerTest, VolumeControl) {
    u32 voice_id = mixer_->create_voice(48000, 2);
    ASSERT_NE(voice_id, UINT32_MAX);
    
    // Test valid volume range
    mixer_->set_voice_volume(voice_id, 0.5f);
    mixer_->set_voice_volume(voice_id, 0.0f);
    mixer_->set_voice_volume(voice_id, 1.0f);
    
    // Test clamping
    mixer_->set_voice_volume(voice_id, -1.0f);  // Should clamp to 0
    mixer_->set_voice_volume(voice_id, 2.0f);   // Should clamp to 1
    
    mixer_->destroy_voice(voice_id);
}

TEST_F(AudioMixerTest, PanControl) {
    u32 voice_id = mixer_->create_voice(48000, 2);
    ASSERT_NE(voice_id, UINT32_MAX);
    
    mixer_->set_voice_pan(voice_id, -1.0f);  // Full left
    mixer_->set_voice_pan(voice_id, 0.0f);   // Center
    mixer_->set_voice_pan(voice_id, 1.0f);   // Full right
    
    mixer_->destroy_voice(voice_id);
}

TEST_F(AudioMixerTest, MasterVolume) {
    mixer_->set_master_volume(0.5f);
    mixer_->set_master_volume(1.0f);
    mixer_->set_master_volume(0.0f);
}

TEST_F(AudioMixerTest, GetOutput) {
    u32 voice_id = mixer_->create_voice(48000, 2);
    ASSERT_NE(voice_id, UINT32_MAX);
    
    // Submit some samples
    std::vector<s16> test_samples(960); // 20ms stereo
    for (size_t i = 0; i < test_samples.size(); i += 2) {
        f32 t = static_cast<f32>(i / 2) / 48000.0f;
        s16 sample = static_cast<s16>(std::sin(2.0f * 3.14159f * 440.0f * t) * 16000.0f);
        test_samples[i] = sample;
        test_samples[i + 1] = sample;
    }
    mixer_->submit_samples(voice_id, test_samples.data(), test_samples.size() / 2);
    
    // Get output
    std::vector<s16> output(512);
    u32 frames = mixer_->get_output(output.data(), 256);
    EXPECT_EQ(frames, 256u);
    
    mixer_->destroy_voice(voice_id);
}

TEST_F(AudioMixerTest, PauseResume) {
    mixer_->pause();
    mixer_->resume();
}

//=============================================================================
// XMA Processor Tests
//=============================================================================

class XmaProcessorTest : public ::testing::Test {
protected:
    void SetUp() override {
        processor_ = std::make_unique<XmaProcessor>();
        mixer_ = std::make_unique<AudioMixer>();
        
        ASSERT_EQ(mixer_->initialize(48000, 1024), Status::Ok);
        // Note: processor needs memory pointer which we don't have in tests
        // So we'll test what we can without full memory integration
        ASSERT_EQ(processor_->initialize(nullptr, mixer_.get()), Status::Ok);
    }
    
    void TearDown() override {
        processor_->shutdown();
        mixer_->shutdown();
        processor_.reset();
        mixer_.reset();
    }
    
    std::unique_ptr<XmaProcessor> processor_;
    std::unique_ptr<AudioMixer> mixer_;
};

TEST_F(XmaProcessorTest, Initialize) {
    SUCCEED();
}

TEST_F(XmaProcessorTest, CreateContext) {
    u32 ctx_id = processor_->create_context();
    ASSERT_NE(ctx_id, UINT32_MAX);
    
    processor_->destroy_context(ctx_id);
}

TEST_F(XmaProcessorTest, CreateMultipleContexts) {
    std::vector<u32> contexts;
    
    // Create several contexts
    for (int i = 0; i < 16; i++) {
        u32 ctx_id = processor_->create_context();
        ASSERT_NE(ctx_id, UINT32_MAX);
        contexts.push_back(ctx_id);
    }
    
    // Verify each context is unique
    for (size_t i = 0; i < contexts.size(); i++) {
        for (size_t j = i + 1; j < contexts.size(); j++) {
            EXPECT_NE(contexts[i], contexts[j]);
        }
    }
    
    // Destroy them
    for (u32 ctx_id : contexts) {
        processor_->destroy_context(ctx_id);
    }
}

TEST_F(XmaProcessorTest, GetContext) {
    u32 ctx_id = processor_->create_context();
    ASSERT_NE(ctx_id, UINT32_MAX);
    
    // Get context should return valid pointer
    XmaContext* ctx = processor_->get_context(ctx_id);
    ASSERT_NE(ctx, nullptr);
    
    // Invalid context should return null
    EXPECT_EQ(processor_->get_context(UINT32_MAX), nullptr);
    EXPECT_EQ(processor_->get_context(999), nullptr);
    
    processor_->destroy_context(ctx_id);
}

TEST_F(XmaProcessorTest, SetInputBuffer) {
    u32 ctx_id = processor_->create_context();
    ASSERT_NE(ctx_id, UINT32_MAX);
    
    // Set both input buffers (double buffering)
    processor_->set_input_buffer(ctx_id, 0x10000, 2048, 0);
    processor_->set_input_buffer(ctx_id, 0x12000, 2048, 1);
    
    XmaContext* ctx = processor_->get_context(ctx_id);
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(ctx->input_buffer_0, 0x10000u);
    EXPECT_EQ(ctx->input_buffer_0_size, 2048u);
    EXPECT_EQ(ctx->input_buffer_1, 0x12000u);
    EXPECT_EQ(ctx->input_buffer_1_size, 2048u);
    
    processor_->destroy_context(ctx_id);
}

TEST_F(XmaProcessorTest, SetOutputBuffer) {
    u32 ctx_id = processor_->create_context();
    ASSERT_NE(ctx_id, UINT32_MAX);
    
    processor_->set_output_buffer(ctx_id, 0x20000, 8192);
    
    XmaContext* ctx = processor_->get_context(ctx_id);
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(ctx->output_buffer, 0x20000u);
    EXPECT_EQ(ctx->output_buffer_size, 8192u);
    
    processor_->destroy_context(ctx_id);
}

TEST_F(XmaProcessorTest, ContextConfiguration) {
    u32 ctx_id = processor_->create_context();
    ASSERT_NE(ctx_id, UINT32_MAX);
    
    // Set configuration
    processor_->set_context_sample_rate(ctx_id, 44100);
    processor_->set_context_channels(ctx_id, 1);
    processor_->set_context_loop(ctx_id, true, 0x100, 0x500);
    
    XmaContext* ctx = processor_->get_context(ctx_id);
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(ctx->sample_rate, 44100u);
    EXPECT_EQ(ctx->num_channels, 1u);
    EXPECT_TRUE(ctx->loop_enabled);
    EXPECT_EQ(ctx->loop_start_offset, 0x100u);
    EXPECT_EQ(ctx->loop_end_offset, 0x500u);
    
    processor_->destroy_context(ctx_id);
}

TEST_F(XmaProcessorTest, EnableDisableContext) {
    u32 ctx_id = processor_->create_context();
    ASSERT_NE(ctx_id, UINT32_MAX);
    
    // Initially inactive
    EXPECT_FALSE(processor_->is_context_active(ctx_id));
    
    // Enable
    processor_->enable_context(ctx_id);
    EXPECT_TRUE(processor_->is_context_active(ctx_id));
    
    // Disable
    processor_->disable_context(ctx_id);
    EXPECT_FALSE(processor_->is_context_active(ctx_id));
    
    processor_->destroy_context(ctx_id);
}

TEST_F(XmaProcessorTest, InputBufferConsumed) {
    u32 ctx_id = processor_->create_context();
    ASSERT_NE(ctx_id, UINT32_MAX);
    
    // Set input buffers with zero size (immediately consumed)
    processor_->set_input_buffer(ctx_id, 0x10000, 0, 0);
    processor_->set_input_buffer(ctx_id, 0x12000, 0, 1);
    
    // Buffer 0 with size 0 should be considered consumed when context is active
    processor_->enable_context(ctx_id);
    
    // Without memory, buffers will be marked consumed
    EXPECT_TRUE(processor_->is_input_buffer_consumed(ctx_id, 0));
    EXPECT_TRUE(processor_->is_input_buffer_consumed(ctx_id, 1));
    
    processor_->destroy_context(ctx_id);
}

TEST_F(XmaProcessorTest, Statistics) {
    // Create a few contexts and check stats
    u32 ctx1 = processor_->create_context();
    u32 ctx2 = processor_->create_context();
    
    processor_->enable_context(ctx1);
    processor_->enable_context(ctx2);
    
    auto stats = processor_->get_stats();
    EXPECT_EQ(stats.active_contexts, 2u);
    
    processor_->disable_context(ctx1);
    
    // Stats update after process
    processor_->process();
    stats = processor_->get_stats();
    EXPECT_EQ(stats.active_contexts, 1u);
    
    processor_->destroy_context(ctx1);
    processor_->destroy_context(ctx2);
}

TEST_F(XmaProcessorTest, InvalidContextOperations) {
    // Operations on invalid context IDs should not crash
    processor_->set_input_buffer(UINT32_MAX, 0x10000, 2048, 0);
    processor_->set_output_buffer(UINT32_MAX, 0x20000, 8192);
    processor_->enable_context(UINT32_MAX);
    processor_->disable_context(UINT32_MAX);
    
    EXPECT_FALSE(processor_->is_context_active(UINT32_MAX));
    EXPECT_TRUE(processor_->is_input_buffer_consumed(UINT32_MAX, 0));
    EXPECT_EQ(processor_->get_output_write_offset(UINT32_MAX), 0u);
    
    SUCCEED();
}

TEST_F(XmaProcessorTest, ProcessWithoutMemory) {
    u32 ctx_id = processor_->create_context();
    ASSERT_NE(ctx_id, UINT32_MAX);
    
    processor_->set_input_buffer(ctx_id, 0x10000, 2048, 0);
    processor_->set_output_buffer(ctx_id, 0x20000, 8192);
    processor_->enable_context(ctx_id);
    
    // Process should not crash even without memory
    processor_->process();
    
    processor_->destroy_context(ctx_id);
}

//=============================================================================
// Android Audio Output Tests
//=============================================================================

class AndroidAudioOutputTest : public ::testing::Test {
protected:
    void SetUp() override {
        audio_output_ = std::make_unique<AndroidAudioOutput>();
    }
    
    void TearDown() override {
        if (audio_output_) {
            audio_output_->shutdown();
        }
        audio_output_.reset();
    }
    
    std::unique_ptr<AndroidAudioOutput> audio_output_;
};

TEST_F(AndroidAudioOutputTest, Initialize) {
    AudioConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    config.buffer_frames = 256;
    config.buffer_count = 4;
    
    Status status = audio_output_->initialize(config);
    EXPECT_EQ(status, Status::Ok);
}

TEST_F(AndroidAudioOutputTest, StartStop) {
    AudioConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    config.buffer_frames = 256;
    
    ASSERT_EQ(audio_output_->initialize(config), Status::Ok);
    
    EXPECT_EQ(audio_output_->start(), Status::Ok);
    EXPECT_TRUE(audio_output_->is_playing());
    
    audio_output_->stop();
    EXPECT_FALSE(audio_output_->is_playing());
}

TEST_F(AndroidAudioOutputTest, PauseResume) {
    AudioConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    
    ASSERT_EQ(audio_output_->initialize(config), Status::Ok);
    ASSERT_EQ(audio_output_->start(), Status::Ok);
    
    audio_output_->pause();
    audio_output_->resume();
    
    EXPECT_TRUE(audio_output_->is_playing());
}

TEST_F(AndroidAudioOutputTest, VolumeControl) {
    AudioConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    
    ASSERT_EQ(audio_output_->initialize(config), Status::Ok);
    
    audio_output_->set_volume(0.5f);
    EXPECT_FLOAT_EQ(audio_output_->get_volume(), 0.5f);
    
    audio_output_->set_volume(0.0f);
    EXPECT_FLOAT_EQ(audio_output_->get_volume(), 0.0f);
    
    audio_output_->set_volume(1.0f);
    EXPECT_FLOAT_EQ(audio_output_->get_volume(), 1.0f);
}

TEST_F(AndroidAudioOutputTest, QueueSamples) {
    AudioConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    config.buffer_frames = 256;
    
    ASSERT_EQ(audio_output_->initialize(config), Status::Ok);
    ASSERT_EQ(audio_output_->start(), Status::Ok);
    
    // Generate test samples (silence)
    std::vector<f32> samples(256 * 2, 0.0f);
    
    u32 queued = audio_output_->queue_samples(samples.data(), 256);
    EXPECT_GT(queued, 0u);
}

TEST_F(AndroidAudioOutputTest, Latency) {
    AudioConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    config.buffer_frames = 256;
    config.buffer_count = 4;
    
    ASSERT_EQ(audio_output_->initialize(config), Status::Ok);
    
    f32 latency = audio_output_->get_latency_ms();
    
    // Latency should be reasonable (between 5ms and 100ms)
    EXPECT_GE(latency, 5.0f);
    EXPECT_LE(latency, 100.0f);
}

//=============================================================================
// Audio Ring Buffer Tests
//=============================================================================

class AudioRingBufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        buffer_ = std::make_unique<AudioRingBuffer>(1024, 2);
    }
    
    std::unique_ptr<AudioRingBuffer> buffer_;
};

TEST_F(AudioRingBufferTest, WriteRead) {
    std::vector<f32> write_data(100 * 2);
    for (size_t i = 0; i < write_data.size(); i++) {
        write_data[i] = static_cast<f32>(i) / write_data.size();
    }
    
    u32 written = buffer_->write(write_data.data(), 100);
    EXPECT_EQ(written, 100u);
    
    std::vector<f32> read_data(100 * 2);
    u32 read = buffer_->read(read_data.data(), 100);
    EXPECT_EQ(read, 100u);
    
    // Verify data integrity
    for (size_t i = 0; i < read_data.size(); i++) {
        EXPECT_FLOAT_EQ(read_data[i], write_data[i]);
    }
}

TEST_F(AudioRingBufferTest, AvailableSpace) {
    EXPECT_EQ(buffer_->available_read(), 0u);
    EXPECT_GT(buffer_->available_write(), 0u);
    
    std::vector<f32> data(512 * 2, 0.0f);
    buffer_->write(data.data(), 512);
    
    EXPECT_EQ(buffer_->available_read(), 512u);
}

TEST_F(AudioRingBufferTest, Clear) {
    std::vector<f32> data(256 * 2, 1.0f);
    buffer_->write(data.data(), 256);
    
    EXPECT_GT(buffer_->available_read(), 0u);
    
    buffer_->clear();
    
    EXPECT_EQ(buffer_->available_read(), 0u);
}

TEST_F(AudioRingBufferTest, Wraparound) {
    std::vector<f32> write_data(800 * 2, 0.5f);
    std::vector<f32> read_data(800 * 2);
    
    // Write, read, write again to cause wraparound
    buffer_->write(write_data.data(), 800);
    buffer_->read(read_data.data(), 600);
    buffer_->write(write_data.data(), 600);
    
    // Should have 800 frames available
    EXPECT_EQ(buffer_->available_read(), 800u);
}

//=============================================================================
// Audio Resampler Tests
//=============================================================================

class AudioResamplerTest : public ::testing::Test {
protected:
    AudioResampler resampler_;
};

TEST_F(AudioResamplerTest, NoResampling) {
    resampler_.configure(48000, 48000, 2);
    
    std::vector<f32> input(100 * 2);
    std::vector<f32> output(100 * 2);
    
    for (size_t i = 0; i < input.size(); i++) {
        input[i] = static_cast<f32>(i);
    }
    
    u32 output_frames = resampler_.process(input.data(), 100, output.data(), 100);
    EXPECT_EQ(output_frames, 100u);
}

TEST_F(AudioResamplerTest, Downsample) {
    resampler_.configure(48000, 24000, 2);  // 2x downsample
    
    std::vector<f32> input(100 * 2);
    std::vector<f32> output(100 * 2);
    
    for (size_t i = 0; i < input.size(); i++) {
        input[i] = std::sin(static_cast<f32>(i) * 0.1f);
    }
    
    u32 output_frames = resampler_.process(input.data(), 100, output.data(), 100);
    
    // Should produce approximately half the frames
    EXPECT_GE(output_frames, 40u);
    EXPECT_LE(output_frames, 60u);
}

TEST_F(AudioResamplerTest, Upsample) {
    resampler_.configure(24000, 48000, 2);  // 2x upsample
    
    std::vector<f32> input(100 * 2);
    std::vector<f32> output(300 * 2);  // Larger output buffer
    
    for (size_t i = 0; i < input.size(); i++) {
        input[i] = std::sin(static_cast<f32>(i) * 0.1f);
    }
    
    u32 output_frames = resampler_.process(input.data(), 100, output.data(), 300);
    
    // Should produce approximately double the frames
    EXPECT_GE(output_frames, 150u);
    EXPECT_LE(output_frames, 250u);
}

TEST_F(AudioResamplerTest, GetOutputFrames) {
    resampler_.configure(48000, 44100, 2);
    
    u32 expected = resampler_.get_output_frames(1000);
    
    // 44100/48000 * 1000 ≈ 919
    EXPECT_GE(expected, 900u);
    EXPECT_LE(expected, 940u);
}

//=============================================================================
// Simple Audio Mixer Tests
//=============================================================================

class SimpleAudioMixerTest : public ::testing::Test {
protected:
    void SetUp() override {
        mixer_.configure(48000, 2);
    }
    
    SimpleAudioMixer mixer_;
};

TEST_F(SimpleAudioMixerTest, AddRemoveSource) {
    std::vector<f32> samples(1000 * 2, 0.5f);
    
    int source_id = mixer_.add_source(samples.data(), 1000, 1.0f, false);
    EXPECT_GE(source_id, 0);
    
    mixer_.remove_source(source_id);
}

TEST_F(SimpleAudioMixerTest, MixSingleSource) {
    std::vector<f32> samples(100 * 2);
    for (size_t i = 0; i < samples.size(); i++) {
        samples[i] = 0.5f;
    }
    
    int source_id = mixer_.add_source(samples.data(), 100, 1.0f, false);
    ASSERT_GE(source_id, 0);
    
    std::vector<f32> output(50 * 2, 0.0f);
    mixer_.mix(output.data(), 50);
    
    // Check that output has non-zero samples
    bool has_audio = false;
    for (f32 sample : output) {
        if (sample != 0.0f) {
            has_audio = true;
            break;
        }
    }
    EXPECT_TRUE(has_audio);
    
    mixer_.remove_source(source_id);
}

TEST_F(SimpleAudioMixerTest, MasterVolume) {
    std::vector<f32> samples(100 * 2, 1.0f);
    
    int source_id = mixer_.add_source(samples.data(), 100, 1.0f, false);
    ASSERT_GE(source_id, 0);
    
    // Set master volume to 0
    mixer_.set_master_volume(0.0f);
    
    std::vector<f32> output(50 * 2, 1.0f);
    mixer_.mix(output.data(), 50);
    
    // Output should be silent
    for (f32 sample : output) {
        EXPECT_FLOAT_EQ(sample, 0.0f);
    }
    
    mixer_.remove_source(source_id);
}

TEST_F(SimpleAudioMixerTest, SourceVolume) {
    std::vector<f32> samples(100 * 2, 1.0f);
    
    int source_id = mixer_.add_source(samples.data(), 100, 0.5f, false);
    ASSERT_GE(source_id, 0);
    
    mixer_.set_source_volume(source_id, 0.25f);
    
    // Volume change should not crash
    SUCCEED();
    
    mixer_.remove_source(source_id);
}

TEST_F(SimpleAudioMixerTest, SourcePan) {
    std::vector<f32> samples(100 * 2, 0.5f);
    
    int source_id = mixer_.add_source(samples.data(), 100, 1.0f, false);
    ASSERT_GE(source_id, 0);
    
    mixer_.set_source_pan(source_id, -1.0f);  // Full left
    mixer_.set_source_pan(source_id, 1.0f);   // Full right
    mixer_.set_source_pan(source_id, 0.0f);   // Center
    
    SUCCEED();
    
    mixer_.remove_source(source_id);
}

//=============================================================================
// Full APU Tests
//=============================================================================

class ApuTest : public ::testing::Test {
protected:
    void SetUp() override {
        apu_ = std::make_unique<Apu>();
    }
    
    void TearDown() override {
        if (apu_) {
            apu_->shutdown();
        }
        apu_.reset();
    }
    
    std::unique_ptr<Apu> apu_;
};

TEST_F(ApuTest, Initialize) {
    ApuConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    config.buffer_size_ms = 20;
    
    // Initialize without memory (testing basic functionality)
    Status status = apu_->initialize(nullptr, config);
    EXPECT_EQ(status, Status::Ok);
}

TEST_F(ApuTest, CreateDestroyContext) {
    ApuConfig config;
    ASSERT_EQ(apu_->initialize(nullptr, config), Status::Ok);
    
    ApuXmaContext ctx = {};
    ctx.valid = true;
    
    Status status = apu_->create_context(0, ctx);
    EXPECT_EQ(status, Status::Ok);
    
    ApuXmaContext* retrieved = apu_->get_context(0);
    EXPECT_NE(retrieved, nullptr);
    
    apu_->destroy_context(0);
    
    // Context should be cleared
    retrieved = apu_->get_context(0);
    EXPECT_NE(retrieved, nullptr);  // Still valid pointer
    EXPECT_FALSE(retrieved->valid);  // But marked invalid
}

TEST_F(ApuTest, CreateDestroyVoice) {
    ApuConfig config;
    ASSERT_EQ(apu_->initialize(nullptr, config), Status::Ok);
    
    u32 voice_id = apu_->create_voice(0);
    EXPECT_NE(voice_id, ~0u);
    
    apu_->destroy_voice(voice_id);
}

TEST_F(ApuTest, VoiceVolume) {
    ApuConfig config;
    ASSERT_EQ(apu_->initialize(nullptr, config), Status::Ok);
    
    u32 voice_id = apu_->create_voice(0);
    ASSERT_NE(voice_id, ~0u);
    
    apu_->set_voice_volume(voice_id, 0.5f, 0.5f);
    apu_->set_voice_volume(voice_id, 1.0f, 0.0f);  // Full left
    apu_->set_voice_volume(voice_id, 0.0f, 1.0f);  // Full right
    
    apu_->destroy_voice(voice_id);
}

TEST_F(ApuTest, VoicePitch) {
    ApuConfig config;
    ASSERT_EQ(apu_->initialize(nullptr, config), Status::Ok);
    
    u32 voice_id = apu_->create_voice(0);
    ASSERT_NE(voice_id, ~0u);
    
    apu_->set_voice_pitch(voice_id, 1.0f);
    apu_->set_voice_pitch(voice_id, 0.5f);
    apu_->set_voice_pitch(voice_id, 2.0f);
    
    apu_->destroy_voice(voice_id);
}

TEST_F(ApuTest, VoiceStartStop) {
    ApuConfig config;
    ASSERT_EQ(apu_->initialize(nullptr, config), Status::Ok);
    
    u32 voice_id = apu_->create_voice(0);
    ASSERT_NE(voice_id, ~0u);
    
    apu_->start_voice(voice_id);
    apu_->stop_voice(voice_id);
    
    apu_->destroy_voice(voice_id);
}

TEST_F(ApuTest, Process) {
    ApuConfig config;
    ASSERT_EQ(apu_->initialize(nullptr, config), Status::Ok);
    
    // Process should not crash even without contexts
    apu_->process();
    
    // Create context and voice
    ApuXmaContext ctx = {};
    apu_->create_context(0, ctx);
    u32 voice_id = apu_->create_voice(0);
    
    // Process should still work
    apu_->process();
    
    apu_->destroy_voice(voice_id);
    apu_->destroy_context(0);
}

TEST_F(ApuTest, GetSamples) {
    ApuConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    ASSERT_EQ(apu_->initialize(nullptr, config), Status::Ok);
    
    std::vector<s16> buffer(256 * 2);
    u32 samples = apu_->get_samples(buffer.data(), 256);
    
    EXPECT_EQ(samples, 256u);
}

TEST_F(ApuTest, Reset) {
    ApuConfig config;
    ASSERT_EQ(apu_->initialize(nullptr, config), Status::Ok);
    
    // Create some state
    ApuXmaContext ctx = {};
    ctx.valid = true;
    apu_->create_context(0, ctx);
    u32 voice_id = apu_->create_voice(0);
    
    // Reset
    apu_->reset();
    
    // State should be cleared
    ApuXmaContext* retrieved = apu_->get_context(0);
    EXPECT_FALSE(retrieved->valid);
    
    apu_->destroy_voice(voice_id);
}

TEST_F(ApuTest, Statistics) {
    ApuConfig config;
    ASSERT_EQ(apu_->initialize(nullptr, config), Status::Ok);
    
    auto stats = apu_->get_stats();
    
    // Initial stats should be zero
    EXPECT_EQ(stats.samples_generated, 0u);
    EXPECT_EQ(stats.xma_packets_decoded, 0u);
}

TEST_F(ApuTest, MultipleContexts) {
    ApuConfig config;
    ASSERT_EQ(apu_->initialize(nullptr, config), Status::Ok);
    
    // Create multiple contexts
    for (u32 i = 0; i < 16; i++) {
        ApuXmaContext ctx = {};
        ctx.valid = true;
        Status status = apu_->create_context(i, ctx);
        EXPECT_EQ(status, Status::Ok);
    }
    
    // Destroy them
    for (u32 i = 0; i < 16; i++) {
        apu_->destroy_context(i);
    }
}

TEST_F(ApuTest, MultipleVoices) {
    ApuConfig config;
    ASSERT_EQ(apu_->initialize(nullptr, config), Status::Ok);
    
    std::vector<u32> voice_ids;
    
    // Create multiple voices
    for (int i = 0; i < 32; i++) {
        u32 voice_id = apu_->create_voice(i % 16);
        ASSERT_NE(voice_id, ~0u);
        voice_ids.push_back(voice_id);
    }
    
    // Process with multiple voices
    apu_->process();
    
    // Destroy voices
    for (u32 voice_id : voice_ids) {
        apu_->destroy_voice(voice_id);
    }
}

//=============================================================================
// Integration Tests
//=============================================================================

TEST(AudioIntegration, FullPipelineNoMemory) {
    // Test the full pipeline without actual memory
    
    // Create components
    auto xma_processor = std::make_unique<XmaProcessor>();
    auto audio_mixer = std::make_unique<AudioMixer>();
    
    ASSERT_EQ(audio_mixer->initialize(48000, 1024), Status::Ok);
    ASSERT_EQ(xma_processor->initialize(nullptr, audio_mixer.get()), Status::Ok);
    
    // Create context
    u32 ctx_id = xma_processor->create_context();
    ASSERT_NE(ctx_id, UINT32_MAX);
    
    // Configure context
    xma_processor->set_context_sample_rate(ctx_id, 48000);
    xma_processor->set_context_channels(ctx_id, 2);
    xma_processor->set_input_buffer(ctx_id, 0x10000, 2048, 0);
    xma_processor->set_output_buffer(ctx_id, 0x20000, 8192);
    
    // Enable and process
    xma_processor->enable_context(ctx_id);
    xma_processor->process();
    
    // Get output
    std::vector<s16> output(1024);
    u32 frames = audio_mixer->get_output(output.data(), 512);
    EXPECT_EQ(frames, 512u);
    
    // Cleanup
    xma_processor->destroy_context(ctx_id);
    xma_processor->shutdown();
    audio_mixer->shutdown();
}

TEST(AudioIntegration, SineWaveGeneration) {
    // Test generating and mixing a sine wave
    
    auto audio_mixer = std::make_unique<AudioMixer>();
    ASSERT_EQ(audio_mixer->initialize(48000, 1024), Status::Ok);
    
    // Create voice
    u32 voice_id = audio_mixer->create_voice(48000, 2);
    ASSERT_NE(voice_id, UINT32_MAX);
    
    // Generate 440Hz sine wave
    std::vector<s16> sine_samples(4800);  // 100ms at 48kHz mono
    for (size_t i = 0; i < sine_samples.size(); i++) {
        f32 t = static_cast<f32>(i) / 48000.0f;
        sine_samples[i] = static_cast<s16>(std::sin(2.0f * 3.14159f * 440.0f * t) * 16000.0f);
    }
    
    // Submit samples
    audio_mixer->submit_samples(voice_id, sine_samples.data(), sine_samples.size());
    
    // Get mixed output
    std::vector<s16> output(1024);
    u32 frames = audio_mixer->get_output(output.data(), 512);
    EXPECT_EQ(frames, 512u);
    
    // Check that output is not all zeros
    bool has_audio = false;
    for (s16 sample : output) {
        if (sample != 0) {
            has_audio = true;
            break;
        }
    }
    EXPECT_TRUE(has_audio);
    
    audio_mixer->destroy_voice(voice_id);
    audio_mixer->shutdown();
}

}  // namespace test
}  // namespace x360mu

