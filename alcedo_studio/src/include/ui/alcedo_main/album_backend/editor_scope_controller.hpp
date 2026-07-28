//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <cstdint>
#include <memory>

#include "edit/scope/final_display_frame_tap.hpp"

namespace alcedo::ui {

class EditorScopeItem;

namespace test {
class EditorScopeControllerTestPeer;
}  // namespace test

/// QML-facing scope presentation owner for the unified editor workspace.
///
/// The analyzer remains an edit-layer service. This object owns the polling
/// cadence, filters outputs against the current image/display identity, and
/// publishes copied render data to the scene-graph item. Hiding the panel
/// pauses submissions and polling without releasing analyzer resources.
class EditorScopeController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(
      bool visualActive READ visual_active WRITE set_visual_active NOTIFY VisualActiveChanged)
  Q_PROPERTY(int activeView READ active_view WRITE set_active_view NOTIFY ActiveViewChanged)
  Q_PROPERTY(qulonglong imageIdentity READ image_identity NOTIFY SnapshotChanged)
  Q_PROPERTY(qulonglong imageGeneration READ image_generation NOTIFY SnapshotChanged)
  Q_PROPERTY(qulonglong displayGeneration READ display_generation NOTIFY SnapshotChanged)
  Q_PROPERTY(qulonglong scopeGeneration READ scope_generation NOTIFY SnapshotChanged)
  Q_PROPERTY(bool hasSnapshot READ has_snapshot NOTIFY SnapshotChanged)

 public:
  explicit EditorScopeController(QObject* parent = nullptr);
  EditorScopeController(std::shared_ptr<alcedo::IScopeAnalyzer> analyzer,
                        QObject*                                parent = nullptr);
  ~EditorScopeController() override;

  [[nodiscard]] auto visual_active() const -> bool { return visual_active_; }
  void               set_visual_active(bool active);
  [[nodiscard]] auto active_view() const -> int { return active_view_; }
  void               set_active_view(int view);
  [[nodiscard]] auto image_identity() const -> qulonglong {
    return static_cast<qulonglong>(image_identity_);
  }
  [[nodiscard]] auto image_generation() const -> qulonglong {
    return static_cast<qulonglong>(image_generation_);
  }
  [[nodiscard]] auto display_generation() const -> qulonglong {
    return static_cast<qulonglong>(snapshot_.display_generation);
  }
  [[nodiscard]] auto scope_generation() const -> qulonglong {
    return static_cast<qulonglong>(snapshot_.generation);
  }
  [[nodiscard]] auto has_snapshot() const -> bool;

  void               SetDownstreamSink(alcedo::IFrameSink* sink);
  [[nodiscard]] auto frame_sink() const -> alcedo::IFrameSink*;
  void               SetImageIdentity(qulonglong image_identity, qulonglong image_generation);

  /// Return a copied snapshot for the QSG item; the analyzer output handles
  /// never cross into QML or remain borrowed by the scene graph.
  [[nodiscard]] auto snapshot() const -> alcedo::ScopeRenderSnapshot;

  /// Submit the current frame and poll once synchronously. The production timer
  /// uses the asynchronous path; tests and a newly exposed panel can request a
  /// deterministic refresh without waiting for the timer.
  Q_INVOKABLE bool   refreshSnapshot();

  /// Stop polling, cancel queued refresh tasks, and block until the
  /// in-flight scope analysis task finishes. The editor session must call
  /// this before releasing the render pipeline so a scope worker never
  /// touches a destroyed pipeline stream or scratch buffer.
  void               Shutdown();

 signals:
  void VisualActiveChanged();
  void ActiveViewChanged();
  void SnapshotChanged();
  void FrameRequested();

 private:
  void pollSnapshot();
  void scheduleSnapshotRefresh();
  auto refreshSnapshotNow() -> bool;
  auto publishSnapshot(alcedo::ScopeRenderSnapshot snapshot, std::uint64_t expected_image_identity,
                       std::uint64_t expected_image_generation,
                       std::uint64_t expected_request_revision) -> bool;
  [[nodiscard]] auto snapshot_view() const -> const alcedo::ScopeRenderSnapshot& {
    return snapshot_;
  }
  void clearSnapshot();

  friend class EditorScopeItem;
  friend class test::EditorScopeControllerTestPeer;

  std::shared_ptr<alcedo::IScopeAnalyzer>           analyzer_;
  std::unique_ptr<alcedo::FinalDisplayFrameTapSink> frame_tap_;
  alcedo::ScopeRequest                              request_{};
  QTimer                                            poll_timer_;
  QThreadPool                                       scope_pool_;
  alcedo::ScopeRenderSnapshot                       snapshot_{};
  std::uint64_t                                     image_identity_                    = 0;
  std::uint64_t                                     image_generation_                  = 0;
  int                                               active_view_                       = 0;
  bool                                              visual_active_                     = false;
  std::uint64_t                                     request_revision_                  = 0;
  std::uint64_t                                     last_scheduled_display_generation_ = 0;
  std::uint64_t                                     last_scheduled_frame_id_           = 0;
  std::shared_ptr<std::atomic_bool> refresh_in_flight_ = std::make_shared<std::atomic_bool>(false);
};

}  // namespace alcedo::ui
