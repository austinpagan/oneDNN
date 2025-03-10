/*******************************************************************************
* Copyright 2022 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef GPU_JIT_CONV_BLOCK_2D_UTILS_HPP
#define GPU_JIT_CONV_BLOCK_2D_UTILS_HPP

#include <algorithm>

#include "gpu/jit/conv/hw_config.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace jit {

inline int block_2d_base_alignment(const hw_config_t &hw_cfg) {
    ir_assert(hw_cfg.hw() >= ngen::HW::XeHPC);
    // XXX: A steppings require 128 byte alignment due to a HW bug.
    if (hw_cfg.stepping_id() < 5) return 128;
    return 64;
}

inline bool block_2d_width_ok(int width, int type_size) {
    int width_bytes = width * type_size;
    if (width_bytes < 64) return false;
    if (width_bytes > (1 << 24)) return false;
    if (width_bytes % std::max(4, type_size) != 0) return false;
    return true;
}

inline bool block_2d_height_ok(int height) {
    if (height > (1 << 24)) return false;
    return true;
}

inline bool block_2d_pitch_ok(const hw_config_t &hw_cfg, int pitch,
        int type_size, bool use_xy = true) {
    int pitch_bytes = pitch * type_size;
    if (pitch_bytes < 64) return false;
    if (pitch_bytes > (1 << 24)) return false;
    if (pitch_bytes % 16 != 0) return false;
    // To be able to point the base to different rows.
    if (use_xy && pitch_bytes % block_2d_base_alignment(hw_cfg) != 0)
        return false;
    return true;
}

} // namespace jit
} // namespace gpu
} // namespace impl
} // namespace dnnl

#endif
