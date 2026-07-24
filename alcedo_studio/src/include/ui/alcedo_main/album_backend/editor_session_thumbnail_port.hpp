//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>

#include "app/editor_session_ports.hpp"
#include "type/type.hpp"

namespace alcedo::ui {

/// Owns the thumbnail refresh callback used after a checkpoint succeeds. It
/// has no knowledge of history, storage, tasks, or rendering.
class EditorSessionThumbnailPort final : public alcedo::IEditorThumbnailPort {
 public:
  /// Construct a port around the focused-thumbnail refresh callback.
  explicit EditorSessionThumbnailPort(std::function<void(sl_element_id_t)> refresh_thumbnail);

  /// Refresh one image's focused thumbnail after a successful checkpoint.
  void RefreshAfterMaterialization(sl_element_id_t element_id) override;

 private:
  std::function<void(sl_element_id_t)> refresh_thumbnail_;
};

}  // namespace alcedo::ui
