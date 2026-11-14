//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <cstdint>
#include <memory>
#include <optional>

#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/curve_input_histogram.hpp"
#include "edit/scope/scope_analyzer.hpp"
#include "ui/alcedo_main/plot_pane/plot_histogram_layer.hpp"

namespace alcedo::ui {

/**
 * @brief QML-facing owner of the histogram at the input of the Curve adjustment (issue #888).
 *
 * The Tone Curve section sets @ref active (section open and its Histogram toggle on).
 * EditorSessionController sets @ref gradeNodeId to the Color Grade whose curve the panel
 * projection shows (the Grade that the section edits). While both are set, @ref request
 * gives the request that the editor session shows (EditorSessionController calls
 * IEditorSessionBackend::ShowCurveInputHistogram on @ref RequestChanged), so the editor renders of
 * the whole frame count the curve input of that Grade. Otherwise there is no request and no
 * render counts a histogram.
 *
 * The renders give their counts to @ref sink on the render thread; the sink moves them to the
 * thread of this object. A result is accepted only for the current image identity and session
 * epoch (SetImageIdentity), the current Grade, and while active. It is published as
 * ScopeHistogramRenderData through BuildScopeHistogramRenderData, the display heights of the scope
 * histogram. A render that did not evaluate the Grade clears the histogram.
 *
 * It is the PlotHistogramSource of the PlotHistogramLayer of the Tone Curve section: the layer
 * reads @ref histogram after HistogramChanged.
 *
 * Thread: the owner thread (the Qt GUI thread), except @ref sink, which any thread can call.
 */
class EditorCurveHistogramController final : public PlotHistogramSource {
  Q_OBJECT
  Q_PROPERTY(bool active READ active WRITE set_active NOTIFY ActiveChanged)
  Q_PROPERTY(
      QString gradeNodeId READ grade_node_id WRITE set_grade_node_id NOTIFY GradeNodeIdChanged)
  Q_PROPERTY(bool hasHistogram READ has_histogram NOTIFY HistogramChanged)
  Q_PROPERTY(int bins READ bins NOTIFY HistogramChanged)
  Q_PROPERTY(qulonglong histogramGeneration READ histogram_generation NOTIFY HistogramChanged)

 public:
  explicit EditorCurveHistogramController(QObject* parent = nullptr);
  ~EditorCurveHistogramController() override;

  EditorCurveHistogramController(const EditorCurveHistogramController&)            = delete;
  EditorCurveHistogramController& operator=(const EditorCurveHistogramController&) = delete;

  [[nodiscard]] auto              active() const -> bool { return active_; }
  void                            set_active(bool active);
  [[nodiscard]] auto              grade_node_id() const -> QString;
  void                            set_grade_node_id(const QString& grade_node_id);
  [[nodiscard]] auto              has_histogram() const -> bool { return histogram_.valid; }
  [[nodiscard]] auto              bins() const -> int { return histogram_.bins; }
  /// Increases with each published or cleared histogram.
  [[nodiscard]] auto              histogram_generation() const -> qulonglong {
    return static_cast<qulonglong>(generation_);
  }

  /// Display heights of the current histogram; `valid` is false when there is none.
  [[nodiscard]] auto histogram() const -> const alcedo::ScopeHistogramRenderData& override {
    return histogram_;
  }

  /**
   * @brief Image that the editor shows. A change clears the histogram; a later result of
   *        another identity is dropped.
   */
  void               SetImageIdentity(std::uint64_t image_identity, std::uint64_t session_epoch);

  /// Grade and receiver to show while active with a Grade; nullopt otherwise.
  [[nodiscard]] auto request() const -> std::optional<alcedo::CurveInputHistogramRequest>;

  /// Receiver of the render results. Thread-safe; it stays valid after this object is destroyed
  /// and then drops what it receives.
  [[nodiscard]] auto sink() const -> std::shared_ptr<alcedo::ICurveInputHistogramSink>;

 signals:
  void ActiveChanged();
  void GradeNodeIdChanged();
  // HistogramChanged is the signal of PlotHistogramSource.
  /// @ref request changed: the session must show the new request or hide the histogram.
  void RequestChanged();

 private:
  class Mailbox;

  /// Accept or drop one render result on the owner thread.
  void                             Accept(const alcedo::CurveInputHistogram& result);
  void                             ClearHistogram();

  std::shared_ptr<Mailbox>         mailbox_;
  alcedo::ScopeHistogramRenderData histogram_{};
  alcedo::NodeId                   grade_id_;
  std::uint64_t                    image_identity_ = 0;
  std::uint64_t                    session_epoch_  = 0;
  std::uint64_t                    generation_     = 0;
  bool                             active_         = false;
};

}  // namespace alcedo::ui
