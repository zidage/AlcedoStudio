//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <QVariantList>
#include <vector>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"

namespace alcedo::ui {

/// Phase 6B tone-curve adjustment model. Owns ordered control points in
/// normalized [0, 1]×[0, 1] space, pointer-drag state, insert/
/// remove, and reset. Submits operator-shaped params JSON through the same
/// `IEditorAdjustmentSubmitter` seam as the numeric models:
///   {"curve":{"size":N,"points":[{"x":…,"y":…},…]}}
/// One `settled=true` submit per completed drag; interactive previews use
/// `settled=false` while dragging.
///
/// Point ordering, spacing, and endpoint rules match `curve::NormalizeCurveControlPoints`
/// and the legacy `ToneCurveWidget`. Load uses `setPoints` (no submit); user
/// edits go through the invokable drag and mutation methods.
class EditorToneCurveModel : public EditorAdjustmentModelBase {
  Q_OBJECT
  Q_PROPERTY(QVariantList points READ points NOTIFY pointsChanged)
  Q_PROPERTY(int pointCount READ pointCount NOTIFY pointsChanged)
  Q_PROPERTY(int activeIndex READ activeIndex NOTIFY activeIndexChanged)
  Q_PROPERTY(bool dragActive READ dragActive NOTIFY dragActiveChanged)
  Q_PROPERTY(int maxControlPoints READ maxControlPoints CONSTANT)

 public:
  explicit EditorToneCurveModel(QObject* parent = nullptr);

  [[nodiscard]] auto points() const -> QVariantList;
  [[nodiscard]] auto pointCount() const -> int {
    return static_cast<int>(points_.size());
  }
  [[nodiscard]] auto activeIndex() const -> int { return activeIndex_; }
  [[nodiscard]] auto dragActive() const -> bool { return dragActive_; }
  [[nodiscard]] static auto maxControlPoints() -> int;

  /// Plain load setter: normalize + replace points. Does NOT submit.
  Q_INVOKABLE void setPoints(const QVariantList& points);
  /// C++ load path used by tests and panel controllers.
  void setControlPoints(const std::vector<QPointF>& points);
  [[nodiscard]] auto controlPoints() const -> const std::vector<QPointF>& { return points_; }

  /// Begin a drag on an existing handle (or after insert). No submit yet.
  Q_INVOKABLE void beginDrag(int index);
  /// Move the active handle to normalized (x, y); submits interactive preview.
  Q_INVOKABLE void updateDrag(double x, double y);
  /// End the drag; submits one settled transaction.
  Q_INVOKABLE void finishDrag();
  /// Insert a control point at normalized (x, y) (if spacing allows), start a
  /// drag on it, and submit one interactive preview.
  Q_INVOKABLE int insertPoint(double x, double y);
  /// Remove an interior point (endpoints are pinned). Commits one settled
  /// transaction when the set actually changes.
  Q_INVOKABLE bool removePoint(int index);
  /// Restore the default linear curve and commit one settled transaction.
  Q_INVOKABLE void reset();
  /// Operator-shaped params JSON for the current (normalized) points.
  [[nodiscard]] Q_INVOKABLE QString paramsJson() const;

 signals:
  void pointsChanged();
  void activeIndexChanged();
  void dragActiveChanged();
  /// Emitted when a settled commit lands (drag release, remove, reset).
  void settledCommitted();

 private:
  void applyNormalized(std::vector<QPointF> points);
  void setActiveIndex(int index);
  void submitInteractive();
  void submitSettled();
  [[nodiscard]] auto buildParamsJson() const -> QString;
  auto moveActivePoint(double x, double y) -> bool;

  std::vector<QPointF> points_;
  int                  activeIndex_   = -1;
  bool                 dragActive_ = false;
};

}  // namespace alcedo::ui
