//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// Phase 5A / 5A-Fix: EditorSessionController with a fake IEditorSessionBackend,
/// runtime→controller state propagation, dependency scan of QML editor path,
/// and build-check that EditorSessionService does not link Qt Widgets.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"
#include "app/editor_session_types.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_history_models.hpp"
#include "ui/alcedo_main/album_backend/workspace_router.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"

namespace alcedo::ui {
namespace {

namespace fs = std::filesystem;

class FakeSessionBackend final : public IEditorSessionBackend {
 public:
  EditorSessionState    state_ = EditorSessionState::NoImage;
  EditorSessionIdentity identity_{};
  PresentationSinkId    sink_id_            = 0;
  int                   open_count          = 0;
  int                   switch_count        = 0;
  int                   close_count         = 0;
  int                   shutdown_count      = 0;
  bool                  last_close_persist  = true;
  int                   presentation_width  = 0;
  int                   presentation_height = 0;
  std::string           last_error_;

  auto                  state() const -> EditorSessionState override { return state_; }
  auto                  identity() const -> EditorSessionIdentity override { return identity_; }
  auto                  active() const -> bool override {
    return state_ != EditorSessionState::NoImage && state_ != EditorSessionState::ShuttingDown;
  }
  auto has_image() const -> bool override {
    return identity_.element_id > 0 && identity_.image_id > 0 && EditorSessionHasImage(state_);
  }
  auto last_error() const -> std::string override { return last_error_; }

  void SetPresentationSinkId(PresentationSinkId sink_id) override { sink_id_ = sink_id; }
  void SetPresentationSize(int width, int height) override {
    presentation_width  = width;
    presentation_height = height;
  }

  auto Open(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override {
    ++open_count;
    if (element_id == 0 || image_id == 0) {
      identity_ = {};
      state_    = EditorSessionState::NoImage;
    } else {
      ++identity_.session_generation;
      identity_.element_id        = element_id;
      identity_.image_id          = image_id;
      identity_.render_generation = identity_.session_generation;
      identity_.view_generation   = 1;
      state_                      = EditorSessionState::Loading;
    }
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::StateChanged;
    result.state    = state_;
    result.identity = identity_;
    NotifyChange();
    return result;
  }

  auto Switch(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override {
    ++switch_count;
    return Open(element_id, image_id);
  }

  auto Close(bool persist_changes) -> EditorSessionResult override {
    ++close_count;
    last_close_persist   = persist_changes;
    state_               = EditorSessionState::NoImage;
    identity_.element_id = 0;
    identity_.image_id   = 0;
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::StateChanged;
    result.state    = state_;
    result.identity = identity_;
    NotifyChange();
    return result;
  }

  auto Shutdown() -> EditorSessionResult override {
    ++shutdown_count;
    state_    = EditorSessionState::ShuttingDown;
    identity_ = {};
    EditorSessionResult result;
    result.kind  = EditorSessionResultKind::StateChanged;
    result.state = state_;
    NotifyChange();
    return result;
  }

  auto Discard() -> EditorSessionResult override {
    EditorSessionResult result;
    result.kind  = EditorSessionResultKind::Accepted;
    result.state = state_;
    return result;
  }

  auto                                Undo() -> EditorSessionResult override { return Discard(); }

  // Phase 6C-7: snapshot publication for panel state loading.
  EditorRenderAdjustmentSnapshot current_snapshot_;
  auto adjustment_snapshot() const -> EditorRenderAdjustmentSnapshot override {
    return current_snapshot_;
  }
  void SetAdjustmentSnapshot(EditorRenderAdjustmentSnapshot snapshot) {
    current_snapshot_ = std::move(snapshot);
  }

  auto                                Redo() -> EditorSessionResult override { return Discard(); }

  // Phase 5D: record view-change routing so tests can assert the controller
  // forwards intents (and only intents) to the backend without bypassing it.
  bool                                view_change_recorded = false;
  EditorRenderReason                  view_change_reason   = EditorRenderReason::ZoomPan;
  std::optional<ViewportRenderRegion> view_change_region{std::nullopt};
  bool                                render_busy_ = false;
  bool                                geometry_overlay_active = false;

  void SetGeometryOverlayActive(bool active) override { geometry_overlay_active = active; }

  auto RequestViewChange(EditorRenderReason reason, std::optional<ViewportRenderRegion> region)
      -> EditorSessionResult override {
    view_change_recorded = true;
    view_change_reason   = reason;
    view_change_region   = std::move(region);
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::RenderRouted;
    result.state    = state_;
    result.identity = identity_;
    return result;
  }
  [[nodiscard]] auto render_busy() const -> bool override { return render_busy_; }

  // Test helper: simulate async Interactive transition from first frame.
  void               SimulateFirstFramePresented() {
    state_ = EditorSessionState::Interactive;
    NotifyChange();
  }

  void NotifyWithoutStateChange() { NotifyChange(); }

  // Phase 6D multi-slider: Patch/Commit update the live snapshot and NotifyChange
  // the same way EditorSessionService::Emit does after HandlePatch.
  int patch_count  = 0;
  int commit_count = 0;

  auto Patch(EditorAdjustmentPatch patch) -> EditorSessionResult override {
    ++patch_count;
    UpsertSnapshotPatch(std::move(patch));
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::RenderRouted;
    result.state    = state_;
    result.identity = identity_;
    NotifyChange();
    return result;
  }

  auto CommitAdjustment(EditorAdjustmentPatch patch) -> EditorSessionResult override {
    ++commit_count;
    UpsertSnapshotPatch(std::move(patch));
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::RenderRouted;
    result.state    = state_;
    result.identity = identity_;
    NotifyChange();
    return result;
  }

  // Phase 7A R0: deterministic counters + async Version-operation modeling for
  // terminal-event evidence. Async completion is driven by an explicit test
  // call (CompletePendingVersionOp), never a wall-clock sleep and never a fake
  // that completes synchronously inside the Q_INVOKABLE call.
  int     history_snapshot_count_ = 0;
  bool    async_version_ops_      = false;
  int     create_root_count_       = 0;
  int     branch_count_             = 0;
  int     checkout_async_count_    = 0;
  QString last_action_;
  std::string pending_message_;
  bool        pending_success_ = false;
  bool        has_pending_     = false;

  auto history_snapshot() -> EditorHistorySnapshot override {
    ++history_snapshot_count_;
    return {};
  }

  auto MakeSaveStarted() const -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::SaveStarted;
    result.state    = state_;
    result.identity = identity_;
    result.message  = "Save started";
    return result;
  }

  auto CreateRootVersion(std::string display_name) -> EditorSessionResult override {
    if (!async_version_ops_) return IEditorSessionBackend::CreateRootVersion(std::move(display_name));
    ++create_root_count_;
    last_action_ = QStringLiteral("createRootVersion");
    return MakeSaveStarted();
  }

  auto BranchFromCommit(const commit_hash_t& commit_id, std::string display_name)
      -> EditorSessionResult override {
    if (!async_version_ops_) return IEditorSessionBackend::BranchFromCommit(commit_id, std::move(display_name));
    ++branch_count_;
    last_action_ = QStringLiteral("branchFromCommit");
    return MakeSaveStarted();
  }

  auto CheckoutVersion(const version_ref_id_t& version_id) -> EditorSessionResult override {
    if (!async_version_ops_) return IEditorSessionBackend::CheckoutVersion(version_id);
    ++checkout_async_count_;
    last_action_ = QStringLiteral("checkoutVersion");
    return MakeSaveStarted();
  }

  /// Deterministic async completion: records the terminal outcome and
  /// NotifyChange() the same way EditorSessionService::Emit does when the save
  /// worker finishes. The defect under test is that EditorSessionController does
  /// not install the service result observer, so this terminal outcome never
  /// reaches the QML boundary as a correlated HistoryOperationFinished.
  void CompletePendingVersionOp(bool success, std::string message) {
    pending_success_ = success;
    pending_message_ = std::move(message);
    has_pending_     = true;
    NotifyChange();
  }
 private:
  void UpsertSnapshotPatch(EditorAdjustmentPatch patch) {
    auto& patches = current_snapshot_.patches;
    const auto it =
        std::find_if(patches.begin(), patches.end(), [&](const EditorAdjustmentPatch& p) {
          return p.field_key == patch.field_key;
        });
    if (it == patches.end()) {
      patches.push_back(std::move(patch));
    } else {
      *it = std::move(patch);
    }
    ++current_snapshot_.snapshot_generation;
  }
};

TEST(EditorSessionControllerPhase5ATest, RoutesOpenThroughInjectedFakeBackend) {
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);
  int                     state_signals = 0;
  QObject::connect(&controller, &EditorSessionController::StateChanged, [&] { ++state_signals; });

  controller.Open(42, 84);
  EXPECT_EQ(backend.open_count, 1);
  EXPECT_TRUE(controller.active());
  EXPECT_TRUE(controller.has_image());
  EXPECT_EQ(controller.element_id(), 42u);
  EXPECT_EQ(controller.image_id(), 84u);
  EXPECT_EQ(controller.session_generation(), 1u);
  EXPECT_EQ(controller.session_state(), EditorSessionState::Loading);
  EXPECT_GE(state_signals, 1);

  controller.Open(100, 200);
  EXPECT_EQ(backend.switch_count, 1);
  EXPECT_EQ(controller.element_id(), 100u);
  EXPECT_EQ(controller.last_element_id(), 100u);

  controller.Close();
  EXPECT_FALSE(controller.has_image());
  EXPECT_EQ(controller.session_state(), EditorSessionState::NoImage);
  EXPECT_EQ(backend.close_count, 1);
  EXPECT_TRUE(backend.last_close_persist);
}

TEST(EditorSessionControllerPhase5ATest, WorkspaceSwitchesImagesWithoutClosingTheFirstSession) {
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);
  WorkspaceRouter         router(&controller);

  router.OpenEditor(1, 2);
  EXPECT_EQ(backend.open_count, 1);
  EXPECT_EQ(backend.switch_count, 0);
  EXPECT_EQ(backend.close_count, 0);

  router.OpenEditor(3, 4);
  EXPECT_EQ(backend.switch_count, 1);
  EXPECT_EQ(backend.close_count, 0);
  EXPECT_EQ(controller.element_id(), 3u);

  router.OpenLibrary();
  EXPECT_EQ(backend.close_count, 1);
  EXPECT_TRUE(backend.last_close_persist);
  EXPECT_EQ(router.workspace(), QStringLiteral("library"));
}

TEST(EditorSessionControllerPhase5ATest, FinalizeAndShutdownKeepLifecycleInTheBackend) {
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);

  controller.Open(1, 2);
  controller.Finalize(false);
  EXPECT_EQ(backend.close_count, 1);
  EXPECT_FALSE(backend.last_close_persist);

  controller.Open(3, 4);
  controller.Shutdown();
  EXPECT_EQ(backend.shutdown_count, 1);
  EXPECT_EQ(controller.session_state(), EditorSessionState::ShuttingDown);
}

TEST(EditorSessionControllerPhase5ATest, PresentationSizeIsForwardedToTheBackend) {
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);

  controller.updatePresentationTargetSize(1920, 1080);

  EXPECT_EQ(backend.presentation_width, 1920);
  EXPECT_EQ(backend.presentation_height, 1080);
}

TEST(EditorSessionControllerPhase5ATest, SubmitViewChangeRoutesThroughBackend) {
  // Phase 5D A4/D2: the controller only reports the new view and forwards a
  // typed ViewChange intent to the backend; it never bypasses the coordinator.
  // ViewChangeKind maps 1:1 to EditorRenderReason; only DetailRefresh carries a
  // region (resolved from the bound frame sink — nullopt when no viewport is
  // bound).
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);
  controller.Open(7, 8);  // backend -> Loading with an image identity
  ASSERT_TRUE(controller.has_image());

  using VCK = alcedo::editor_rhi::EditorInteractionController::ViewChangeKind;

  controller.submitViewChange(static_cast<int>(VCK::DetailRefresh));
  EXPECT_TRUE(backend.view_change_recorded);
  EXPECT_EQ(backend.view_change_reason, alcedo::EditorRenderReason::DetailRefresh);
  EXPECT_FALSE(backend.view_change_region.has_value());  // no bound sink

  controller.submitViewChange(static_cast<int>(VCK::CropRotate));
  EXPECT_EQ(backend.view_change_reason, alcedo::EditorRenderReason::CropRotate);
  EXPECT_FALSE(backend.view_change_region.has_value());

  controller.submitViewChange(static_cast<int>(VCK::Resize));
  EXPECT_EQ(backend.view_change_reason, alcedo::EditorRenderReason::Resize);

  controller.submitViewChange(static_cast<int>(VCK::ZoomPan));
  EXPECT_EQ(backend.view_change_reason, alcedo::EditorRenderReason::ZoomPan);
}

TEST(EditorSessionControllerPhase5ATest, BoundInteractionRoutesDetailRefreshWithoutQmlRelay) {
  // The production workspace binds this signal directly. This protects the
  // double-click/zoom route from loss while a QML workspace item is rebuilt.
  FakeSessionBackend                      backend;
  EditorSessionController                 controller(nullptr, &backend);
  editor_rhi::EditorInteractionController interaction;
  controller.Open(7, 8);
  controller.bindInteractionController(&interaction);

  interaction.setViewportMetrics(800, 600, 1.0);
  interaction.setImageSize(1600, 1200);
  interaction.setRenderReferenceSize(1600, 1200);
  interaction.handleWheel(400, 300, 120, 0, 0, Qt::ControlModifier, false);

  // Zoomed DetailRefresh is intentionally settled so continuous wheel/pan
  // ticks coalesce into one production request.
  QEventLoop settle_loop;
  QTimer::singleShot(150, &settle_loop, &QEventLoop::quit);
  settle_loop.exec();

  EXPECT_TRUE(backend.view_change_recorded);
  EXPECT_EQ(backend.view_change_reason, EditorRenderReason::DetailRefresh);
}

TEST(EditorSessionControllerPhase5ATest, SubmitViewChangeIsNoOpWithoutImage) {
  // Phase 5D: a backend with no open image must not route a view change. The
  // view state itself already updated in the interaction controller; the
  // viewport re-samples whatever frame it last received.
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);
  EXPECT_FALSE(controller.has_image());
  controller.submitViewChange(
      static_cast<int>(alcedo::editor_rhi::EditorInteractionController::ViewChangeKind::ZoomPan));
  EXPECT_FALSE(backend.view_change_recorded);
}

TEST(EditorSessionControllerPhase5ATest, GeometryPanelSelectionUsesSourceFramePreview) {
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);

  controller.Open(7, 8);
  EXPECT_FALSE(backend.geometry_overlay_active);

  // Panel selection while still Loading only flips the overlay flag; the
  // source-frame refresh waits for Interactive (see next assertion block).
  controller.set_active_adjustment_panel(QStringLiteral("geometry"));
  EXPECT_TRUE(backend.geometry_overlay_active);
  EXPECT_FALSE(backend.view_change_recorded);

  controller.set_active_adjustment_panel(QStringLiteral("tone"));
  EXPECT_FALSE(backend.geometry_overlay_active);
}

TEST(EditorSessionControllerPhase5ATest,
     GeometryPanelSelectionWhileInteractiveRequestsCropRotateRefresh) {
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);

  controller.Open(7, 8);
  backend.SimulateFirstFramePresented();
  ASSERT_EQ(backend.state(), EditorSessionState::Interactive);

  backend.view_change_recorded = false;
  controller.set_active_adjustment_panel(QStringLiteral("geometry"));
  EXPECT_TRUE(backend.geometry_overlay_active);
  EXPECT_TRUE(backend.view_change_recorded);
  EXPECT_EQ(backend.view_change_reason, EditorRenderReason::CropRotate);

  backend.view_change_recorded = false;
  controller.set_active_adjustment_panel(QStringLiteral("tone"));
  EXPECT_FALSE(backend.geometry_overlay_active);
  EXPECT_TRUE(backend.view_change_recorded);
  EXPECT_EQ(backend.view_change_reason, EditorRenderReason::CropRotate);
}

TEST(EditorSessionControllerPhase5ATest, RenderBusyReflectsBackendDiagnostics) {
  // Phase 5D D6: render_busy is a thin reflection of backend diagnostics; it
  // never exposes a pipeline task object to QML.
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);
  EXPECT_FALSE(controller.render_busy());
  backend.render_busy_ = true;
  EXPECT_TRUE(controller.render_busy());
}

TEST(EditorSessionControllerPhase5ATest, WorksWithoutBackendForShellOnlyTests) {
  EditorSessionController controller(static_cast<EditorController*>(nullptr));
  controller.Open(1, 2);
  EXPECT_TRUE(controller.has_image());
  EXPECT_EQ(controller.session_generation(), 1u);
  controller.Close();
  EXPECT_FALSE(controller.has_image());
}

TEST(EditorSessionControllerPhase5ATest, AsyncBackendChangeEmitsStateChangedToQml) {
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);
  int                     state_signals = 0;
  QObject::connect(&controller, &EditorSessionController::StateChanged, [&] { ++state_signals; });

  controller.Open(7, 8);
  const int after_open = state_signals;
  EXPECT_EQ(controller.session_state(), EditorSessionState::Loading);

  backend.SimulateFirstFramePresented();
  EXPECT_GT(state_signals, after_open);
  EXPECT_EQ(controller.session_state(), EditorSessionState::Interactive);
  EXPECT_EQ(controller.session_state_name(), QStringLiteral("Interactive"));
}

TEST(EditorSessionControllerPhase5ATest, WorkerNotificationIsDeliveredOnTheControllerThread) {
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);
  QThread*                delivered_thread = nullptr;
  int                     state_signals    = 0;
  QObject::connect(&controller, &EditorSessionController::StateChanged, [&] {
    delivered_thread = QThread::currentThread();
    ++state_signals;
  });

  std::thread worker([&] { backend.NotifyWithoutStateChange(); });
  worker.join();
  QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);

  EXPECT_EQ(state_signals, 1);
  EXPECT_EQ(delivered_thread, controller.thread());
}

TEST(EditorSessionControllerPhase5ATest, DestroyedControllerIsDetachedFromBackendNotifications) {
  FakeSessionBackend backend;
  {
    EditorSessionController controller(nullptr, &backend);
    controller.Open(1, 2);
  }

  backend.NotifyWithoutStateChange();
  SUCCEED();
}

TEST(EditorSessionControllerPhase5ATest, RuntimeCoordinatorPresentationUpdatesController) {
  auto runtime = alcedo::EditorSessionRuntime::Create();
  runtime->service->SetPresentationSinkId(1);
  runtime->service->SetPresentationSize(640, 480);
  EditorSessionController controller(nullptr, runtime->service.get());
  int                     state_signals = 0;
  QObject::connect(&controller, &EditorSessionController::StateChanged, [&] { ++state_signals; });

  controller.Open(3, 4);
  EXPECT_EQ(controller.session_state(), EditorSessionState::Loading);
  auto* bootstrap_scheduler =
      dynamic_cast<alcedo::EditorSessionBootstrapSchedulerPort*>(runtime->scheduler.get());
  ASSERT_NE(bootstrap_scheduler, nullptr);
  ASSERT_FALSE(bootstrap_scheduler->scheduled().empty());
  const auto request_id = bootstrap_scheduler->scheduled().front().request_id;

  runtime->service->NotifyImageAcquired(runtime->service->identity().session_generation, true);
  EXPECT_EQ(controller.session_state(), EditorSessionState::Loading);

  runtime->coordinator->NotifySchedulerCompleted(request_id, true);
  runtime->coordinator->NotifyFrameSubmitted(request_id);
  runtime->coordinator->NotifyFramePresented(request_id);

  EXPECT_EQ(controller.session_state(), EditorSessionState::Interactive);
  EXPECT_EQ(controller.session_state_name(), QStringLiteral("Interactive"));
  EXPECT_GT(state_signals, 0);
}

auto ReadFileText(const std::string& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

auto CollectSourceFiles(const fs::path& root, const std::vector<std::string>& extensions)
    -> std::vector<fs::path> {
  std::vector<fs::path> out;
  if (!fs::exists(root)) {
    return out;
  }
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto ext = entry.path().extension().string();
    for (const auto& want : extensions) {
      if (ext == want) {
        out.push_back(entry.path());
        break;
      }
    }
  }
  return out;
}

TEST(EditorSessionControllerPhase5ATest, QmlEditorPathDoesNotIncludePipelineScheduler) {
  // Phase 5A acceptance (scoped to the QML editor path + Phase 5A session/render
  // sources). The legacy QWidget editor still calls PipelineScheduler until
  // Phase 5D cutover; those files are explicit temporary exceptions.
  const fs::path              repo_root  = fs::path(ALCEDO_REPO_ROOT);
  const std::vector<fs::path> scan_roots = {
      repo_root / "alcedo_studio/src/ui/alcedo_main/album_backend",
      repo_root / "alcedo_studio/src/ui/editor_rhi",
      repo_root / "alcedo_studio/src/ui/edit_viewer",
      repo_root / "alcedo_studio/src/include/ui/alcedo_main/album_backend",
      repo_root / "alcedo_studio/src/include/ui/editor_rhi",
      repo_root / "alcedo_studio/src/include/ui/edit_viewer",
  };
  // Phase 5A session/render sources only (not entire app/, which owns export/
  // thumbnail schedulers outside the editor).
  const std::vector<fs::path> extra_files = {
      repo_root / "alcedo_studio/src/app/editor_session_service.cpp",
      repo_root / "alcedo_studio/src/app/editor_render_coordinator.cpp",
      repo_root / "alcedo_studio/src/include/app/editor_session_types.hpp",
      repo_root / "alcedo_studio/src/include/app/editor_adjustment_types.hpp",
      repo_root / "alcedo_studio/src/include/app/editor_render_intent.hpp",
      repo_root / "alcedo_studio/src/include/app/editor_session_ports.hpp",
      repo_root / "alcedo_studio/src/include/app/editor_session_service.hpp",
      repo_root / "alcedo_studio/src/include/app/editor_render_coordinator.hpp",
      repo_root / "alcedo_studio/src/include/app/editor_session_bootstrap.hpp",
  };
  // Temporary exceptions:
  // - legacy QWidget editor scheduler callers (deleted at Phase 10 hard cutover)
  // - Phase 5B/5E production IEditorPipelineSchedulerPort implementation (the only
  //   component allowed to call PipelineScheduler::ScheduleTask for the QML path)
  const std::vector<std::string> exception_substrings = {
      "editor_controller.cpp",
      "editor_controller.hpp",
      "editor_dialog/render/editor_render_coordinator",
      "editor_dialog\\render\\editor_render_coordinator",
      "editor_session_render_scheduler_port.cpp",
      "editor_session_render_scheduler_port.hpp",
  };

  const std::vector<std::string> forbidden = {
      "renderer/pipeline_scheduler.hpp",
      "ScheduleTask(",
  };

  auto should_scan = [&](const std::string& path_str) -> bool {
    for (const auto& ex : exception_substrings) {
      if (path_str.find(ex) != std::string::npos) {
        return false;
      }
    }
    return true;
  };

  int  scanned  = 0;
  auto scan_one = [&](const fs::path& path) {
    const std::string path_str = path.generic_string();
    if (!should_scan(path_str)) {
      return;
    }
    const std::string text = ReadFileText(path.string());
    ASSERT_FALSE(text.empty()) << "missing source: " << path_str;
    ++scanned;
    for (const auto& needle : forbidden) {
      EXPECT_EQ(text.find(needle), std::string::npos)
          << path_str << " must not reference " << needle
          << "; only alcedo::EditorRenderCoordinator may own scheduler calls "
             "(legacy QWidget path is excepted until Phase 10 hard cutover)";
    }
  };

  for (const auto& root : scan_roots) {
    for (const auto& path : CollectSourceFiles(root, {".cpp", ".hpp", ".h"})) {
      scan_one(path);
    }
  }
  for (const auto& path : extra_files) {
    scan_one(path);
  }
  EXPECT_GT(scanned, 10) << "expected to scan editor QML path sources";
}

TEST(EditorSessionControllerPhase5ATest, Phase5AAppHeadersDoNotIncludeUi) {
  const fs::path app_include = fs::path(ALCEDO_REPO_ROOT) / "alcedo_studio/src/include/app";
  const std::vector<std::string> phase5a_headers = {
      "editor_session_types.hpp",     "editor_adjustment_types.hpp",
      "editor_render_intent.hpp",     "editor_session_ports.hpp",
      "editor_session_service.hpp",   "editor_render_coordinator.hpp",
      "editor_session_bootstrap.hpp",
  };
  for (const auto& name : phase5a_headers) {
    const fs::path    path = app_include / name;
    const std::string text = ReadFileText(path.string());
    ASSERT_FALSE(text.empty()) << "missing " << path.string();
    EXPECT_EQ(text.find("ui/"), std::string::npos)
        << path.string() << " must not include ui/ headers (Phase 5A-Fix)";
    EXPECT_EQ(text.find("edit_viewer/frame_sink.hpp"), std::string::npos)
        << path.string() << " must not include frame_sink.hpp";
  }
}

TEST(EditorSessionControllerPhase5ATest, EditorSessionServiceCMakeDoesNotLinkQtWidgets) {
  const fs::path    repo_root = fs::path(ALCEDO_REPO_ROOT);
  const std::vector<fs::path> cmake_files = {
      repo_root / "alcedo_studio/src/app/CMakeLists.txt",
      repo_root / "alcedo_studio/src/CMakeLists.txt",
  };
  std::string text;
  for (const auto& cmake : cmake_files) {
    const std::string candidate = ReadFileText(cmake.string());
    if (candidate.find("def_library(EditorSessionService") != std::string::npos) {
      text = candidate;
      break;
    }
  }
  ASSERT_FALSE(text.empty());

  const auto start = text.find("def_library(EditorSessionService");
  ASSERT_NE(start, std::string::npos);
  const auto        next = text.find("\ndef_library(", start + 1);
  const std::string block =
      text.substr(start, next == std::string::npos ? std::string::npos : next - start);
  // Match real link dependency tokens, not comments that mention Widgets.
  // The facade may declare PRIVATE_DEPS on internal module libraries
  // (EditorSaveCheckpointService, EditorSessionLifecycle, etc.) as part of
  // Fix-1B; it must never link Qt Widgets or expose PUBLIC_DEPS.
  EXPECT_EQ(block.find("PUBLIC_DEPS"), std::string::npos)
      << "EditorSessionService must not declare PUBLIC_DEPS";
  EXPECT_EQ(block.find("Qt6::Widgets"), std::string::npos)
      << "EditorSessionService must not link Qt6::Widgets";
  EXPECT_EQ(block.find("Qt5::Widgets"), std::string::npos);
  EXPECT_NE(block.find("no Widgets"), std::string::npos);
}

// ── Phase 6C-7: adjustment snapshot publication ────────────────────────

TEST(EditorSessionControllerPhase5ATest, SnapshotRevisionStartsAtZero) {
  EditorSessionController controller(static_cast<EditorController*>(nullptr));
  EXPECT_EQ(controller.snapshot_revision(), 0u);
  EXPECT_TRUE(controller.adjustment_snapshot().isEmpty());
}

TEST(EditorSessionControllerPhase5ATest, BackendSnapshotIsPublishedToController) {
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);

  EditorRenderAdjustmentSnapshot snap;
  snap.patches = {
      EditorAdjustmentPatch{"exposure", R"({"exposure":1.5})", true},
      EditorAdjustmentPatch{"contrast", R"({"contrast":18.0})", true},
  };
  snap.snapshot_generation = 1;
  backend.SetAdjustmentSnapshot(snap);

  backend.NotifyWithoutStateChange();

  EXPECT_GE(controller.snapshot_revision(), 1u);
  const auto map = controller.adjustment_snapshot();
  EXPECT_TRUE(map.contains(QStringLiteral("exposure")));
  EXPECT_TRUE(map.contains(QStringLiteral("contrast")));
}

TEST(EditorSessionControllerPhase5ATest, SnapshotRevisionIncrementsOnChange) {
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);

  EditorRenderAdjustmentSnapshot snap1;
  snap1.patches = {EditorAdjustmentPatch{"exposure", R"({"exposure":0.0})", true}};
  backend.SetAdjustmentSnapshot(snap1);
  backend.NotifyWithoutStateChange();
  const auto rev1 = controller.snapshot_revision();
  EXPECT_GE(rev1, 1u);

  EditorRenderAdjustmentSnapshot snap2;
  snap2.patches = {EditorAdjustmentPatch{"exposure", R"({"exposure":1.5})", true}};
  backend.SetAdjustmentSnapshot(snap2);
  backend.NotifyWithoutStateChange();

  EXPECT_GT(controller.snapshot_revision(), rev1);
}

TEST(EditorSessionControllerPhase5ATest, SameSnapshotDoesNotIncrementRevision) {
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);

  EditorRenderAdjustmentSnapshot snap;
  snap.patches = {EditorAdjustmentPatch{"exposure", R"({"exposure":0.75})", true}};
  backend.SetAdjustmentSnapshot(snap);
  backend.NotifyWithoutStateChange();
  const auto rev1 = controller.snapshot_revision();

  backend.NotifyWithoutStateChange();
  EXPECT_EQ(controller.snapshot_revision(), rev1);
}

TEST(EditorSessionControllerPhase5ATest, NoBackendReturnsEmptySnapshot) {
  EditorSessionController controller(static_cast<EditorController*>(nullptr));
  controller.Open(1, 2);
  EXPECT_TRUE(controller.has_image());
  EXPECT_TRUE(controller.adjustment_snapshot().isEmpty());
  EXPECT_EQ(controller.snapshot_revision(), 0u);
}

TEST(EditorSessionControllerPhase5ATest, SnapshotSignalFiresOnChange) {
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);

  int signal_count = 0;
  QObject::connect(&controller, &EditorSessionController::AdjustmentSnapshotChanged,
                   [&] { ++signal_count; });

  EditorRenderAdjustmentSnapshot snap;
  snap.patches = {EditorAdjustmentPatch{"contrast", R"({"contrast":10.0})", true}};
  backend.SetAdjustmentSnapshot(snap);
  backend.NotifyWithoutStateChange();
  EXPECT_GE(signal_count, 1);
}

TEST(EditorSessionControllerPhase5ATest,
     InteractiveSubmitPatchDoesNotEmitAdjustmentSnapshotChanged) {
  // Repro: multi-slider handoff freezes when every interactive submitPatch
  // emits AdjustmentSnapshotChanged → QML loadFromSnapshot on the GUI stack.
  FakeSessionBackend backend;
  backend.state_ = EditorSessionState::Interactive;
  backend.identity_.element_id        = 1;
  backend.identity_.image_id          = 2;
  backend.identity_.session_generation = 1;
  EditorSessionController controller(nullptr, &backend);

  int snapshot_signals = 0;
  int state_signals    = 0;
  QObject::connect(&controller, &EditorSessionController::AdjustmentSnapshotChanged,
                   [&] { ++snapshot_signals; });
  QObject::connect(&controller, &EditorSessionController::StateChanged, [&] { ++state_signals; });

  // Rapid interactive moves across two fields (saturation then vibrance).
  for (int i = 1; i <= 8; ++i) {
    ASSERT_TRUE(controller.submitPatch(
        QStringLiteral("saturation"),
        QStringLiteral("{\"saturation\":%1}").arg(i * 5), false));
  }
  for (int i = 1; i <= 8; ++i) {
    ASSERT_TRUE(controller.submitPatch(
        QStringLiteral("vibrance"),
        QStringLiteral("{\"vibrance\":%1}").arg(i * 3), false));
  }

  EXPECT_EQ(backend.patch_count, 16);
  EXPECT_EQ(snapshot_signals, 0)
      << "interactive submitPatch must not flood AdjustmentSnapshotChanged "
         "(each emit re-enters QML loadFromSnapshot during pointer moves)";
  EXPECT_GE(state_signals, 1) << "StateChanged must still fire for renderBusy bindings";
  // Cache stays warm so a later external NotifyChange does not look brand-new
  // unless params actually change again.
  EXPECT_GE(controller.snapshot_revision(), 1u);
  EXPECT_TRUE(controller.adjustment_snapshot().contains(QStringLiteral("saturation")));
  EXPECT_TRUE(controller.adjustment_snapshot().contains(QStringLiteral("vibrance")));
}

TEST(EditorSessionControllerPhase5ATest, SettledSubmitPatchEmitsAdjustmentSnapshotChanged) {
  FakeSessionBackend backend;
  backend.state_ = EditorSessionState::Interactive;
  backend.identity_.element_id         = 1;
  backend.identity_.image_id           = 2;
  backend.identity_.session_generation = 1;
  EditorSessionController controller(nullptr, &backend);

  int snapshot_signals = 0;
  QObject::connect(&controller, &EditorSessionController::AdjustmentSnapshotChanged,
                   [&] { ++snapshot_signals; });

  // Interactive first (no signal), then settled (must signal for panel sync).
  ASSERT_TRUE(controller.submitPatch(QStringLiteral("exposure"),
                                     QStringLiteral("{\"exposure\":0.5}"), false));
  EXPECT_EQ(snapshot_signals, 0);

  ASSERT_TRUE(controller.submitPatch(QStringLiteral("exposure"),
                                     QStringLiteral("{\"exposure\":0.8}"), true));
  EXPECT_GE(snapshot_signals, 1)
      << "settled submitPatch must publish AdjustmentSnapshotChanged";
  EXPECT_EQ(backend.commit_count, 1);
}

TEST(EditorSessionControllerPhase5ATest, SnapshotSignalDoesNotRetriggerOnSameNotify) {
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);

  int signal_count = 0;
  QObject::connect(&controller, &EditorSessionController::AdjustmentSnapshotChanged,
                   [&] { ++signal_count; });

  EditorRenderAdjustmentSnapshot snap;
  snap.patches = {EditorAdjustmentPatch{"contrast", R"({"contrast":10.0})", true}};
  backend.SetAdjustmentSnapshot(snap);
  backend.NotifyWithoutStateChange();
  EXPECT_GE(signal_count, 1);

  const auto before = signal_count;
  backend.NotifyWithoutStateChange();
  EXPECT_EQ(signal_count, before);

  snap.patches = {EditorAdjustmentPatch{"contrast", R"({"contrast":42.0})", true}};
  backend.SetAdjustmentSnapshot(snap);
  backend.NotifyWithoutStateChange();
  EXPECT_GT(signal_count, before);
}

TEST(EditorSessionControllerPhase5ATest, SnapshotIncludesParsedJsonValues) {
  FakeSessionBackend      backend;
  EditorSessionController controller(nullptr, &backend);

  EditorRenderAdjustmentSnapshot snap;
  snap.patches = {EditorAdjustmentPatch{"exposure", R"({"exposure":-1.75})", true}};
  backend.SetAdjustmentSnapshot(snap);
  backend.NotifyWithoutStateChange();

  const auto map = controller.adjustment_snapshot();
  ASSERT_TRUE(map.contains(QStringLiteral("exposure")));
  const auto entry = map[QStringLiteral("exposure")].toMap();
  ASSERT_TRUE(entry.contains(QStringLiteral("exposure")));
  EXPECT_DOUBLE_EQ(entry[QStringLiteral("exposure")].toDouble(), -1.75);
}


// ---------------------------------------------------------------------------
// Phase 7A R0: failing evidence for the history-notification amplification,
// lost async terminal results, and swallowed validation errors. Each test
// asserts the repaired target behavior and fails against the current defect.
// Counters are deterministic (lambda signal counters + FakeSessionBackend
// counters); no wall-clock sleeps; async completion is driven by an explicit
// test call (CompletePendingVersionOp), never a synchronous fake completion.
// ---------------------------------------------------------------------------

TEST(EditorSessionControllerPhase5ATest,
     InteractivePreviewDoesNotPublishHistoryRevisionOrReadHistorySnapshot) {
  FakeSessionBackend backend;
  backend.state_    = EditorSessionState::Interactive;
  backend.identity_ = {42, 84, 1, 1, 1};
  EditorSessionController controller(nullptr, &backend);
  EditorHistoryModel       model;
  model.setEditorSession(&controller);

  int history_signals = 0;
  int model_refreshes  = 0;
  QObject::connect(&controller, &EditorSessionController::HistoryChanged, [&] { ++history_signals; });
  QObject::connect(&model, &EditorHistoryModel::StateChanged, [&] { ++model_refreshes; });
  backend.history_snapshot_count_ = 0;

  ASSERT_TRUE(controller.submitPatch(QStringLiteral("exposure"),
                                      QStringLiteral(R"({"exposure":0.5})"), false));

  EXPECT_EQ(history_signals, 0) << "interactive preview must not emit HistoryChanged";
  EXPECT_EQ(model_refreshes, 0) << "interactive preview must not refresh the history model";
  EXPECT_EQ(backend.history_snapshot_count_, 0)
      << "interactive preview must not read a history snapshot";
}

TEST(EditorSessionControllerPhase5ATest,
     SettledCommitPublishesOneHistoryRevisionAndOneProjection) {
  FakeSessionBackend backend;
  backend.state_    = EditorSessionState::Interactive;
  backend.identity_ = {42, 84, 1, 1, 1};
  EditorSessionController controller(nullptr, &backend);
  EditorHistoryModel       model;
  model.setEditorSession(&controller);

  int history_signals = 0;
  int model_refreshes  = 0;
  QObject::connect(&controller, &EditorSessionController::HistoryChanged, [&] { ++history_signals; });
  QObject::connect(&model, &EditorHistoryModel::StateChanged, [&] { ++model_refreshes; });
  backend.history_snapshot_count_ = 0;

  ASSERT_TRUE(controller.submitPatch(QStringLiteral("exposure"),
                                      QStringLiteral(R"({"exposure":0.8})"), true));

  EXPECT_EQ(history_signals, 1) << "one settled commit publishes one history revision";
  EXPECT_EQ(model_refreshes, 1) << "one settled commit performs one projection";
  EXPECT_EQ(backend.history_snapshot_count_, 1)
      << "one settled commit reads one history snapshot";
}

TEST(EditorSessionControllerPhase5ATest, RenderBusyAndFrameCompletionDoNotRefreshHistoryModels) {
  FakeSessionBackend backend;
  backend.state_    = EditorSessionState::Interactive;
  backend.identity_ = {42, 84, 1, 1, 1};
  EditorSessionController controller(nullptr, &backend);
  EditorHistoryModel       model;
  model.setEditorSession(&controller);

  int history_signals = 0;
  int model_refreshes  = 0;
  QObject::connect(&controller, &EditorSessionController::HistoryChanged, [&] { ++history_signals; });
  QObject::connect(&model, &EditorHistoryModel::StateChanged, [&] { ++model_refreshes; });

  // Settle the session to Interactive (first-frame presented) before measuring.
  backend.SimulateFirstFramePresented();
  history_signals = 0;
  model_refreshes = 0;
  backend.history_snapshot_count_ = 0;

  // A render-busy toggle is a renderer notification, not a history mutation.
  backend.render_busy_ = true;
  backend.NotifyWithoutStateChange();

  EXPECT_EQ(history_signals, 0) << "render-busy must not emit HistoryChanged";
  EXPECT_EQ(model_refreshes, 0) << "render-busy must not refresh the history model";
  EXPECT_EQ(backend.history_snapshot_count_, 0)
      << "render-busy must not read a history snapshot";
}

TEST(EditorSessionControllerPhase5ATest, AsyncRootVersionCompletionClosesMatchingDraft) {
  FakeSessionBackend backend;
  backend.state_            = EditorSessionState::Interactive;
  backend.identity_         = {42, 84, 1, 1, 1};
  backend.async_version_ops_ = true;
  EditorSessionController controller(nullptr, &backend);

  int finished = 0;
  QObject::connect(&controller, &EditorSessionController::HistoryOperationFinished,
                   [&] { ++finished; });

  controller.CreateRootVersion(QStringLiteral("New Look"));
  ASSERT_EQ(finished, 1) << "the immediate SaveStarted result must publish one event";
  EXPECT_EQ(controller.last_history_result().value("action").toString(),
            QStringLiteral("createRootVersion"));
  EXPECT_EQ(controller.last_history_result().value("kind").toInt(),
            static_cast<int>(EditorSessionResultKind::SaveStarted));

  // Deterministic async completion of the save checkpoint. The QML inline draft
  // closes on the terminal HistoryOperationFinished for this operation id; this
  // controller-level assertion proves that terminal event must arrive (R3 repair).
  backend.CompletePendingVersionOp(true, "Editor session materialized");

  EXPECT_EQ(finished, 2) << "async completion must publish a terminal event";
  EXPECT_EQ(controller.last_history_result().value("kind").toInt(),
            static_cast<int>(EditorSessionResultKind::SaveFinished))
      << "async completion must publish a terminal SaveFinished, not SaveStarted";
  EXPECT_EQ(controller.last_history_message().toStdString(), "Editor session materialized")
      << "the terminal event must carry the exact backend message";
}

TEST(EditorSessionControllerPhase5ATest, AsyncRootVersionFailureShowsExactBackendMessage) {
  FakeSessionBackend backend;
  backend.state_            = EditorSessionState::Interactive;
  backend.identity_         = {42, 84, 1, 1, 1};
  backend.async_version_ops_ = true;
  EditorSessionController controller(nullptr, &backend);

  int finished = 0;
  QObject::connect(&controller, &EditorSessionController::HistoryOperationFinished,
                   [&] { ++finished; });

  controller.CreateRootVersion(QStringLiteral("Broken Root"));
  ASSERT_EQ(finished, 1);

  backend.CompletePendingVersionOp(false, "root Version creation failed: disk full");

  EXPECT_EQ(finished, 2) << "async failure must publish a terminal event";
  EXPECT_TRUE(controller.last_history_failed())
      << "async failure must mark the result failed at the QML boundary";
  EXPECT_EQ(controller.last_history_message().toStdString(),
            "root Version creation failed: disk full")
      << "the terminal event must carry the exact backend failure message";
}

TEST(EditorSessionControllerPhase5ATest,
     AsyncBranchFailureKeepsSelectedCommitAndShowsExactBackendMessage) {
  FakeSessionBackend backend;
  backend.state_            = EditorSessionState::Interactive;
  backend.identity_         = {42, 84, 1, 1, 1};
  backend.async_version_ops_ = true;
  EditorSessionController controller(nullptr, &backend);
  const QString selected_commit = QStringLiteral("abcdef0123456789fedcba9876543210");

  int finished = 0;
  QObject::connect(&controller, &EditorSessionController::HistoryOperationFinished,
                   [&] { ++finished; });

  controller.BranchFromCommit(selected_commit, QStringLiteral("Branch Look"));
  ASSERT_EQ(finished, 1);
  EXPECT_EQ(backend.branch_count_, 1);
  EXPECT_EQ(controller.element_id(), 84u);

  backend.CompletePendingVersionOp(false, "branch creation failed: selected commit missing");

  EXPECT_EQ(finished, 2) << "async branch failure must publish a terminal event";
  EXPECT_EQ(controller.last_history_result().value("action").toString(),
            QStringLiteral("branchFromCommit"));
  EXPECT_TRUE(controller.last_history_failed());
  EXPECT_EQ(controller.last_history_message().toStdString(),
            "branch creation failed: selected commit missing")
      << "the terminal event must carry the exact backend failure message";
  // The selected commit must survive the failed branch (no live mutation
  // before the durable candidate succeeds). The QML-level selectedCommitId
  // preservation is asserted in R5 alongside the inline draft flow.
  EXPECT_EQ(controller.element_id(), 84u);
}

TEST(EditorSessionControllerPhase5ATest,
     InvalidVersionOrCommitIdPublishesRejectedTerminalResult) {
  FakeSessionBackend backend;
  backend.state_    = EditorSessionState::Interactive;
  backend.identity_ = {42, 84, 1, 1, 1};
  EditorSessionController controller(nullptr, &backend);

  int finished = 0;
  QObject::connect(&controller, &EditorSessionController::HistoryOperationFinished,
                   [&] { ++finished; });

  controller.CheckoutVersion(QStringLiteral("not-a-hex-version-id"));
  EXPECT_EQ(finished, 1)
      << "an invalid Version id must publish a terminal rejected result, not fail silently";
  EXPECT_TRUE(controller.last_history_failed());
  EXPECT_EQ(controller.last_history_result().value("action").toString(),
            QStringLiteral("checkoutVersion"));
  EXPECT_FALSE(controller.last_history_message().isEmpty());

  controller.MoveHeadToCommit(QStringLiteral("zzz"));
  EXPECT_EQ(finished, 2)
      << "an invalid commit id must publish a terminal rejected result, not fail silently";
  EXPECT_EQ(controller.last_history_result().value("action").toString(),
            QStringLiteral("moveHeadToCommit"));
}
}  // namespace
}  // namespace alcedo::ui
