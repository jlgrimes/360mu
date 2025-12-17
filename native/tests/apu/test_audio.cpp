/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Audio subsystem unit tests
 * 
 * Tests XMA decoder, audio mixer, and related components.
 */

#include <gtest/gtest.h>
#include "apu/xma_decoder.h"
#include <cmath>
#include <vector>
#include <cstring>

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

}  // namespace test
}  // namespace x360mu

