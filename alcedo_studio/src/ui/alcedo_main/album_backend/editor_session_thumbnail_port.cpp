//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_thumbnail_port.hpp"

#include <utility>

namespace alcedo::ui {

EditorSessionThumbnailPort::EditorSessionThumbnailPort(
    std::function<void(sl_element_id_t)> invalidate_thumbnail)
    : invalidate_thumbnail_(std::move(invalidate_thumbnail)) {}

void EditorSessionThumbnailPort::Invalidate(sl_element_id_t element_id) {
  if (!invalidate_thumbnail_) return;
  try {
    invalidate_thumbnail_(element_id);
  } catch (...) {
    // A thumbnail is an acceleration cache; a committed checkpoint remains
    // valid when its invalidation callback cannot run.
  }
}

}  // namespace alcedo::ui
