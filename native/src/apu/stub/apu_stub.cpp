/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Stub APU implementation when audio is disabled
 */

#include "apu/audio.h"

namespace x360mu {

// Apu implementation
Apu::Apu() = default;
Apu::~Apu() = default;

Status Apu::initialize(Memory* memory, const ApuConfig& config) {
    memory_ = memory;
    config_ = config;
    return Status::Ok;
}

void Apu::shutdown() {}

void Apu::reset() {
    for (auto& ctx : xma_contexts_) {
        ctx = {};
    }
    for (auto& voice : voices_) {
        voice.active = false;
        voice.context_index = 0;
        voice.volume_left = 1.0f;
        voice.volume_right = 1.0f;
        voice.pitch = 1.0f;
        voice.pcm_buffer.clear();
        voice.read_pos.store(0);
        voice.write_pos.store(0);
    }
    stats_ = {};
}

void Apu::process() {
    // Stub - no audio processing
}

Status Apu::create_context(u32 index, const XmaContext& ctx) {
    if (index >= xma_contexts_.size()) {
        return Status::InvalidArgument;
    }
    xma_contexts_[index] = ctx;
    return Status::Ok;
}

void Apu::destroy_context(u32 index) {
    if (index < xma_contexts_.size()) {
        xma_contexts_[index] = {};
    }
}

XmaContext* Apu::get_context(u32 index) {
    if (index < xma_contexts_.size()) {
        return &xma_contexts_[index];
    }
    return nullptr;
}

u32 Apu::create_voice(u32 context_index) {
    for (u32 i = 0; i < voices_.size(); i++) {
        if (!voices_[i].active) {
            voices_[i].active = true;
            voices_[i].context_index = context_index;
            voices_[i].volume_left = 1.0f;
            voices_[i].volume_right = 1.0f;
            voices_[i].pitch = 1.0f;
            return i;
        }
    }
    return ~0u;
}

void Apu::destroy_voice(u32 voice_id) {
    if (voice_id < voices_.size()) {
        voices_[voice_id].active = false;
        voices_[voice_id].context_index = 0;
        voices_[voice_id].volume_left = 1.0f;
        voices_[voice_id].volume_right = 1.0f;
        voices_[voice_id].pitch = 1.0f;
        voices_[voice_id].pcm_buffer.clear();
        voices_[voice_id].read_pos.store(0);
        voices_[voice_id].write_pos.store(0);
    }
}

void Apu::set_voice_volume(u32 voice_id, f32 left, f32 right) {
    if (voice_id < voices_.size()) {
        voices_[voice_id].volume_left = left;
        voices_[voice_id].volume_right = right;
    }
}

void Apu::set_voice_pitch(u32 voice_id, f32 pitch) {
    if (voice_id < voices_.size()) {
        voices_[voice_id].pitch = pitch;
    }
}

void Apu::start_voice(u32 /*voice_id*/) {}
void Apu::stop_voice(u32 /*voice_id*/) {}

u32 Apu::read_register(u32 /*offset*/) {
    return 0;
}

void Apu::write_register(u32 /*offset*/, u32 /*value*/) {}

u32 Apu::get_samples(s16* buffer, u32 sample_count) {
    // Return silence
    if (buffer && sample_count > 0) {
        for (u32 i = 0; i < sample_count * 2; i++) {
            buffer[i] = 0;
        }
    }
    return sample_count;
}

void Apu::decode_xma_packets() {}
void Apu::mix_voices() {}
void Apu::submit_to_output() {}

// XmaDecoder stub
Apu::XmaDecoder::XmaDecoder() = default;
Apu::XmaDecoder::~XmaDecoder() = default;

Status Apu::XmaDecoder::initialize() {
    return Status::Ok;
}

void Apu::XmaDecoder::shutdown() {}

Status Apu::XmaDecoder::decode(
    const void* /*input*/, u32 /*input_size*/,
    s16* output, u32 output_size,
    u32& samples_decoded
) {
    // Return silence
    if (output && output_size > 0) {
        for (u32 i = 0; i < output_size * 2; i++) {
            output[i] = 0;
        }
    }
    samples_decoded = output_size;
    return Status::Ok;
}

void Apu::XmaDecoder::reset_state(void* /*context*/) {}

// AudioOutput stub
Apu::AudioOutput::AudioOutput() = default;
Apu::AudioOutput::~AudioOutput() = default;

Status Apu::AudioOutput::initialize(const ApuConfig& config) {
    config_ = config;
    return Status::Ok;
}

void Apu::AudioOutput::shutdown() {}

Status Apu::AudioOutput::start() {
    playing_ = true;
    return Status::Ok;
}

void Apu::AudioOutput::stop() {
    playing_ = false;
}

Status Apu::AudioOutput::queue_samples(const s16* /*samples*/, u32 /*count*/) {
    return Status::Ok;
}

u32 Apu::AudioOutput::get_available_space() const {
    return 4096;
}

} // namespace x360mu
