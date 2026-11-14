// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "image/dcp_source_tables.hpp"

#include <stdexcept>
#include <utility>

#include "image/dng_camera_matrix.hpp"

namespace alcedo {

auto MergeDcpTablesWithSourceCalibration(const DcpCameraProfile& dcp, const DngColorProfile& source)
    -> DngColorProfilePtr {
  if (!dcp.tables) throw std::runtime_error("DCP profile: tables are missing");
  const auto&     tables = *dcp.tables;
  DngColorProfile merged;
  merged.name                     = tables.name;
  merged.analog_balance           = source.analog_balance;
  merged.hue_sat_map_1            = tables.hue_sat_map_1;
  merged.hue_sat_map_2            = tables.hue_sat_map_2;
  merged.look_table               = tables.look_table;
  merged.baseline_exposure        = source.baseline_exposure;
  merged.baseline_exposure_offset = tables.baseline_exposure_offset;
  const auto& calibration         = source.source_calibration;
  if (CameraCalibrationSignaturesMatch(calibration.signature, dcp.profile_calibration_signature)) {
    merged.camera_calibration_1 = calibration.camera_calibration_1;
    merged.camera_calibration_2 = calibration.camera_calibration_2;
  }
  return MakeDngColorProfile(std::move(merged));
}

}  // namespace alcedo
