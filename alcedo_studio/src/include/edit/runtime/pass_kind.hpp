//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

namespace alcedo {

/**
 * @brief Backend work items produced by GraphCompiler. Not serialized.
 *
 * CUDA full-frame Develop order is Linearize, optional CFA clamp, Demosaic,
 * then HighlightRecover on RGB. That matches ProcessCudaFullFrame, not the CPU
 * Linearize → HLR → Debayer path.
 */
enum class GpuPassKind : std::uint8_t {
  UploadRaw         = 0,
  UploadRgb         = 1,
  Linearize         = 2,
  CfaClamp          = 3,
  Demosaic          = 4,
  HighlightRecover  = 5,
  InverseCamMulPack = 6,
  Lens              = 7,
  GeometryResample  = 8,
  CameraToAp1       = 9,
  MaskEvaluate      = 10,
  MaskFeather       = 11,
  PrimaryColorGrade = 12,
  Drt               = 13,
  MaskUnion         = 14,
};

[[nodiscard]] inline auto GpuPassKindName(GpuPassKind kind) -> const char* {
  switch (kind) {
    case GpuPassKind::UploadRaw:
      return "UploadRaw";
    case GpuPassKind::UploadRgb:
      return "UploadRgb";
    case GpuPassKind::Linearize:
      return "Linearize";
    case GpuPassKind::CfaClamp:
      return "CfaClamp";
    case GpuPassKind::Demosaic:
      return "Demosaic";
    case GpuPassKind::HighlightRecover:
      return "HighlightRecover";
    case GpuPassKind::InverseCamMulPack:
      return "InverseCamMulPack";
    case GpuPassKind::Lens:
      return "Lens";
    case GpuPassKind::GeometryResample:
      return "GeometryResample";
    case GpuPassKind::CameraToAp1:
      return "CameraToAp1";
    case GpuPassKind::MaskEvaluate:
      return "MaskEvaluate";
    case GpuPassKind::MaskFeather:
      return "MaskFeather";
    case GpuPassKind::PrimaryColorGrade:
      return "PrimaryColorGrade";
    case GpuPassKind::Drt:
      return "Drt";
    case GpuPassKind::MaskUnion:
      return "MaskUnion";
  }
  return "Unknown";
}

}  // namespace alcedo
