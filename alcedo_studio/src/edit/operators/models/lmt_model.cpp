//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/lmt_model.hpp"

#include "edit/operators/models/json_read.hpp"

namespace alcedo {

auto LmtModel::IsDefault() const -> bool {
  return Read([](const LmtPayload& payload) { return payload.cube_path.empty(); });
}

void LmtModel::SetCubePath(std::string path) {
  Mutate(LmtDirty::Path, [path = std::move(path)](LmtPayload& payload) mutable {
    payload.cube_path = std::move(path);
  });
}

auto LmtModel::CubePath() const -> std::string {
  return Read([](const LmtPayload& payload) { return payload.cube_path; });
}

auto LmtModel::ToJson() const -> nlohmann::json { return {{"cube_path", CubePath()}}; }

void LmtModel::LoadJson(const nlohmann::json& json) {
  SetCubePath(json_util::ReadString(json, "cube_path", {}));
}

}  // namespace alcedo
