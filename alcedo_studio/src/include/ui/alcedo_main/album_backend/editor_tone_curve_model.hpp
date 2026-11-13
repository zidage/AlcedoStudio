//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QList>
#include <QPointF>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <array>
#include <vector>

#include "edit/operators/models/curve_model.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"

namespace alcedo::ui {

/**
 * @brief Panel model of the Tone Curve section: the four curves of the Curve adjustment.
 *
 * Owns the control points of the RGB, red, green, and blue curves in normalized [0, 1] x [0, 1]
 * space (the curve axis), the selected channel, the selected point, and the state of a pointer
 * drag or a key move sequence. The plot layers of the section only report positions; this model
 * moves the points and writes them.
 *
 * Writes: a user edit enqueues an `EditorCurveWrite` with only the list of the selected curve, so
 * the other curves stay as they are. Each completed drag, key move sequence, field edit, removal,
 * and reset gives exactly one settled write (see the TC5 section of tone_curve_editor_plan.md).
 *
 * Load: @ref loadFromSnapshotEntry and @ref setControlPoints replace points without a write. The
 * selected channel is panel state and a load does not change it.
 *
 * Point rules: as `curve::NormalizeCurveControlPoints` (2 to 16 points, x increases by at least
 * 0.001, coordinates in [0, 1]). The end points stay; only interior points can be removed.
 */
class EditorToneCurveModel : public EditorAdjustmentModelBase {
  Q_OBJECT
  /// Selected curve: 0 RGB, 1 red, 2 green, 3 blue.
  Q_PROPERTY(int channel READ channel WRITE setChannel NOTIFY channelChanged)
  /// Control points of the selected curve.
  Q_PROPERTY(QList<QPointF> points READ points NOTIFY pointsChanged)
  Q_PROPERTY(int pointCount READ pointCount NOTIFY pointsChanged)
  Q_PROPERTY(
      int selectedIndex READ selectedIndex WRITE setSelectedIndex NOTIFY selectedIndexChanged)
  Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectedIndexChanged)
  /// Coordinates of the selected point; 0 without a selection.
  Q_PROPERTY(double selectedX READ selectedX NOTIFY selectedPointChanged)
  Q_PROPERTY(double selectedY READ selectedY NOTIFY selectedPointChanged)
  Q_PROPERTY(bool dragActive READ dragActive NOTIFY dragActiveChanged)
  Q_PROPERTY(int maxControlPoints READ maxControlPoints CONSTANT)
  /// Sampled lines of the four curves (curve::SampleCurveLine).
  Q_PROPERTY(QList<QPointF> rgbLine READ rgbLine NOTIFY curvesChanged)
  Q_PROPERTY(QList<QPointF> redLine READ redLine NOTIFY curvesChanged)
  Q_PROPERTY(QList<QPointF> greenLine READ greenLine NOTIFY curvesChanged)
  Q_PROPERTY(QList<QPointF> blueLine READ blueLine NOTIFY curvesChanged)
  /// Four booleans in channel order: true for a curve that is not the identity.
  Q_PROPERTY(QVariantList channelModified READ channelModified NOTIFY curvesChanged)
  /// The Tone Curve presets in menu order: maps with `id`, `name` (English), and `group`.
  Q_PROPERTY(QVariantList presets READ presets CONSTANT)
  /// Id of the preset whose points equal the selected curve; empty for other points (Custom).
  Q_PROPERTY(QString presetId READ presetId NOTIFY pointsChanged)
  /// 18% grey and one stop on the curve axis.
  Q_PROPERTY(double axisGrey READ axisGrey CONSTANT)
  Q_PROPERTY(double axisStop READ axisStop CONSTANT)

 public:
  static constexpr int    kChannelCount = static_cast<int>(alcedo::kCurveChannelCount);
  /// Arrow key step on the curve axis; with Shift ten times larger.
  static constexpr double kKeyStep      = 0.001;
  static constexpr double kKeyStepLarge = 0.01;
  /// Time after the last arrow key that settles a key move sequence.
  static constexpr int    kDefaultKeySettleIntervalMs = 500;

  explicit EditorToneCurveModel(QObject* parent = nullptr);

  [[nodiscard]] auto channel() const -> int { return channel_; }
  /// Selects the curve that the section edits. Settles a pending key move sequence first and
  /// clears the selected point. No write of its own.
  void               setChannel(int channel);
  [[nodiscard]] auto points() const -> QList<QPointF>;
  [[nodiscard]] auto pointCount() const -> int { return static_cast<int>(Selected().size()); }
  [[nodiscard]] auto selectedIndex() const -> int { return selectedIndex_; }
  /// Selects a point of the selected curve (-1 clears). Settles a pending key move sequence.
  void               setSelectedIndex(int index);
  [[nodiscard]] auto hasSelection() const -> bool { return selectedIndex_ >= 0; }
  [[nodiscard]] auto selectedX() const -> double;
  [[nodiscard]] auto selectedY() const -> double;
  [[nodiscard]] auto dragActive() const -> bool { return dragActive_; }
  [[nodiscard]] static auto maxControlPoints() -> int;
  [[nodiscard]] auto        rgbLine() const -> QList<QPointF> { return lines_[0]; }
  [[nodiscard]] auto        redLine() const -> QList<QPointF> { return lines_[1]; }
  [[nodiscard]] auto        greenLine() const -> QList<QPointF> { return lines_[2]; }
  [[nodiscard]] auto        blueLine() const -> QList<QPointF> { return lines_[3]; }
  [[nodiscard]] auto        channelModified() const -> QVariantList;
  [[nodiscard]] static auto presets() -> QVariantList;
  [[nodiscard]] auto        presetId() const -> QString;
  [[nodiscard]] static auto axisGrey() -> double;
  [[nodiscard]] static auto axisStop() -> double;

  /// Load-only: replace the points of the selected curve. No write.
  Q_INVOKABLE void setPoints(const QVariantList& points);
  /**
   * @brief Load-only from the projected `curve` entry of an adjustment snapshot.
   *
   * Accepts `{"curve":{"points":[...],"red":[...],"green":[...],"blue":[...]}}` or the inner map.
   * An absent channel list is the identity. No write; nothing while a drag runs.
   */
  Q_INVOKABLE void loadFromSnapshotEntry(const QVariantMap& entry);
  /// Load-only for the selected curve (tests and C++ callers).
  void                      setControlPoints(const std::vector<QPointF>& points);
  /// Load-only for @p channel.
  void               setChannelControlPoints(int channel, const std::vector<QPointF>& points);
  [[nodiscard]] auto controlPoints() const -> const std::vector<QPointF>& { return Selected(); }
  [[nodiscard]] auto channelControlPoints(int channel) const -> const std::vector<QPointF>&;

  /// Value of the curve of @p channel at @p x (the render spline).
  Q_INVOKABLE double evaluate(int channel, double x) const;

  /// Starts a drag of point @p index of the selected curve and selects it. No write yet.
  Q_INVOKABLE void beginDrag(int index);
  /// Moves the dragged point to (@p x, @p y). With Shift in @p modifiers only the axis with the
  /// larger motion from the press position moves. Writes an interactive preview when the point
  /// changed.
  Q_INVOKABLE void   updateDrag(double x, double y, int modifiers = 0);
  /// Ends the drag. Writes one settled entry when the point moved or was added during the drag.
  Q_INVOKABLE void finishDrag();
  /// Adds a point on the selected curve at @p x, at the curve value, selects it, and starts a drag
  /// on it. Returns its index, or -1 when the curve has 16 points or @p x is within 0.001 of a
  /// point. No write yet; the drag writes at release.
  Q_INVOKABLE int    insertPointOnCurve(double x);
  /// Arrow key move of the selected point by (@p dx, @p dy). Writes an interactive preview and
  /// starts the settle timer of the key move sequence.
  Q_INVOKABLE void   nudgeSelected(double dx, double dy);
  /// Arrow, Delete, and Backspace keys of the selected point (Qt::Key and Qt::KeyboardModifiers).
  Q_INVOKABLE bool   handleKey(int key, int modifiers);
  /// In field edit: x of the selected point. One settled write when it changed.
  Q_INVOKABLE bool   setSelectedXValue(double x);
  /// Out field edit: y of the selected point. One settled write when it changed.
  Q_INVOKABLE bool   setSelectedYValue(double y);
  /// Removes interior point @p index of the selected curve. One settled write.
  Q_INVOKABLE bool removePoint(int index);
  Q_INVOKABLE bool   removeSelected();
  Q_INVOKABLE void   clearSelection() { setSelectedIndex(-1); }
  /// Sets the selected curve to the identity. One settled write when it changed.
  Q_INVOKABLE void reset();
  /// Replaces the points of the selected curve with the points of preset @p id. One settled write
  /// when they changed. Returns false for an unknown id and during a drag.
  Q_INVOKABLE bool          applyPreset(const QString& id);
  /// Writes the settled entry of a pending key move sequence now.
  Q_INVOKABLE void   settlePendingKeyMoves();
  [[nodiscard]] Q_INVOKABLE bool    hasPendingKeyMoves() const { return keyMovesPending_; }
  /// Operator-shaped params JSON of the RGB curve.
  [[nodiscard]] Q_INVOKABLE QString paramsJson() const;

  /// C++ test hook: settle interval of a key move sequence.
  void                              setKeySettleIntervalMs(int ms);

 signals:
  void channelChanged();
  void pointsChanged();
  void selectedIndexChanged();
  void selectedPointChanged();
  void dragActiveChanged();
  void curvesChanged();
  /// Emitted when a settled write lands (drag release, key sequence, field, removal, reset).
  void settledCommitted();

 private:
  [[nodiscard]] auto Selected() const -> const std::vector<QPointF>& {
    return curves_[static_cast<size_t>(channel_)];
  }
  /// Replace the points of @p channel with the normalized @p points; returns true when they
  /// changed. Emits the change signals.
  auto               ReplaceChannelPoints(int channel, std::vector<QPointF> points) -> bool;
  /// Move point @p index of the selected curve inside its neighbor limits; true when it moved.
  auto               MoveSelectedCurvePoint(int index, double x, double y) -> bool;
  void               RebuildLine(int channel);
  void               SetSelectedIndexInternal(int index);
  void               SetDragActive(bool active);
  void               SubmitSelected(bool settled);
  void               CommitSettled();
  [[nodiscard]] auto SelectedCurveWrite() const -> alcedo::EditorCurveWrite;

  std::array<std::vector<QPointF>, alcedo::kCurveChannelCount> curves_{};
  std::array<QList<QPointF>, alcedo::kCurveChannelCount>       lines_{};
  int                                                          channel_       = 0;
  int                                                          selectedIndex_ = -1;
  bool                                                         dragActive_    = false;
  bool                                                         dragChanged_   = false;
  QPointF                                                      dragOrigin_{};
  bool                                                         keyMovesPending_ = false;
  QTimer*                                                      keySettleTimer_  = nullptr;
};

}  // namespace alcedo::ui
