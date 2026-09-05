//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <string>

#include "app/editor_session_ports.hpp"
#include "app/editor_session_types.hpp"

namespace alcedo::ui {

struct HistoryWorkingState;
class EditorHistoryState;

/// Extracted projection unit. Reads the history and adjustment snapshots for
/// QML models. Copies mutable graph values under the short WorkingState lock,
/// then parses, sorts, and formats outside that lock.
class EditorHistoryProjection {
 public:
  explicit EditorHistoryProjection(EditorHistoryState& state);

  /// Read the active Version identity without walking refs or commits.
  auto ReadActiveVersionId(const alcedo::EditorHistoryGuardHandle& guard,
                           alcedo::version_ref_id_t* version_id, std::string* error) -> bool;

  /// Project the named refs and active first-parent path for the QML model.
  auto ReadHistorySnapshot(const alcedo::EditorHistoryGuardHandle& guard,
                           alcedo::EditorHistorySnapshot* snapshot, std::string* error) -> bool;

  /// Return the committed adjustment snapshot for rendering and the UI.
  auto ReadAdjustmentSnapshot(const alcedo::EditorHistoryGuardHandle& guard,
                              alcedo::EditorRenderAdjustmentSnapshot* snapshot, std::string* error)
      -> bool;

 private:
  EditorHistoryState& state_;
};

}  // namespace alcedo::ui
