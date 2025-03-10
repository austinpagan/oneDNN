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

#ifndef GPU_JIT_CONV_EPILOGUE_HPP
#define GPU_JIT_CONV_EPILOGUE_HPP

#include "gpu/jit/conv/config.hpp"
#include "gpu/jit/conv/gemm_schedule.hpp"
#include "gpu/jit/conv/ir.hpp"
#include "gpu/jit/conv/post_op_support.hpp"
#include "gpu/jit/conv/slm_reduce_builder.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace jit {

stmt_t create_epilogue_stmt(const conv_config_t &cfg, ir_context_t &ir_ctx,
        const constraint_set_t &cset, const gemm_schedule_t &gemm_schedule,
        const post_op_context_t &post_op_ctx, const tensor_t &thr_tile,
        const view_t &c_mem_view, const layout_t &c_reg_layout,
        const expr_t &c_mem_buf, const expr_t &c_reg_buf);

} // namespace jit
} // namespace gpu
} // namespace impl
} // namespace dnnl

#endif
