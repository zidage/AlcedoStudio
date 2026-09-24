//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/operators/op_base.hpp"

namespace alcedo {

struct ContrastOpKernel : PointOpTag {
  inline void operator()(Pixel& p, OperatorParams& params) const {
    if (!params.contrast_enabled_) return;
    p.r_ = (p.r_ - 0.05707762557f) * params.contrast_scale_ + 0.05707762557f;  // 1 stop = 1/17.52
    p.g_ = (p.g_ - 0.05707762557f) * params.contrast_scale_ + 0.05707762557f;
    p.b_ = (p.b_ - 0.05707762557f) * params.contrast_scale_ + 0.05707762557f;
  }
};
}  // namespace alcedo