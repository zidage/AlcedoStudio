//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once
#include "edit/operators/op_base.hpp"

namespace alcedo {
struct WhiteOpKernel : PointOpTag {
  inline void operator()(Pixel& p, OperatorParams& params) const {
    if (!params.white_enabled_) return;
    p.r_ = (p.r_) * params.slope_ + params.black_point_;
    p.g_ = (p.g_) * params.slope_ + params.black_point_;
    p.b_ = (p.b_) * params.slope_ + params.black_point_;
  }
};
}  // namespace alcedo