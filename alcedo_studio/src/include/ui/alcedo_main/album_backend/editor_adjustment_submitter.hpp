//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

namespace alcedo::ui {

/// Narrow QML-facing seam used by typed adjustment models to enqueue one
/// adjustment field write at a time. `EditorSessionController` implements
/// this; tests inject a fake. The models never touch the pipeline scheduler,
/// the render coordinator, or the journal — they only call this seam, which
/// forwards to `IEditorSessionBackend::EnqueueAdjustmentInput`. True means the
/// write was accepted for later owner processing, not that live parameters or
/// history were updated. One `settled=true` call per completed pointer drag or
/// stabilized keyboard/wheel input sequence seals that sequence with Release.
class IEditorAdjustmentSubmitter {
 public:
  virtual ~IEditorAdjustmentSubmitter() = default;

  /// Enqueue one adjustment field write. `settled=false` continues the current
  /// input sequence; `settled=true` also seals it with Release. Returns false
  /// when the session cannot accept input (no image, or not Interactive).
  virtual auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool = 0;

  /// True when the session has an image and is in the Interactive state, i.e.
  /// adjustment controls should be enabled and `submitPatch` will be accepted.
  virtual auto canEdit() const -> bool = 0;
};

}  // namespace alcedo::ui
