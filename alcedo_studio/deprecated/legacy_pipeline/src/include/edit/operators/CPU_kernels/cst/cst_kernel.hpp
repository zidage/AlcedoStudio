//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <OpenColorIO/OpenColorIO.h>
#include <OpenColorIO/OpenColorTypes.h>

#include <filesystem>
#include <optional>

#include "edit/operators/op_base.hpp"
#include "edit/operators/op_kernel.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
struct OCIO_ACES_Transform_Op_Kernel : PointOpTag {
  inline void operator()(Pixel&, OperatorParams&) const {
    // // The pair of transform ops should always be enabled.
    // if (params.is_working_space_) {
    //   params.cpu_to_working_processor_->applyRGBA(&p.r_);
    //   params.is_working_space_ = false;
    // } else {
    //   params.cpu_to_output_processor_->applyRGBA(&p.r_);
    //   params.is_working_space_ = true;
    // }
  }
};

}  // namespace alcedo