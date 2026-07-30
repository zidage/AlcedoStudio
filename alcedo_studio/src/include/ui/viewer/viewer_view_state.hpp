//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "ui/edit_viewer/viewer_state.hpp"

namespace alcedo {

// Shared viewer state consumed by the legacy surface and the unified
// QQuickRhiItem renderer. It contains no surface or widget ownership.
struct ViewerViewState {
  ViewerStateSnapshot snapshot{};
  bool                prefer_interactive_primary = false;
  bool                allow_detail_patch         = true;
  bool                has_expected_detail_token  = false;
  std::uint64_t       expected_detail_generation = 0;
  std::uint64_t       expected_detail_serial     = 0;
};

}  // namespace alcedo
