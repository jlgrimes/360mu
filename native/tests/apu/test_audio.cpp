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
// APU Integration Tests
//=============================================================================

class ApuTest : public ::testing::Test {
protected:
    void SetUp() override {
        apu_ = std::make_unique<Apu>();
        // Note: APU requires memory to be passed, so we test limited functionality
    }
    
    void TearDown() override {
        apu_->shutdown();
        apu_.reset();
    }
    
    std::unique_ptr<Apu> apu_;
};

TEST_F(ApuTest, InitializeWithoutMemory) {
    Status status = apu_->initialize(nullptr);
    EXPECT_EQ(status, Status::Ok);  // Should still initialize mixer and decoder
}

TEST_F(ApuTest, Shutdown) {
    apu_->initialize(nullptr);
    apu_->shutdown();
    // Should not crash on multiple shutdowns
    apu_->shutdown();
}

TEST_F(ApuTest, RegisterAccess) {
    apu_->initialize(nullptr);
    
    // Write and read registers
    apu_->write_register(0x00, 0x12345678);
    u32 value = apu_->read_register(0x00);
    // Value may be modified by register handling
    (void)value;
    
    // Test interrupt registers
    apu_->write_register(0x10, 0xFF);  // Interrupt mask
    apu_->write_register(0x14, 0xFF);  // Interrupt acknowledge
}

TEST_F(ApuTest, GetOutput) {
    apu_->initialize(nullptr);
    
    std::vector<s16> output(512);
    u32 frames = apu_->get_output(output.data(), 256);
    EXPECT_EQ(frames, 256u);
}

TEST_F(ApuTest, Process) {
    apu_->initialize(nullptr);
    
    // Should handle null memory gracefully
    apu_->process();
}


}  // namespace test
}  // namespace x360mu
