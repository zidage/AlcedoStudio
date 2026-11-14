// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once

#include "image/dcp_camera_profile.hpp"
#include "image/dng_color_profile.hpp"

namespace alcedo {

/**
 * @brief The tables of a DCP profile for one DNG source (dcp_camera_profile_plan.md, phase C6).
 *
 * A DNG file keeps its own camera calibration when the user selects a DCP profile. The result
 * holds:
 * - `AnalogBalance` and `BaselineExposure` of @p source;
 * - `CameraCalibration1/2` of `source.source_calibration` when its signature equals
 *   `dcp.profile_calibration_signature` (CameraCalibrationSignaturesMatch), identity otherwise;
 * - the name, the two hue and saturation maps, the look table, their encodings, and
 *   `BaselineExposureOffset` of `dcp.tables`.
 *
 * The calibration of the result is resolved, so its `source_calibration` is empty. The function
 * computes a new profile on each call; DcpProfileLibrary::SourceTables shares the results.
 *
 * @throws std::runtime_error when MakeDngColorProfile rejects the merged values.
 */
[[nodiscard]] auto MergeDcpTablesWithSourceCalibration(const DcpCameraProfile& dcp,
                                                       const DngColorProfile&  source)
    -> DngColorProfilePtr;

}  // namespace alcedo
