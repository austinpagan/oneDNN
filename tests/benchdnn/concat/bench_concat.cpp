/*******************************************************************************
* Copyright 2019-2022 Intel Corporation
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

#include <stdio.h>
#include <stdlib.h>

#include <sstream>

#include "dnnl_common.hpp"
#include "utils/parser.hpp"

#include "concat/concat.hpp"

namespace concat {

void check_correctness(const settings_t &s) {
    for_(const auto &i_sdt : s.sdt)
    for_(const auto &i_ddt : s.ddt)
    for_(const auto &i_stag_ : s.stag)
    for_(const auto &i_dtag : s.dtag)
    for_(const auto &i_axis : s.axis)
    for_(const auto &i_scales : s.scales)
    for (const auto &i_scratchpad_mode : s.scratchpad_mode) {
        // if dst is omitted by dtag = tag::undef, omit ddt as well
        auto ddt = i_dtag == tag::undef ? dnnl_data_type_undef : i_ddt;

        // broadcast tag if needed
        auto i_stag = i_stag_;
        if (i_stag.size() == 1) {
            const auto val = i_stag_[0];
            i_stag.assign(s.prb_vdims.n_inputs(), val);
        }

        // want 1:1 match of vdims and tag
        if (s.prb_vdims.n_inputs() != static_cast<int>(i_stag.size()))
            SAFE_V(FAIL);

        auto attr = settings_t::get_attr(i_scales, i_scratchpad_mode);

        const prb_t prb(s.prb_vdims, i_sdt, ddt, i_stag, i_dtag, i_axis, attr);
        std::stringstream ss;
        ss << prb;
        const std::string cpp_pstr = ss.str();
        const char *pstr = cpp_pstr.c_str();
        BENCHDNN_PRINT(1, "run: %s\n", pstr);

        res_t res {};
        doit(&prb, &res);

        parse_result(res, pstr);

        if (is_bench_mode(PERF)) {
            perf_report_t pr(&prb, s.perf_template);
            pr.report(&res, pstr);
        }
    }
}

int bench(int argc, char **argv) {
    driver_name = "concat";
    using namespace parser;
    static settings_t s;
    static const settings_t def {};
    for (; argc > 0; --argc, ++argv) {
        const bool parsed_options = parse_bench_settings(argv[0])
                || parse_batch(bench, argv[0])
                || parse_dt(s.sdt, def.sdt, argv[0], "sdt")
                || parse_dt(s.ddt, def.ddt, argv[0], "ddt")
                || parse_multi_tag(s.stag, def.stag, argv[0])
                || parse_tag(s.dtag, def.dtag, argv[0], "dtag")
                || parse_axis(s.axis, def.axis, argv[0])
                || parse_attr_scales(s.scales, argv[0])
                || parse_attr_scratchpad_mode(
                        s.scratchpad_mode, def.scratchpad_mode, argv[0])
                || parse_perf_template(s.perf_template, s.perf_template_def,
                        s.perf_template_csv(), argv[0])
                || parse_reset(s, argv[0]) || parse_help(argv[0]);
        if (!parsed_options) {
            catch_unknown_options(argv[0]);

            parse_prb_vdims(s.prb_vdims, argv[0]);

            check_correctness(s);
        }
    }

    return parse_last_argument();
}

} // namespace concat
