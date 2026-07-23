//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>

#include "app/editor_session_ports.hpp"
#include "type/type.hpp"

namespace alcedo::ui {

/// Owns the single thumbnail invalidation callback used after a checkpoint
/// succeeds. It has no knowledge of history, storage, tasks, or rendering.
class EditorSessionThumbnailPort final : public alcedo::IEditorThumbnailPort {
 public:
  /// Construct a port around the thumbnail invalidation callback.
  explicit EditorSessionThumbnailPort(std::function<void(sl_element_id_t)> invalidate_thumbnail);

  /// Invalidate the thumbnail for one image after a successful checkpoint.
  void Invalidate(sl_element_id_t element_id) override;

 private:
  std::function<void(sl_element_id_t)> invalidate_thumbnail_;
};

}  // namespace alcedo::ui
