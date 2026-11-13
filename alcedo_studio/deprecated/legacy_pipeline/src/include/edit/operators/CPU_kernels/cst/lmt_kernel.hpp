//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <OpenColorIO/OpenColorIO.h>
#include <OpenColorIO/OpenColorTypes.h>

#include "edit/operators/op_base.hpp"

namespace alcedo {

struct OCIO_LMT_Transform_Op_Kernel : PointOpTag {
  inline void operator()(Pixel&, OperatorParams&) const {
    // if (!params.lmt_enabled_ || !params.cpu_lmt_processor_) return;
    // params.cpu_lmt_processor_->applyRGBA(&p.r_);
  }
};

}  // namespace alcedo