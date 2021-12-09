/*******************************************************************************
 * Copyright 2021 Intel Corporation
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
#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "interface/c_types_map.hpp"

#include "insert_ops.hpp"
#include "utils.hpp"

namespace dnnl {
namespace graph {
namespace impl {
namespace dnnl_impl {
using op_t = impl::op_t;
using op_ptr = std::shared_ptr<impl::op_t>;

// TODO(xxx): extend to support other ops
// for those ops with data_format/filter_format attributes
static bool need_insert_permute(op_kind_t kind) {
    static const std::set<op_kind_t> ops {
            op_kind::dnnl_convolution,
            op_kind::conv_depthwise,
            impl::op_kind::Convolution,
            impl::op_kind::ConvTranspose,
            op_kind::dnnl_convtranspose,
            impl::op_kind::MaxPool,
            impl::op_kind::AvgPool,
            op_kind::dnnl_pool,
            op_kind::dnnl_batchnorm,
            impl::op_kind::BatchNormInference,
            impl::op_kind::BatchNormForwardTraining,
    };
    return ops.count(kind) != 0;
}

// TODO(xxx): extend to support other ops
// for those ops whose input's format must be defined, such as pool, eltwise,...
static bool require_input_format(op_kind_t kind) {
    static const std::set<op_kind_t> ops {op_kind::dnnl_pool,
            // eltwise algorithms
            impl::op_kind::Abs, impl::op_kind::MaxPool, impl::op_kind::AvgPool,
            impl::op_kind::Elu, impl::op_kind::Exp, impl::op_kind::GELU,
            impl::op_kind::HardTanh, impl::op_kind::Log, impl::op_kind::ReLU,
            impl::op_kind::Round, impl::op_kind::Sigmoid, impl::op_kind::Sqrt,
            impl::op_kind::Square, impl::op_kind::Tanh, op_kind::dnnl_sum};
    return ops.count(kind) != 0;
}

static inline dims get_dense_strides(const dims &shape) {
    dims strides(shape.size());
    for (auto it = shape.begin(); it < shape.end(); ++it) {
        const auto val = std::accumulate(
                std::next(it), shape.end(), 1, std::multiplies<dim_t>());
        const auto dist = std::distance(shape.begin(), it);
        strides[static_cast<size_t>(dist)] = val;
    }
    return strides;
}

impl::status_t insert_permute(std::shared_ptr<subgraph_t> &sg) {
    auto &subgraph = sg->get_mutable_ops();
    std::vector<op_ptr> to_be_inserted_ops;
    std::vector<op_ptr> to_be_removed_ops;
    for (auto &cur_op : subgraph) {
        if (!need_insert_permute(cur_op->get_kind())) continue;

        // TODO(xx) how to support multiple binary post-ops?
        const bool need_permute_0 = cur_op->has_attr("data_format")
                ? (cur_op->get_attr<std::string>("data_format") == "NXC")
                : false;
        const bool need_permute_1 = cur_op->has_attr("filter_format")
                ? (cur_op->get_attr<std::string>("filter_format") == "XIO")
                : false;
        // conv + depthwise case
        const bool need_permute_2 = cur_op->has_attr("dw_filter_format")
                ? (cur_op->get_attr<std::string>("dw_filter_format") == "XIO")
                : false;
        const bool need_permute_sum_1 = cur_op->has_attr("with_sum")
                ? (cur_op->get_attr<bool>("with_sum") && need_permute_0)
                : false;
        const bool need_permute_binary_src_1 = cur_op->has_attr("with_binary")
                ? (cur_op->get_attr<bool>("with_binary") && need_permute_0)
                : false;

        if (!(need_permute_0 || need_permute_1 || need_permute_2
                    || need_permute_sum_1 || need_permute_binary_src_1))
            continue;

        for (size_t i = 0; i < cur_op->num_inputs(); i++) {
            op_ptr perm_op = std::make_shared<impl::op_t>(op_kind::permute);
            perm_op->set_attr<std::string>("permute_kind", "permute");
            if (i == 0 && need_permute_0) {
                perm_op->set_attr<std::string>("from_format", "NXC");
                perm_op->set_attr<std::string>("to_format", "NCX");
            } else if ((i == 1 && need_permute_1)
                    || (i == 2 && need_permute_2)) {
                perm_op->set_attr<std::string>("from_format", "XIO");
                perm_op->set_attr<std::string>("to_format", "OIX");
            } else if (i == cur_op->num_inputs() - 1 && need_permute_sum_1) {
                perm_op->set_attr<std::string>("from_format", "NXC");
                perm_op->set_attr<std::string>("to_format", "NCX");
            } else if (i == cur_op->num_inputs() - 1
                    && need_permute_binary_src_1) {
                perm_op->set_attr<std::string>("from_format", "NXC");
                perm_op->set_attr<std::string>("to_format", "NCX");
            } else {
                continue;
            }

            insert_op_before(perm_op, cur_op, i);
            to_be_inserted_ops.emplace_back(perm_op);
        }

        // permute output back to NXC
        if (need_permute_0) {
            op_ptr perm_op = std::make_shared<impl::op_t>(op_kind::permute);
            perm_op->set_attr<std::string>("permute_kind", "permute");
            perm_op->set_attr<std::string>("from_format", "NCX");
            perm_op->set_attr<std::string>("to_format", "NXC");
            insert_op_after(perm_op, cur_op, 0);
            to_be_inserted_ops.emplace_back(perm_op);
        }

        // remove the attrs in cur_op to avoid re-permute
        cur_op->set_attr<std::string>("data_format", "NCX");
        if (cur_op->has_attr("filter_format"))
            cur_op->set_attr<std::string>("filter_format", "OIX");
        // conv + depthwise case
        if (need_permute_2)
            cur_op->set_attr<std::string>("dw_filter_format", "OIX");

        if (cur_op->get_kind() == impl::op_kind::Convolution) {
            // replace impl::op_kind::Convolution to be
            // op_kind::dnnl_convolution
            op_ptr new_op = std::make_shared<op_t>(op_kind::dnnl_convolution);
            replace_op(cur_op, new_op);
            to_be_inserted_ops.emplace_back(new_op);
            to_be_removed_ops.emplace_back(cur_op);
        }

        if (cur_op->get_kind() == impl::op_kind::ConvTranspose) {
            // replace impl::op_kind::ConvTranspose to be
            // op_kind::dnnl_convtranspose
            op_ptr new_op = std::make_shared<op_t>(op_kind::dnnl_convtranspose);
            replace_op(cur_op, new_op);
            to_be_inserted_ops.emplace_back(new_op);
            to_be_removed_ops.emplace_back(cur_op);
        }

        if (cur_op->get_kind() == impl::op_kind::MaxPool
                || cur_op->get_kind() == impl::op_kind::AvgPool) {
            op_ptr new_op = std::make_shared<op_t>(op_kind::dnnl_pool);
            replace_op(cur_op, new_op);
            if (cur_op->get_kind() == impl::op_kind::MaxPool) {
                new_op->set_attr<std::string>("kind", "maxpool");
            } else {
                new_op->set_attr<std::string>("kind", "avgpool");
            }
            to_be_inserted_ops.emplace_back(new_op);
            to_be_removed_ops.emplace_back(cur_op);
        }
    }
    for (const auto &op : to_be_inserted_ops)
        subgraph.emplace_back(op);
    for (const auto &op : to_be_removed_ops) {
        auto pos = std::find_if(subgraph.begin(), subgraph.end(),
                [op](const op_ptr &tmp) { return op.get() == tmp.get(); });
        if (pos != subgraph.end()) subgraph.erase(pos);
    }
    return impl::status::success;
}

impl::status_t insert_permute_for_shuffle(std::shared_ptr<subgraph_t> &sg) {
    // optimization for NXC case - it will help to hit an optimized kernel
    auto &subgraph = sg->get_mutable_ops();
    std::vector<op_ptr> to_be_inserted_ops;
    for (auto &cur_op : subgraph) {
        if (cur_op->get_kind() != op_kind::dnnl_shuffle) continue;

        impl::logical_tensor_t src_lt
                = cur_op->get_input_value(0)->get_logical_tensor();
        const logical_tensor_wrapper_t src(src_lt);
        const auto axis = cur_op->get_attr<int64_t>("axis");
        const auto known_strides
                = (src.is_strided()) ? !src.is_stride_unknown() : false;
        const bool need_permute = axis == src.ndims() - 1 && known_strides
                && src.vstrides() == get_dense_strides(src.vdims());
        if (!need_permute) continue;

        const int64_t new_axis = 1;
        cur_op->set_attr("axis", new_axis);
        op_ptr perm_src_op = std::make_shared<impl::op_t>(op_kind::permute);
        perm_src_op->set_attr<std::string>("permute_kind", "permute");
        perm_src_op->set_attr<std::string>("from_format", "NXC");
        perm_src_op->set_attr<std::string>("to_format", "NCX");
        insert_op_before(perm_src_op, cur_op, 0);
        to_be_inserted_ops.emplace_back(perm_src_op);

        // permute output back to NXC
        op_ptr perm_dst_op = std::make_shared<impl::op_t>(op_kind::permute);
        perm_dst_op->set_attr<std::string>("permute_kind", "permute");
        perm_dst_op->set_attr<std::string>("from_format", "NCX");
        perm_dst_op->set_attr<std::string>("to_format", "NXC");
        insert_op_after(perm_dst_op, cur_op, 0);
        to_be_inserted_ops.emplace_back(perm_dst_op);
    }

    for (const auto &op : to_be_inserted_ops)
        subgraph.emplace_back(op);

    return impl::status::success;
}

impl::status_t insert_to_group_for_conv_or_deconv(
        std::shared_ptr<subgraph_t> &sg) {
    auto &subgraph = sg->get_mutable_ops();
    std::vector<op_ptr> to_be_inserted_ops;
    std::vector<op_ptr> to_be_removed_ops;

    auto insert_to_group
            = [&to_be_inserted_ops](op_ptr &op, const std::string &attr_name,
                      const size_t offset) -> bool {
        auto groups = op->get_attr<int64_t>(attr_name);
        if (groups <= 1) return false;

        op_ptr to_group_op = std::make_shared<impl::op_t>(op_kind::to_group);
        to_group_op->set_attr<int64_t>("groups", groups);

        insert_op_before(to_group_op, op, offset);
        to_be_inserted_ops.emplace_back(to_group_op);

        if (op->get_kind() == impl::op_kind::ConvTranspose
                || op->get_kind() == op_kind::dnnl_convtranspose)
            to_group_op->set_attr<bool>("is_convtranspose", true);

        return true;
    };

    for (auto &cur_op : subgraph) {
        if (cur_op->get_kind() != op_kind::dnnl_convolution
                && cur_op->get_kind() != impl::op_kind::Convolution
                && cur_op->get_kind() != op_kind::dnnl_convtranspose
                && cur_op->get_kind() != impl::op_kind::ConvTranspose
                && cur_op->get_kind() != op_kind::conv_depthwise)
            continue;

        if (cur_op->get_kind() == op_kind::conv_depthwise) {
            const auto inserted = insert_to_group(cur_op, "dw_groups", 2);
            if (!inserted) continue;
        }

        const auto inserted = insert_to_group(cur_op, "groups", 1);
        if (!inserted) continue;

        if (cur_op->get_kind() == impl::op_kind::Convolution) {
            // replace impl::op_kind::Convolution to be
            // op_kind::dnnl_convolution
            op_ptr new_op = std::make_shared<op_t>(op_kind::dnnl_convolution);
            replace_op(cur_op, new_op);
            to_be_inserted_ops.emplace_back(new_op);
            to_be_removed_ops.emplace_back(cur_op);
        }

        if (cur_op->get_kind() == impl::op_kind::ConvTranspose) {
            // replace impl::op_kind::Convolution to be
            // op_kind::dnnl_convtranspose
            op_ptr new_op = std::make_shared<op_t>(op_kind::dnnl_convtranspose);
            replace_op(cur_op, new_op);
            to_be_inserted_ops.emplace_back(new_op);
            to_be_removed_ops.emplace_back(cur_op);
        }
    }
    for (const auto &op : to_be_inserted_ops)
        subgraph.emplace_back(op);
    for (const auto &op : to_be_removed_ops) {
        auto pos = std::find_if(subgraph.begin(), subgraph.end(),
                [op](const op_ptr &tmp) { return op.get() == tmp.get(); });
        if (pos != subgraph.end()) subgraph.erase(pos);
    }
    return impl::status::success;
}

impl::status_t insert_transpose_for_matmul(std::shared_ptr<subgraph_t> &sg) {
    auto &subgraph = sg->get_mutable_ops();
    std::vector<op_ptr> to_be_inserted_ops;
    for (auto &cur_op : subgraph) {
        if (cur_op->get_kind() != impl::op_kind::MatMul) continue;

        std::vector<bool> trans_flag(2);
        if (cur_op->has_attr("transpose_a"))
            trans_flag[0] = cur_op->get_attr<bool>("transpose_a");
        if (cur_op->has_attr("transpose_b"))
            trans_flag[1] = cur_op->get_attr<bool>("transpose_b");
        if (!(trans_flag[0] || trans_flag[1])) continue;

        for (size_t i = 0; i < trans_flag.size(); ++i) {
            // skip if transpose flag is false or the input's ndim is 1
            // otherwise, we need do do expand
            if (!trans_flag[i]
                    || cur_op->get_input_value(i)->get_logical_tensor().ndims
                            <= 1)
                continue;
            op_ptr transpose_op = std::make_shared<op_t>(op_kind::permute);
            transpose_op->set_attr<std::string>("permute_kind", "transpose");
            insert_op_before(transpose_op, cur_op, i);
            to_be_inserted_ops.emplace_back(transpose_op);
        }
        // remove attr to avoid re-transpose during shape inference
        cur_op->set_attr<bool>("transpose_a", false);
        cur_op->set_attr<bool>("transpose_b", false);
    }
    for (const auto &op : to_be_inserted_ops)
        subgraph.emplace_back(op);
    return impl::status::success;
}

impl::status_t insert_reshape_for_ndx2d_matmul(
        std::shared_ptr<subgraph_t> &sg) {
    auto &subgraph = sg->get_mutable_ops();
    auto &prm_attr_mgr = sg->prm_attr_mgr_;

    std::vector<op_ptr> to_be_inserted_ops;
    for (auto &cur_op : subgraph) {
        if (cur_op->get_kind() != impl::op_kind::MatMul) continue;
        // skip due to dnnl cannot reshape such kind of strided memory desc
        if (cur_op->get_input_value(0)->has_producer()
                && cur_op->get_input_value(0)->get_producer().get_kind()
                        == op_kind::permute) {
            continue;
        }
        int32_t src_ndims
                = cur_op->get_input_value(0)->get_logical_tensor().ndims;
        int32_t wei_ndims
                = cur_op->get_input_value(1)->get_logical_tensor().ndims;
        if (wei_ndims != 2 || src_ndims <= 2) continue;

        auto src_dims = logical_tensor_wrapper_t(
                cur_op->get_input_value(0)->get_logical_tensor())
                                .vdims();
        impl::dims expected_dims {-1, src_dims.back()};
        auto reshape_op = std::make_shared<op_t>(impl::op_kind::StaticReshape);
        reshape_op->set_attr<bool>("special_zero", false);
        reshape_op->set_attr<std::vector<int64_t>>("shape", expected_dims);
        to_be_inserted_ops.emplace_back(reshape_op);
        insert_op_before(reshape_op, cur_op, 0);

        impl::dims expected_dims2(src_dims);
        expected_dims2[expected_dims2.size() - 1] = 0;
        auto reshape_op2 = std::make_shared<op_t>(impl::op_kind::StaticReshape);
        reshape_op2->set_attr<bool>("special_zero", true);
        reshape_op2->set_attr<std::vector<int64_t>>("shape", expected_dims2);
        to_be_inserted_ops.emplace_back(reshape_op2);
        insert_op_after(reshape_op2, cur_op, 0);

        bool with_bias = cur_op->has_attr("with_bias")
                ? cur_op->get_attr<bool>("with_bias")
                : false;
        if (!with_bias && cur_op->num_inputs() == 3) {
            auto post_src_dims = logical_tensor_wrapper_t(
                    cur_op->get_input_value(2)->get_logical_tensor())
                                         .vdims();
            impl::dims expected_dims3 {-1, post_src_dims.back()};
            auto reshape_op3
                    = std::make_shared<op_t>(impl::op_kind::StaticReshape);
            reshape_op3->set_attr<bool>("special_zero", false);
            reshape_op3->set_attr<std::vector<int64_t>>(
                    "shape", expected_dims3);
            to_be_inserted_ops.emplace_back(reshape_op3);
            insert_op_before(reshape_op3, cur_op, 2);
        } else if (with_bias && cur_op->num_inputs() == 4) {
            auto post_src_dims = logical_tensor_wrapper_t(
                    cur_op->get_input_value(3)->get_logical_tensor())
                                         .vdims();
            impl::dims expected_dims3 {-1, post_src_dims.back()};
            auto reshape_op3
                    = std::make_shared<op_t>(impl::op_kind::StaticReshape);
            reshape_op3->set_attr<bool>("special_zero", false);
            reshape_op3->set_attr<std::vector<int64_t>>(
                    "shape", expected_dims3);
            to_be_inserted_ops.emplace_back(reshape_op3);
            insert_op_before(reshape_op3, cur_op, 3);
        }

        // update the mask (mask is related to axis, after changing shape, the
        // axis is changed)
        if (cur_op->has_attr("primitive_attr_key")) {
            int64_t key = cur_op->get_attr<int64_t>("primitive_attr_key");
            auto prm_attr = prm_attr_mgr.get_attr(key);
            scale_t ori_scales;
            int ori_mask;
            prm_attr.get_output_scales(ori_mask, ori_scales);
            ori_mask = 2; // the second axis
            prm_attr.set_output_scales(ori_mask, ori_scales);
        }
    }
    for (const auto &op : to_be_inserted_ops)
        subgraph.emplace_back(op);
    return impl::status::success;
}

impl::status_t insert_expand_and_squeeze_for_matmul(
        std::shared_ptr<subgraph_t> &sg) {
    auto &subgraph = sg->get_mutable_ops();
    std::vector<op_ptr> to_be_inserted_ops;
    for (auto &cur_op : subgraph) {
        if (cur_op->get_kind() != impl::op_kind::MatMul) continue;

        std::vector<op_ptr> expand_ops;
        expand_ops.reserve(cur_op->num_inputs());

        int32_t new_src_ndims
                = cur_op->get_input_value(0)->get_logical_tensor().ndims;
        int32_t new_wei_ndims
                = cur_op->get_input_value(1)->get_logical_tensor().ndims;
        assertm(new_src_ndims >= 1 && new_wei_ndims >= 1, "invalid dims");

        std::vector<int32_t> ori_ndims {new_src_ndims, new_wei_ndims};
        int32_t dst_ndims
                = cur_op->get_output_value(0)->get_logical_tensor().ndims;
        for (size_t i = 0; i < cur_op->num_inputs(); ++i) {
            auto expand_op = std::make_shared<op_t>(op_kind::expand);
            if (i < 2) { // src and weight
                auto ndims = ori_ndims[i];
                if (ndims != 1) {
                    expand_ops.emplace_back(expand_op);
                    continue;
                }
                // 1D -> 2D
                if (i == 0) {
                    expand_op->set_attr<std::string>("insert_1dim", "before");
                    new_src_ndims = ndims + 1;
                } else if (i == 1) {
                    expand_op->set_attr<std::string>("insert_1dim", "after");
                    new_wei_ndims = ndims + 1;
                }
            } else { // bias
                // expand bias to dst ndims if they are mis-matched
                int64_t tgt_ndims = std::max(new_src_ndims, new_wei_ndims);
                if (cur_op->get_input_value(i)->get_logical_tensor().ndims
                        != tgt_ndims)
                    expand_op->set_attr<int64_t>("expand_to", tgt_ndims);
            }
            expand_ops.emplace_back(expand_op);
        }

        std::vector<int64_t> squeeze_dims = {};
        for (size_t i = 0; i < expand_ops.size(); ++i) {
            if (i == 0 && new_src_ndims < new_wei_ndims) {
                expand_ops[i]->set_attr<int64_t>("expand_to", new_wei_ndims);
            } else if (i == 1 && new_wei_ndims < new_src_ndims) {
                expand_ops[i]->set_attr<int64_t>("expand_to", new_src_ndims);
            }

            // insert expand ops
            if ((expand_ops[i]->has_attr("insert_1dim")
                        && expand_ops[i]->get_attr<std::string>("insert_1dim")
                                != "none")
                    || (expand_ops[i]->has_attr("expand_to")
                            && expand_ops[i]->get_attr<int64_t>("expand_to")
                                    != -1)) {
                insert_op_before(expand_ops[i], cur_op, i);
                to_be_inserted_ops.emplace_back(expand_ops[i]);
            }

            // decide squeeze dims
            if (expand_ops[i]->has_attr("insert_1dim")
                    && expand_ops[i]->get_attr<std::string>("insert_1dim")
                            != "none") {
                // -2 means the second rightmost dimension, -1 means the
                // rightmost dimension
                int64_t dim_to_be_squeezed
                        = expand_ops[i]->get_attr<std::string>("insert_1dim")
                                == "before"
                        ? static_cast<int64_t>(-2)
                        : static_cast<int64_t>(-1);
                squeeze_dims.push_back(dim_to_be_squeezed);
            }
        }

        // insert squeeze ops
        if (!squeeze_dims.empty()) {
            op_ptr squeeze_op = std::make_shared<op_t>(op_kind::squeeze);
            squeeze_op->set_attr<std::vector<int64_t>>("axes", squeeze_dims);
            insert_op_after(squeeze_op, cur_op, 0);
            to_be_inserted_ops.emplace_back(squeeze_op);
        }
    }
    for (const auto &op : to_be_inserted_ops)
        subgraph.emplace_back(op);
    return impl::status::success;
}

impl::status_t insert_u8_to_s8_for_matmul(std::shared_ptr<subgraph_t> &sg) {
    auto &subgraph = sg->get_mutable_ops();
    auto &prm_attr_mgr = sg->prm_attr_mgr_;

    std::vector<op_ptr> to_be_inserted_ops;
    for (auto &cur_op : subgraph) {
        if (cur_op->get_kind() != impl::op_kind::MatMul) continue;

        int32_t new_src0_dtype
                = cur_op->get_input_value(0)->get_logical_tensor().data_type;
        int32_t new_src1_dtype
                = cur_op->get_input_value(1)->get_logical_tensor().data_type;
        if (!impl::utils::one_of(
                    new_src0_dtype, impl::data_type::u8, impl::data_type::s8)
                || new_src1_dtype != impl::data_type::u8)
            continue;

        int64_t key = -1;
        if (cur_op->has_attr("primitive_attr_key")) {
            key = cur_op->get_attr<int64_t>("primitive_attr_key");
        } else {
            key = prm_attr_mgr.init_attr();
            cur_op->set_attr<int64_t>("primitive_attr_key", key);
        }
        dnnl::primitive_attr &prm_attr = prm_attr_mgr.get_attr(key);
        std::vector<int32_t> current_zp;
        int mask = 0;
        prm_attr.get_zero_points(DNNL_ARG_WEIGHTS, mask, current_zp);
        if (current_zp.size() != 1) continue;
        std::vector<int32_t> adjusted_zp {current_zp[0] - 128};
        prm_attr.set_zero_points(DNNL_ARG_WEIGHTS, mask, adjusted_zp);

        op_ptr u8_to_s8_op = std::make_shared<op_t>(op_kind::dnnl_u8_to_s8);
        insert_op_before(u8_to_s8_op, cur_op, 1);
        to_be_inserted_ops.emplace_back(u8_to_s8_op);
    }
    for (const auto &op : to_be_inserted_ops)
        subgraph.emplace_back(op);
    return impl::status::success;
}

} // namespace dnnl_impl
} // namespace impl
} // namespace graph
} // namespace dnnl
