/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Stub AndroidAudioOutput for non-Android builds
 * The real Apu implementation is in apu.cpp (always compiled).
 * This file only stubs AndroidAudioOutput which requires Android AAudio.
 */

#include "apu/android_audio.h"

namespace x360mu {

// AndroidAudioOutput stub for non-Android builds
AndroidAudioOutput::AndroidAudioOutput() = default;
AndroidAudioOutput::~AndroidAudioOutput() = default;

Status AndroidAudioOutput::initialize(const AudioConfig& /*config*/) { return Status::Ok; }
void AndroidAudioOutput::shutdown() {}
Status AndroidAudioOutput::start() { return Status::Ok; }
void AndroidAudioOutput::stop() {}
void AndroidAudioOutput::pause() {}
void AndroidAudioOutput::resume() {}
void AndroidAudioOutput::set_callback(AudioCallback /*callback*/) {}
u32 AndroidAudioOutput::queue_samples(const f32* /*samples*/, u32 /*frame_count*/) { return 0; }
f32 AndroidAudioOutput::get_latency_ms() const { return 0.0f; }
void AndroidAudioOutput::set_volume(f32 /*volume*/) {}

// AudioRingBuffer stub
AudioRingBuffer::AudioRingBuffer(u32 /*frame_count*/, u32 channels)
    : channels_(channels), capacity_(0), read_pos_(0), write_pos_(0) {}

u32 AudioRingBuffer::write(const f32* /*data*/, u32 /*frame_count*/) { return 0; }
u32 AudioRingBuffer::read(f32* /*data*/, u32 /*frame_count*/) { return 0; }
u32 AudioRingBuffer::available_read() const { return 0; }
u32 AudioRingBuffer::available_write() const { return 0; }
void AudioRingBuffer::clear() { read_pos_ = 0; write_pos_ = 0; }

// AudioResampler stub
AudioResampler::AudioResampler() = default;
void AudioResampler::configure(u32 /*input_rate*/, u32 /*output_rate*/, u32 /*channels*/) {}
u32 AudioResampler::process(const f32* /*input*/, u32 /*input_frames*/, f32* /*output*/, u32 /*max_output_frames*/) { return 0; }
void AudioResampler::reset() {}
u32 AudioResampler::get_output_frames(u32 input_frames) const { return input_frames; }

// SimpleAudioMixer stub
SimpleAudioMixer::SimpleAudioMixer() = default;
void SimpleAudioMixer::configure(u32 /*sample_rate*/, u32 /*channels*/) {}
int SimpleAudioMixer::add_source(const f32* /*samples*/, u32 /*frame_count*/, f32 /*volume*/, bool /*loop*/) { return -1; }
void SimpleAudioMixer::remove_source(int /*index*/) {}
void SimpleAudioMixer::set_source_volume(int /*index*/, f32 /*volume*/) {}
void SimpleAudioMixer::set_source_pan(int /*index*/, f32 /*pan*/) {}
void SimpleAudioMixer::mix(f32* /*output*/, u32 /*frame_count*/) {}

} // namespace x360mu
