//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_scope_controller.hpp"

#include <QMetaObject>
#include <QThreadPool>
#include <algorithm>
#include <utility>

namespace alcedo::ui {
namespace {

constexpr auto kHistogramScopeMask = static_cast<uint32_t>(alcedo::ScopeType::Histogram);
constexpr auto kWaveformScopeMask  = static_cast<uint32_t>(alcedo::ScopeType::Waveform);

}  // namespace

EditorScopeController::EditorScopeController(QObject* parent)
    : EditorScopeController(alcedo::CreateDefaultScopeAnalyzer(), parent) {}

EditorScopeController::EditorScopeController(std::shared_ptr<alcedo::IScopeAnalyzer> analyzer,
                                             QObject*                                parent)
    : QObject(parent),
      analyzer_(analyzer ? std::move(analyzer) : alcedo::CreateDefaultScopeAnalyzer()),
      frame_tap_(std::make_unique<alcedo::FinalDisplayFrameTapSink>(nullptr, analyzer_)),
      poll_timer_(this) {
  // Scope is secondary UI. Keep it bounded and submit it away from the
  // display render deadline; only the active plot is calculated.
  request_.enabled_mask    = kHistogramScopeMask;
  request_.waveform_width  = 320;
  request_.waveform_height = 160;
  request_.target_fps      = 15;
  frame_tap_->SetScopeAnalysisDeferred(true);
  frame_tap_->SetScopeRequest(request_);
  poll_timer_.setInterval(1000 / std::max(1, request_.target_fps));
  poll_timer_.setTimerType(Qt::PreciseTimer);
  connect(&poll_timer_, &QTimer::timeout, this, &EditorScopeController::pollSnapshot);
  scope_pool_.setMaxThreadCount(1);
}

EditorScopeController::~EditorScopeController() { Shutdown(); }

void EditorScopeController::Shutdown() {
  // Stop launching new refresh work, drop queued tasks, and block until the
  // in-flight analysis finishes. The session calls this before releasing the
  // render pipeline so a scope worker never outlives the pipeline stream or
  // scratch buffers it would otherwise still reference.
  poll_timer_.stop();
  scope_pool_.clear();
  scope_pool_.waitForDone();
}

void EditorScopeController::set_visual_active(bool active) {
  if (visual_active_ == active) {
    return;
  }
  visual_active_ = active;
  frame_tap_->SetScopeActive(active);
  last_scheduled_display_generation_ = 0;
  last_scheduled_frame_id_           = 0;
  if (active) {
    poll_timer_.start();
  } else {
    poll_timer_.stop();
  }
  emit VisualActiveChanged();
}

void EditorScopeController::set_active_view(int view) {
  const int normalized = std::clamp(view, 0, 1);
  if (active_view_ == normalized) {
    return;
  }
  active_view_          = normalized;
  request_.enabled_mask = active_view_ == 0 ? kHistogramScopeMask : kWaveformScopeMask;
  ++request_revision_;
  frame_tap_->SetScopeRequest(request_);
  clearSnapshot();
  emit ActiveViewChanged();
  emit FrameRequested();
}

auto EditorScopeController::has_snapshot() const -> bool {
  if (snapshot_.generation == 0) {
    return false;
  }
  return active_view_ == 0 ? snapshot_.histogram.valid : snapshot_.waveform.valid;
}

void EditorScopeController::SetDownstreamSink(alcedo::IFrameSink* sink) {
  frame_tap_->SetDownstreamSink(sink);
}

auto EditorScopeController::frame_sink() const -> alcedo::IFrameSink* { return frame_tap_.get(); }

void EditorScopeController::SetImageIdentity(qulonglong image_identity,
                                             qulonglong image_generation) {
  const auto next_identity   = static_cast<std::uint64_t>(image_identity);
  const auto next_generation = static_cast<std::uint64_t>(image_generation);
  if (image_identity_ == next_identity && image_generation_ == next_generation) {
    return;
  }
  image_identity_   = next_identity;
  image_generation_ = next_generation;
  frame_tap_->SetFrameIdentity(image_identity_, image_generation_);
  if (image_identity_ == 0 && visual_active_) {
    set_visual_active(false);
  }
  clearSnapshot();
}

auto EditorScopeController::snapshot() const -> alcedo::ScopeRenderSnapshot { return snapshot_; }

bool EditorScopeController::refreshSnapshot() {
  if (!visual_active_ || !analyzer_) {
    return false;
  }

  (void)frame_tap_->SubmitCurrentDisplayFrameToScope();
  return refreshSnapshotNow();
}

auto EditorScopeController::refreshSnapshotNow() -> bool {
  if (!visual_active_ || !analyzer_) {
    return false;
  }

  const auto output = analyzer_->GetLatestOutput();
  if (output.generation == 0 ||
      (image_identity_ != 0 && output.image_identity != image_identity_) ||
      (image_generation_ != 0 && output.image_generation != image_generation_)) {
    return false;
  }

  auto next_snapshot = alcedo::ReadScopeRenderSnapshot(output);
  if (!next_snapshot.histogram.valid && !next_snapshot.waveform.valid) {
    return false;
  }
  return publishSnapshot(std::move(next_snapshot), image_identity_, image_generation_,
                         request_revision_);
}

void EditorScopeController::pollSnapshot() { scheduleSnapshotRefresh(); }

void EditorScopeController::scheduleSnapshotRefresh() {
  if (!visual_active_ || !analyzer_ || refresh_in_flight_->exchange(true)) {
    return;
  }

  const auto scope_frame = frame_tap_->GetCurrentScopeFrameView();
  if (!scope_frame) {
    refresh_in_flight_->store(false);
    return;
  }

  const auto expected_image_identity   = image_identity_;
  const auto expected_image_generation = image_generation_;
  const auto expected_request_revision = request_revision_;
  const auto request                   = request_;
  const bool submit_new_frame =
      scope_frame.display_generation != last_scheduled_display_generation_ ||
      scope_frame.frame_id != last_scheduled_frame_id_;
  last_scheduled_display_generation_                = scope_frame.display_generation;
  last_scheduled_frame_id_                          = scope_frame.frame_id;

  const auto                      analyzer          = analyzer_;
  const auto                      refresh_in_flight = refresh_in_flight_;
  QPointer<EditorScopeController> receiver(this);
  scope_pool_.start([analyzer, scope_frame, request, submit_new_frame, expected_image_identity,
                     expected_image_generation, expected_request_revision, refresh_in_flight,
                     receiver]() {
    alcedo::ScopeRenderSnapshot next_snapshot;
    try {
      // Collect the latest completed output for the current image first; the
      // frame submitted below completes on a later tick, so a tick that only
      // submits must not drop an already-finished result.
      const auto output = analyzer->GetLatestOutput();
      if (output.generation != 0) {
        next_snapshot = alcedo::ReadScopeRenderSnapshot(output);
      }
      if (submit_new_frame) {
        analyzer->SubmitFrame(scope_frame, request);
      }
    } catch (...) {
      next_snapshot = {};
    }

    if (!receiver) {
      refresh_in_flight->store(false);
      return;
    }
    const bool queued = QMetaObject::invokeMethod(
        receiver.data(),
        [receiver, next_snapshot = std::move(next_snapshot), expected_image_identity,
         expected_image_generation, expected_request_revision, refresh_in_flight]() mutable {
          refresh_in_flight->store(false);
          if (!receiver || next_snapshot.generation == 0) {
            return;
          }
          (void)receiver->publishSnapshot(std::move(next_snapshot), expected_image_identity,
                                          expected_image_generation, expected_request_revision);
        },
        Qt::QueuedConnection);
    if (!queued) {
      refresh_in_flight->store(false);
    }
  });
}

auto EditorScopeController::publishSnapshot(alcedo::ScopeRenderSnapshot next_snapshot,
                                            std::uint64_t               expected_image_identity,
                                            std::uint64_t               expected_image_generation,
                                            std::uint64_t expected_request_revision) -> bool {
  if (!visual_active_ || expected_request_revision != request_revision_ ||
      expected_image_identity != image_identity_ ||
      expected_image_generation != image_generation_) {
    return false;
  }
  if (next_snapshot.generation == 0 ||
      (next_snapshot.image_identity != 0 && next_snapshot.image_identity != image_identity_) ||
      (next_snapshot.image_generation != 0 &&
       next_snapshot.image_generation != image_generation_)) {
    return false;
  }
  if (!next_snapshot.histogram.valid && !next_snapshot.waveform.valid) {
    return false;
  }
  // Publish the latest completed analysis of the current image. During
  // continuous dragging the render display_generation keeps advancing; accept
  // a completed output whose display_generation may lag the newest frame
  // rather than dropping every result that isn't the very latest render.
  if (next_snapshot.generation == snapshot_.generation &&
      next_snapshot.image_identity == snapshot_.image_identity &&
      next_snapshot.image_generation == snapshot_.image_generation &&
      next_snapshot.display_generation == snapshot_.display_generation) {
    return false;
  }

  snapshot_ = std::move(next_snapshot);
  emit SnapshotChanged();
  return true;
}

void EditorScopeController::clearSnapshot() {
  snapshot_                          = {};
  last_scheduled_display_generation_ = 0;
  last_scheduled_frame_id_           = 0;
  emit SnapshotChanged();
}

}  // namespace alcedo::ui
