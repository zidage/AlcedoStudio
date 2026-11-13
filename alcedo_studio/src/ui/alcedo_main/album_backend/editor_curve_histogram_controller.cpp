//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_curve_histogram_controller.hpp"

#include <QMetaObject>
#include <mutex>
#include <utility>

namespace alcedo::ui {

/**
 * @brief Sink that the renders call on the render thread. It posts each result to the owner
 *        thread. The owner detaches it in its destructor, so a later result is dropped.
 */
class EditorCurveHistogramController::Mailbox final : public alcedo::ICurveInputHistogramSink {
 public:
  explicit Mailbox(EditorCurveHistogramController* owner) : owner_(owner) {}

  void Detach() {
    std::scoped_lock lock(mutex_);
    owner_ = nullptr;
  }

  void PublishCurveInputHistogram(alcedo::CurveInputHistogram histogram) override {
    std::scoped_lock lock(mutex_);
    if (owner_ == nullptr) {
      return;
    }
    // Qt drops the queued call when the owner is destroyed before it runs.
    QMetaObject::invokeMethod(
        owner_, [owner = owner_, histogram = std::move(histogram)]() { owner->Accept(histogram); },
        Qt::QueuedConnection);
  }

 private:
  std::mutex                      mutex_;
  EditorCurveHistogramController* owner_ = nullptr;
};

EditorCurveHistogramController::EditorCurveHistogramController(QObject* parent)
    : PlotHistogramSource(parent), mailbox_(std::make_shared<Mailbox>(this)) {}

EditorCurveHistogramController::~EditorCurveHistogramController() { mailbox_->Detach(); }

void EditorCurveHistogramController::set_active(bool active) {
  if (active_ == active) {
    return;
  }
  active_ = active;
  if (!active_) {
    ClearHistogram();
  }
  emit ActiveChanged();
  emit RequestChanged();
}

auto EditorCurveHistogramController::grade_node_id() const -> QString {
  const auto value = grade_id_.Value();
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

void EditorCurveHistogramController::set_grade_node_id(const QString& grade_node_id) {
  alcedo::NodeId grade_id{grade_node_id.toStdString()};
  if (grade_id == grade_id_) {
    return;
  }
  grade_id_ = std::move(grade_id);
  ClearHistogram();
  emit GradeNodeIdChanged();
  emit RequestChanged();
}

void EditorCurveHistogramController::SetImageIdentity(std::uint64_t image_identity,
                                                      std::uint64_t session_epoch) {
  if (image_identity == image_identity_ && session_epoch == session_epoch_) {
    return;
  }
  image_identity_ = image_identity;
  session_epoch_  = session_epoch;
  ClearHistogram();
}

auto EditorCurveHistogramController::request() const
    -> std::optional<alcedo::CurveInputHistogramRequest> {
  if (!active_ || grade_id_.Empty()) {
    return std::nullopt;
  }
  return alcedo::CurveInputHistogramRequest{grade_id_, mailbox_};
}

auto EditorCurveHistogramController::sink() const
    -> std::shared_ptr<alcedo::ICurveInputHistogramSink> {
  return mailbox_;
}

void EditorCurveHistogramController::Accept(const alcedo::CurveInputHistogram& result) {
  if (!active_ || result.grade_id != grade_id_ || result.image_identity != image_identity_ ||
      result.session_epoch != session_epoch_) {
    return;
  }
  if (!result.Evaluated()) {
    ClearHistogram();
    return;
  }
  histogram_ = alcedo::BuildScopeHistogramRenderData(
      result.counts, static_cast<int>(alcedo::kCurveInputHistogramBins));
  ++generation_;
  emit HistogramChanged();
}

void EditorCurveHistogramController::ClearHistogram() {
  if (!histogram_.valid) {
    return;
  }
  histogram_ = {};
  ++generation_;
  emit HistogramChanged();
}

}  // namespace alcedo::ui
