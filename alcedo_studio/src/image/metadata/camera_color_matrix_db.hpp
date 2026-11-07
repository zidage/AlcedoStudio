//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

#include "decoders/processor/raw_color_context.hpp"

namespace alcedo::metadata_detail {

/// Look up Adobe DNG colour matrices for a camera make/model pair and store
/// the result directly into the provided double[9] arrays.
auto LookupCameraColorMatrices(const std::string& camera_make, const std::string& camera_model,
                               double cm1_out[9], double cm2_out[9]) -> bool;

/// Set the calibration illuminants of matrices from LookupCameraColorMatrices: Standard Light A
/// and D65.
void MarkAdobeCameraMatrixDatabaseIlluminants(RawRuntimeColorContext& ctx);

}  // namespace alcedo::metadata_detail
