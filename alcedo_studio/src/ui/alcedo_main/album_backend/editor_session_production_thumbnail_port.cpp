//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_production_thumbnail_port.hpp"

#include <utility>

namespace alcedo::ui {

EditorSessionProductionThumbnailPort::EditorSessionProductionThumbnailPort(
    std::function<void(sl_element_id_t)> invalidate_thumbnail)
    : invalidate_thumbnail_(std::move(invalidate_thumbnail)) {}

void EditorSessionProductionThumbnailPort::Invalidate(sl_element_id_t element_id) {
  if (invalidate_thumbnail_) {
    try {
      invalidate_thumbnail_(element_id);
    } catch (...) {
      // Thumbnail invalidation is an acceleration step. A committed
      // history and serialized pipeline state remain durable if it cannot run.
    }
  }
}

}  // namespace alcedo::ui
