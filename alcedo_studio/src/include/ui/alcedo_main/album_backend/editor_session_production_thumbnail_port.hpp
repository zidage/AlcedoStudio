//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <mutex>

#include "app/editor_session_ports.hpp"
#include "type/type.hpp"

namespace alcedo::ui {

/// Production thumbnail invalidation port. Wraps a single callback registered at
/// construction time; the port does not own ThumbnailService, schedule tasks, or
/// know about pipeline/checkpoint state.
///
/// Thread context: Invalidate may be called from the save worker while the
/// global save lock is held. The underlying callback is responsible for thread
/// safety.
class EditorSessionProductionThumbnailPort final : public alcedo::IEditorThumbnailPort {
 public:
  /// @param invalidate_thumbnail  Callback invoked on Invalidate. Must be
  ///                              thread-safe and reentrant.
  explicit EditorSessionProductionThumbnailPort(
      std::function<void(sl_element_id_t)> invalidate_thumbnail);

  /// Invalidate the thumbnail for an element. Called only after a successful
  /// DuckDB materialization. If the callback throws or is null, the error is
  /// silently consumed (thumbnail invalidation is an acceleration step, not a
  /// correctness requirement).
  void Invalidate(sl_element_id_t element_id) override;

 private:
  std::function<void(sl_element_id_t)> invalidate_thumbnail_;
};

}  // namespace alcedo::ui
