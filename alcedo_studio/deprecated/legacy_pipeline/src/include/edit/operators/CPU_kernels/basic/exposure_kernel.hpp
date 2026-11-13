//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/operators/op_base.hpp"

namespace alcedo {
struct ExposureOpKernel : PointOpTag {
  inline void operator()(Pixel& p, OperatorParams& params) const {
    if (!params.exposure_enabled_) return;
    p.r_ += params.exposure_offset_;
    p.g_ += params.exposure_offset_;
    p.b_ += params.exposure_offset_;
  }
};
}  // namespace alcedo