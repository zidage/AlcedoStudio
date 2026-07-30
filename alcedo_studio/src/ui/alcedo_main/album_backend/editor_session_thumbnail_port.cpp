//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_thumbnail_port.hpp"

#include <utility>

namespace alcedo::ui {

EditorSessionThumbnailPort::EditorSessionThumbnailPort(
    std::function<void(sl_element_id_t)> refresh_thumbnail)
    : refresh_thumbnail_(std::move(refresh_thumbnail)) {}

void EditorSessionThumbnailPort::RefreshAfterMaterialization(sl_element_id_t element_id) {
  if (!refresh_thumbnail_) return;
  try {
    refresh_thumbnail_(element_id);
  } catch (...) {
    // A thumbnail is an acceleration cache; a durable checkpoint remains
    // valid when its refresh callback cannot run.
  }
}

}  // namespace alcedo::ui
