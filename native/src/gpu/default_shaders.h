/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Default Fallback Shaders
 * Simple passthrough vertex and solid color pixel shaders for testing
 */

#pragma once

#include "x360mu/types.h"
#include <vector>

namespace x360mu {

/**
 * Get default passthrough vertex shader SPIR-V
 *
 * Input: vec3 position (location 0)
 * Output: gl_Position = vec4(position, 1.0)
 */
const std::vector<u32>& get_default_vertex_shader_spirv();

/**
 * Get default solid color pixel shader SPIR-V
 *
 * Output: vec4(1.0, 0.0, 0.0, 1.0) - solid red
 */
const std::vector<u32>& get_default_pixel_shader_spirv();

} // namespace x360mu
