//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/grade_lut.hpp"

#include <stdexcept>
#include <string>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lmt_model.hpp"

namespace alcedo {

auto PackCubeLutRgba(const CubeLut& lut) -> std::vector<std::byte> {
  const auto             edge   = static_cast<std::size_t>(lut.edge3d_);
  const auto             voxels = edge * edge * edge;
  std::vector<std::byte> packed(voxels * 4 * sizeof(float));
  auto*                  out = reinterpret_cast<float*>(packed.data());
  for (std::size_t i = 0; i < voxels; ++i) {
    out[i * 4 + 0] = lut.lut3d_[i * 3 + 0];
    out[i * 4 + 1] = lut.lut3d_[i * 3 + 1];
    out[i * 4 + 2] = lut.lut3d_[i * 3 + 2];
    out[i * 4 + 3] = 1.0f;
  }
  return packed;
}

auto TryPackGradeLut(const ColorGradeNodeModel& grade) -> std::optional<PackedGradeLut> {
  const auto* model = dynamic_cast<const LmtModel*>(grade.FindAdjustmentByType(type_ids::Lmt()));
  if (model == nullptr || model->CubePath().empty()) {
    return std::nullopt;
  }

  CubeLut     cube;
  std::string error;
  if (!ParseCubeFile(model->CubePath(), cube, &error) || !cube.Has3D()) {
    throw std::runtime_error("Primary grade: failed to load LMT cube '" + model->CubePath() +
                             "': " + error);
  }
  PackedGradeLut packed;
  packed.rgba = PackCubeLutRgba(cube);
  packed.edge = static_cast<std::uint32_t>(cube.edge3d_);
  return packed;
}

}  // namespace alcedo
