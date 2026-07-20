//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

namespace alcedo::ui {

/// Narrow QML-facing seam used by the Phase 6A typed adjustment models to
/// submit one adjustment patch at a time. `EditorSessionController` implements
/// this; tests inject a fake. The models never touch the pipeline scheduler,
/// the render coordinator, or the journal — they only call this seam, which
/// forwards to `EditorSessionService::Patch` (interactive) or `GestureCommit`
/// (settled). One `settled=true` call per completed gesture is the contract.
class IEditorAdjustmentSubmitter {
 public:
  virtual ~IEditorAdjustmentSubmitter() = default;

  /// Submit one adjustment patch. `settled=false` requests an interactive
  /// preview; `settled=true` finalizes the value as one committed transaction
  /// (full-quality render). Returns false when the session cannot accept
  /// patches right now (no image, or not in the Interactive state). Callers
  /// must check `canEdit()` first and must drop the call silently when this
  /// returns false.
  virtual auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool = 0;

  /// True when the session has an image and is in the Interactive state, i.e.
  /// adjustment controls should be enabled and `submitPatch` will be accepted.
  virtual auto canEdit() const -> bool = 0;
};

}  // namespace alcedo::ui