//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Qt Quick Test: multi-slider user operations through the production
// session path (not a UI-only fake).
//
// Drag sat → immediately drag vib → models call EditorSessionController::submitPatch
// → backend Patch/Commit → real EditorSessionHistoryPort Capture/Commit against a
// live CPUPipelineExecutor. A concurrent "render" worker holds GetRenderLock and
// BlockingQueued to the GUI (present handshake shape). If Capture/Commit block on
// that lock while the worker waits for the GUI, the handoff freezes — the hang
// users report when switching sliders quickly during a busy render.
//
// Build (win_release):
//   cmd /c scripts\msvc_env.cmd --build --preset win_release --parallel 4 --target EditorMultiSliderQuickTest
// Run:
//   build\release\...\EditorMultiSliderQuickTest.exe -o -,txt -v1

#include <QtQuickTest>
#include <QApplication>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QtMath>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "app/editor_session_service.hpp"
#include "app/editor_session_types.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::quicktest {
namespace {

auto SrcQmlDir() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

auto MakeGuard(sl_element_id_t element_id) -> std::shared_ptr<alcedo::PipelineGuard> {
  auto guard       = std::make_shared<alcedo::PipelineGuard>();
  guard->id_       = element_id;
  guard->pipeline_ = std::make_shared<alcedo::CPUPipelineExecutor>();
  guard->commit_graph_ =
      std::make_shared<alcedo::CommitGraph>(alcedo::CommitGraph::CreateEmpty(element_id));
  guard->root_id_                  = guard->commit_graph_->GetRootId();
  return guard;
}

/// Production-shaped session backend: real Mini-Git history + pipeline guard.
/// Patch/Commit call history Capture/Commit (GUI-thread, same as service).
/// After each patch, a render worker holds GetRenderLock and may BlockingQueued
/// to the GUI — the cross-thread shape of Apply + present.
class ProductionSessionBackend final : public alcedo::IEditorSessionBackend {
 public:
  explicit ProductionSessionBackend(QObject* gui_anchor) : gui_anchor_(gui_anchor) {
    RegisterAllOperators();
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    journal_path_ = std::filesystem::temp_directory_path() / ("qml_multi_slider_" + stamp + ".wal");
    guard_        = MakeGuard(42);
    pipeline_port_ = std::make_shared<EditorSessionPipelinePort>();
    pipeline_port_->SetServices(EditorSessionPipelineServices{
        {}, [g = guard_](sl_element_id_t) { return g; }});
    history_.SetServices(EditorSessionHistoryPort::Services{
        [this](sl_element_id_t) { return journal_path_; }});
    history_.SetPipelinePort(pipeline_port_);

    std::string error;
    handle_ = history_.Acquire(42, &error);
    if (!handle_.valid) {
      last_error_ = error;
    }

    identity_.element_id   = 42;
    identity_.image_id     = 84;
    image_load_request_    = alcedo::ImageLoadRequestId{1};
    state_                 = alcedo::EditorSessionState::Interactive;
  }

  ~ProductionSessionBackend() override {
    StopRenderWorker();
    if (handle_.valid) {
      history_.Release(handle_);
    }
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);
  }

  auto state() const -> alcedo::EditorSessionState override { return state_; }
  auto identity() const -> alcedo::EditorSessionIdentity override { return identity_; }
  [[nodiscard]] auto active_image_load_request() const -> alcedo::ImageLoadRequestId override {
    return image_load_request_;
  }
  auto active() const -> bool override { return true; }
  auto has_image() const -> bool override { return true; }
  auto last_error() const -> std::string override { return last_error_; }
  auto adjustment_snapshot() const -> alcedo::EditorRenderAdjustmentSnapshot override {
    alcedo::EditorRenderAdjustmentSnapshot snap;
    std::string                            error;
    if (handle_.valid) {
      // ReadAdjustmentSnapshot is non-const on the port; snapshot is still
      // logically const for the backend API.
      const_cast<EditorSessionHistoryPort&>(history_).ReadAdjustmentSnapshot(handle_, &snap,
                                                                              &error);
    }
    return snap;
  }
  auto render_busy() const -> bool override { return render_busy_.load(); }

  void SetPresentationSinkId(alcedo::PresentationSinkId) override {}
  void SetPresentationSize(int, int) override {}

  auto Open(sl_element_id_t, image_id_t) -> alcedo::EditorSessionResult override {
    return Accept(alcedo::EditorSessionResultKind::StateChanged);
  }
  auto Switch(sl_element_id_t e, image_id_t i) -> alcedo::EditorSessionResult override {
    return Open(e, i);
  }
  auto Close(bool) -> alcedo::EditorSessionResult override {
    return Accept(alcedo::EditorSessionResultKind::StateChanged);
  }
  auto Shutdown() -> alcedo::EditorSessionResult override {
    state_ = alcedo::EditorSessionState::ShuttingDown;
    return Accept(alcedo::EditorSessionResultKind::StateChanged);
  }
  auto Discard() -> alcedo::EditorSessionResult override {
    return Accept(alcedo::EditorSessionResultKind::Accepted);
  }
  auto Undo() -> alcedo::EditorSessionResult override {
    return Accept(alcedo::EditorSessionResultKind::Accepted);
  }
  auto Redo() -> alcedo::EditorSessionResult override {
    return Accept(alcedo::EditorSessionResultKind::Accepted);
  }

  auto Patch(alcedo::EditorAdjustmentPatch patch) -> alcedo::EditorSessionResult override {
    return ApplyPatch(std::move(patch), /*settled=*/false);
  }

  auto CommitAdjustment(alcedo::EditorAdjustmentPatch patch) -> alcedo::EditorSessionResult override {
    return ApplyPatch(std::move(patch), /*settled=*/true);
  }

  // --- stats for QML ---
  int  patch_count() const { return patch_count_; }
  int  commit_count() const { return commit_count_; }
  int  history_fail_count() const { return history_fail_count_; }
  int  max_history_ms() const { return max_history_ms_; }
  bool history_ok() const { return handle_.valid && last_error_.empty(); }

  auto pipeline_guard() const -> std::shared_ptr<alcedo::PipelineGuard> { return guard_; }

  void StartContendedRenderLoop() {
    StopRenderWorker();
    stop_render_       = false;
    continuous_render_ = true;
    render_busy_       = true;
    render_worker_     = std::thread([this] {
      while (!stop_render_.load()) {
        if (!guard_ || !guard_->pipeline_) {
          break;
        }
        std::unique_lock<std::mutex> held(guard_->pipeline_->GetRenderLock());
        // Simulate GPU Apply wall time while lock is held.
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        // Present handshake: worker needs GUI while still holding the lock.
        if (gui_anchor_ && !stop_render_.load()) {
          QMetaObject::invokeMethod(gui_anchor_, [] {}, Qt::BlockingQueuedConnection);
        }
        held.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      continuous_render_ = false;
      render_busy_       = oneshot_running_.load();
    });
  }

  void StopRenderWorker() {
    stop_render_       = true;
    continuous_render_ = false;
    stop_oneshot_      = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while ((render_worker_.joinable() || oneshot_worker_.joinable()) &&
           std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      if (render_worker_.joinable() && !continuous_render_.load() && !render_busy_.load()) {
        // still may need join if flag flipped late
      }
      if (!continuous_render_.load() && !oneshot_running_.load()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (render_worker_.joinable()) {
      if (!continuous_render_.load()) {
        render_worker_.join();
      } else {
        render_worker_.detach();
      }
    }
    if (oneshot_worker_.joinable()) {
      if (!oneshot_running_.load()) {
        oneshot_worker_.join();
      } else {
        oneshot_worker_.detach();
      }
    }
    render_busy_ = false;
  }

 private:
  auto Accept(alcedo::EditorSessionResultKind kind) -> alcedo::EditorSessionResult {
    alcedo::EditorSessionResult result;
    result.kind     = kind;
    result.state    = state_;
    result.identity = identity_;
    NotifyChange();
    return result;
  }

  auto ApplyPatch(alcedo::EditorAdjustmentPatch patch, bool settled) -> alcedo::EditorSessionResult {
    const auto t0 = std::chrono::steady_clock::now();
    if (!handle_.valid) {
      ++history_fail_count_;
      alcedo::EditorSessionResult result;
      result.kind    = alcedo::EditorSessionResultKind::Rejected;
      result.state   = state_;
      result.message = last_error_.empty() ? "history handle invalid" : last_error_;
      return result;
    }

    std::string error;
    // Same order as EditorSessionEditController::HandlePatch.
    if (!history_.CaptureAdjustmentBeforePreview(handle_, patch, &error)) {
      ++history_fail_count_;
      last_error_ = error;
      alcedo::EditorSessionResult result;
      result.kind    = alcedo::EditorSessionResultKind::Rejected;
      result.state   = state_;
      result.message = error;
      return result;
    }
    if (settled) {
      if (!history_.CommitAdjustment(handle_, patch, &error)) {
        ++history_fail_count_;
        last_error_ = error;
        alcedo::EditorSessionResult result;
        result.kind    = alcedo::EditorSessionResultKind::Rejected;
        result.state   = state_;
        result.message = error;
        return result;
      }
      ++commit_count_;
    } else {
      ++patch_count_;
    }

    const auto ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    max_history_ms_ = std::max(max_history_ms_, ms);

    // One-shot render frame when no continuous worker is running: hold lock
    // briefly then present to GUI. Tracked so destructor can join.
    if (!render_busy_.load() && !continuous_render_.load()) {
      LaunchOneShotRender();
    }

    alcedo::EditorSessionResult result;
    result.kind     = alcedo::EditorSessionResultKind::RenderRouted;
    result.state    = state_;
    result.identity = identity_;
    NotifyChange();
    return result;
  }

  void LaunchOneShotRender() {
    // Join any previous one-shot before starting another (keeps at most one).
    if (oneshot_worker_.joinable()) {
      stop_oneshot_ = true;
      // Unblock possible BlockingQueued by pumping.
      for (int i = 0; i < 50 && oneshot_running_.load(); ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      if (oneshot_worker_.joinable() && !oneshot_running_.load()) {
        oneshot_worker_.join();
      } else if (oneshot_worker_.joinable()) {
        oneshot_worker_.detach();
      }
    }
    stop_oneshot_    = false;
    oneshot_running_ = true;
    render_busy_     = true;
    oneshot_worker_  = std::thread([this] {
      if (guard_ && guard_->pipeline_ && !stop_oneshot_.load()) {
        std::unique_lock<std::mutex> held(guard_->pipeline_->GetRenderLock());
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (!stop_oneshot_.load() && gui_anchor_) {
          QMetaObject::invokeMethod(gui_anchor_, [] {}, Qt::BlockingQueuedConnection);
        }
      }
      oneshot_running_ = false;
      render_busy_     = continuous_render_.load();
    });
  }

  QObject*                                 gui_anchor_ = nullptr;
  std::filesystem::path                    journal_path_;
  std::shared_ptr<alcedo::PipelineGuard>   guard_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_port_;
  EditorSessionHistoryPort                 history_;
  alcedo::EditorHistoryGuardHandle         handle_{};
  alcedo::EditorSessionState               state_ = alcedo::EditorSessionState::NoImage;
  alcedo::EditorSessionIdentity            identity_{};
  alcedo::ImageLoadRequestId               image_load_request_{};
  std::string                              last_error_;

  int patch_count_        = 0;
  int commit_count_       = 0;
  int history_fail_count_ = 0;
  int max_history_ms_     = 0;

  std::thread       render_worker_;
  std::thread       oneshot_worker_;
  std::atomic<bool> stop_render_{true};
  std::atomic<bool> stop_oneshot_{true};
  std::atomic<bool> continuous_render_{false};
  std::atomic<bool> oneshot_running_{false};
  std::atomic<bool> render_busy_{false};
};

/// QML-facing probe: pointer helpers + stats. Models submit through the session
/// controller (production seam), not through this object.
class HangProbe : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool reenteredSubmit READ reenteredSubmit NOTIFY statsChanged)
  Q_PROPERTY(int maxSubmitMs READ maxSubmitMs NOTIFY statsChanged)
  Q_PROPERTY(int maxHistoryMs READ maxHistoryMs NOTIFY statsChanged)
  Q_PROPERTY(int submitCount READ submitCount NOTIFY statsChanged)
  Q_PROPERTY(int interactiveCount READ interactiveCount NOTIFY statsChanged)
  Q_PROPERTY(int settledCount READ settledCount NOTIFY statsChanged)
  Q_PROPERTY(int sequenceMs READ sequenceMs NOTIFY statsChanged)
  Q_PROPERTY(int historyFailCount READ historyFailCount NOTIFY statsChanged)
  Q_PROPERTY(bool contentionObserved READ contentionObserved NOTIFY statsChanged)
  Q_PROPERTY(bool productionPath READ productionPath NOTIFY statsChanged)
  Q_PROPERTY(int fuzzyOpsCompleted READ fuzzyOpsCompleted NOTIFY statsChanged)
  Q_PROPERTY(int fuzzyMaxOpMs READ fuzzyMaxOpMs NOTIFY statsChanged)
  Q_PROPERTY(QString fuzzyLastOp READ fuzzyLastOp NOTIFY statsChanged)
  Q_PROPERTY(int fuzzyHandleDrags READ fuzzyHandleDrags NOTIFY statsChanged)
  Q_PROPERTY(int fuzzyValueChangingDrags READ fuzzyValueChangingDrags NOTIFY statsChanged)
  Q_PROPERTY(int fuzzyTrackClicks READ fuzzyTrackClicks NOTIFY statsChanged)
  Q_PROPERTY(int fuzzyHandoffs READ fuzzyHandoffs NOTIFY statsChanged)
  Q_PROPERTY(int fuzzyDoubleClicks READ fuzzyDoubleClicks NOTIFY statsChanged)

 public:
  HangProbe(EditorSessionController* session, ProductionSessionBackend* backend,
            EditorAdjustmentValueModel* sat, EditorAdjustmentValueModel* vib,
            QObject* parent = nullptr)
      : QObject(parent), session_(session), backend_(backend), sat_(sat), vib_(vib) {}

  [[nodiscard]] auto reenteredSubmit() const -> bool { return reentered_; }
  [[nodiscard]] auto maxSubmitMs() const -> int { return max_submit_ms_; }
  [[nodiscard]] auto maxHistoryMs() const -> int {
    return backend_ ? backend_->max_history_ms() : 0;
  }
  [[nodiscard]] auto submitCount() const -> int {
    return backend_ ? (backend_->patch_count() + backend_->commit_count()) : 0;
  }
  [[nodiscard]] auto interactiveCount() const -> int {
    return backend_ ? backend_->patch_count() : 0;
  }
  [[nodiscard]] auto settledCount() const -> int {
    return backend_ ? backend_->commit_count() : 0;
  }
  [[nodiscard]] auto sequenceMs() const -> int { return sequence_ms_; }
  [[nodiscard]] auto historyFailCount() const -> int {
    return backend_ ? backend_->history_fail_count() : 0;
  }
  [[nodiscard]] auto contentionObserved() const -> bool { return contention_observed_; }
  [[nodiscard]] auto productionPath() const -> bool { return backend_ && backend_->history_ok(); }
  [[nodiscard]] auto fuzzyOpsCompleted() const -> int { return fuzzy_ops_completed_; }
  [[nodiscard]] auto fuzzyMaxOpMs() const -> int { return fuzzy_max_op_ms_; }
  [[nodiscard]] auto fuzzyLastOp() const -> QString { return fuzzy_last_op_; }
  [[nodiscard]] auto fuzzyHandleDrags() const -> int { return fuzzy_handle_drags_; }
  [[nodiscard]] auto fuzzyValueChangingDrags() const -> int {
    return fuzzy_value_changing_drags_;
  }
  [[nodiscard]] auto fuzzyTrackClicks() const -> int { return fuzzy_track_clicks_; }
  [[nodiscard]] auto fuzzyHandoffs() const -> int { return fuzzy_handoffs_; }
  [[nodiscard]] auto fuzzyDoubleClicks() const -> int { return fuzzy_double_clicks_; }

 public slots:
  void beginContendedRender() {
    if (backend_) {
      backend_->StartContendedRenderLoop();
    }
  }

  void endContendedRender() {
    if (backend_) {
      backend_->StopRenderWorker();
    }
  }

  void markSequenceStart() {
    sequence_ms_           = 0;
    max_submit_ms_         = 0;
    reentered_             = false;
    contention_observed_   = false;
    sequence_start_patches_ = backend_ ? backend_->patch_count() : 0;
    sequence_start_commits_ = backend_ ? backend_->commit_count() : 0;
    emit statsChanged();
  }

  void markSequenceEnd(int elapsedMs) {
    sequence_ms_ = elapsedMs;
    if (backend_) {
      max_submit_ms_ = backend_->max_history_ms();
      // If history spent a long time while render was busy, treat as contention.
      if (backend_->max_history_ms() >= 200) {
        contention_observed_ = true;
      }
    }
    emit statsChanged();
  }

  void refreshStats() { emit statsChanged(); }

  Q_INVOKABLE void clickItem(QQuickItem* item, qreal nx, qreal ny) {
    if (!item || !item->window()) {
      return;
    }
    auto* window = item->window();
    window->show();
    window->requestActivate();
    QTest::qWaitForWindowExposed(window);
    QTest::qWaitForWindowActive(window);
    const QPoint pos = MapToWindow(item, nx, ny);
    last_click_pos_  = pos;
    const auto t0    = std::chrono::steady_clock::now();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, pos);
    QCoreApplication::processEvents();
    QTest::qWait(20);
    NoteSubmitWall(t0);
  }

  Q_INVOKABLE void dragItem(QQuickItem* item, qreal fromNx, qreal toNx, int steps) {
    if (!item || !item->window() || steps < 1) {
      return;
    }
    auto* window = item->window();
    window->show();
    window->requestActivate();
    (void)QTest::qWaitForWindowExposed(window);
    (void)QTest::qWaitForWindowActive(window);
    // Prefer the real handle position for the matching model when possible.
    EditorAdjustmentValueModel* model = nullptr;
    if (item->objectName() == QLatin1String("adjustmentSliderHandle")) {
      // Ambiguous which slider; use fromNx as press (callers pass handle nx).
      model = nullptr;
    }
    Q_UNUSED(model);
    const QPoint start = MapToWindow(item, fromNx, 0.5);
    const QPoint end   = MapToWindow(item, toNx, 0.5);
    const auto   t0    = std::chrono::steady_clock::now();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    for (int i = 1; i <= steps; ++i) {
      const qreal  t = static_cast<qreal>(i) / static_cast<qreal>(steps);
      const QPoint p(start.x() + static_cast<int>((end.x() - start.x()) * t), start.y());
      SendMouseMove(window, p);
      QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
      QTest::qWait(4);
    }
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 4);
    QTest::qWait(10);
    NoteSubmitWall(t0);
  }

  /// Drag by pressing the real handle for `model`, then sliding to to_nx.
  Q_INVOKABLE bool dragHandleOf(QQuickItem* slider, QObject* model_obj, qreal toNx, int steps) {
    auto* model = qobject_cast<EditorAdjustmentValueModel*>(model_obj);
    if (!slider || !model) {
      return false;
    }
    auto* window = slider->window();
    if (!window) {
      return false;
    }
    window->show();
    window->requestActivate();
    (void)QTest::qWaitForWindowExposed(window);
    (void)QTest::qWaitForWindowActive(window);
    return DragHandleTo(slider, model, toNx, steps);
  }

  Q_INVOKABLE void doubleClickItem(QQuickItem* item, qreal nx, qreal ny) {
    if (!item || !item->window()) {
      return;
    }
    auto* window = item->window();
    window->show();
    window->requestActivate();
    QTest::qWaitForWindowExposed(window);
    QTest::qWaitForWindowActive(window);
    const QPoint pos = MapToWindow(item, nx, ny);
    last_click_pos_  = pos;
    const auto t0    = std::chrono::steady_clock::now();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, pos);
    QCoreApplication::processEvents();
    QTest::qWait(40);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, pos);
    QCoreApplication::processEvents();
    QTest::qWait(20);
    NoteSubmitWall(t0);
  }

  Q_INVOKABLE QPoint lastClickPos() const { return last_click_pos_; }

  Q_INVOKABLE int patchesThisSequence() const {
    if (!backend_) {
      return 0;
    }
    return (backend_->patch_count() - sequence_start_patches_) +
           (backend_->commit_count() - sequence_start_commits_);
  }

  /// High-volume user-op stress through real pointer delivery:
  ///   - handle drag (press ON handle → move with button held → release)
  ///   - multi-slider handoff (finish A, immediately drag B)
  ///   - track click far from handle (must not jump after handle-only change)
  ///   - double-click reset on the handle
  /// Optional contended GetRenderLock + BlockingQueued present shape.
  /// Returns false on event-loop death, single-op hang, or if handle drags
  /// failed to change values (simulation is inert).
  Q_INVOKABLE bool runFuzzyStress(QQuickItem* sat_slider, QQuickItem* vib_slider, int iterations,
                                  bool contended, int hang_threshold_ms = 4000) {
    fuzzy_ops_completed_         = 0;
    fuzzy_max_op_ms_             = 0;
    fuzzy_handle_drags_          = 0;
    fuzzy_value_changing_drags_  = 0;
    fuzzy_track_clicks_          = 0;
    fuzzy_handoffs_              = 0;
    fuzzy_double_clicks_         = 0;
    fuzzy_last_op_               = QStringLiteral("init");
    emit statsChanged();

    if (!sat_slider || !vib_slider || !sat_slider->window() || iterations < 1 || !sat_ || !vib_) {
      fuzzy_last_op_ = QStringLiteral("invalid-args");
      emit statsChanged();
      return false;
    }

    auto* window = sat_slider->window();
    window->show();
    window->requestActivate();
    (void)QTest::qWaitForWindowExposed(window);
    (void)QTest::qWaitForWindowActive(window);

    // Known baselines so every handle drag has a real distance to travel.
    sat_->setValue(0);
    vib_->setValue(0);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    if (contended && backend_) {
      backend_->StartContendedRenderLoop();
      QTest::qWait(80);
    }

    markSequenceStart();
    const auto sequence_t0 = std::chrono::steady_clock::now();

    // Deterministic mix; weights bias toward real handle motion + handoff.
    // 0,1 track click | 2,3 short handle drag | 4,5 handoff | 6 double-click |
    // 7 drag then double-click | 8 track burst | 9 click then handoff drag
    std::mt19937                             rng(0xA1CED0u ^ static_cast<uint32_t>(iterations));
    std::uniform_int_distribution<int>       op_dist(0, 9);
    std::uniform_real_distribution<qreal>    nx_dist(0.05, 0.95);
    std::uniform_int_distribution<int>       steps_dist(4, 12);
    std::uniform_int_distribution<int>       target_dist(0, 1);
    std::uniform_int_distribution<int>       side_dist(0, 1);  // low vs high end

    auto pump_alive = [window](int budget_ms) -> bool {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
      bool       flagged  = false;
      QMetaObject::invokeMethod(
          window, [&flagged]() { flagged = true; }, Qt::QueuedConnection);
      while (!flagged && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QTest::qWait(1);
      }
      return flagged;
    };

    for (int i = 0; i < iterations; ++i) {
      const int                    op      = op_dist(rng);
      const bool                   use_sat = target_dist(rng) == 0;
      QQuickItem*                  a_item  = use_sat ? sat_slider : vib_slider;
      QQuickItem*                  b_item  = use_sat ? vib_slider : sat_slider;
      EditorAdjustmentValueModel*  a_model = use_sat ? sat_ : vib_;
      EditorAdjustmentValueModel*  b_model = use_sat ? vib_ : sat_;
      const auto                   op0     = std::chrono::steady_clock::now();

      switch (op) {
        case 0:
        case 1: {
          // Far from handle (value mid ≈ 0.5): track press must not jump.
          fuzzy_last_op_ = QStringLiteral("trackClick");
          const qreal track_nx = side_dist(rng) == 0 ? 0.08 : 0.92;
          FastClick(a_item, track_nx, 0.5);
          ++fuzzy_track_clicks_;
          break;
        }
        case 2:
        case 3: {
          fuzzy_last_op_ = QStringLiteral("handleDrag");
          const qreal to_nx = side_dist(rng) == 0 ? nx_dist(rng) * 0.35 + 0.05
                                                  : nx_dist(rng) * 0.35 + 0.60;
          (void)DragHandleTo(a_item, a_model, to_nx, steps_dist(rng));
          break;
        }
        case 4:
        case 5: {
          // Classic hang pattern: finish drag A, immediately drag B.
          fuzzy_last_op_ = QStringLiteral("handoffDrag");
          const qreal a_to = 0.15 + nx_dist(rng) * 0.35;
          const qreal b_to = 0.55 + nx_dist(rng) * 0.35;
          (void)DragHandleTo(a_item, a_model, a_to, steps_dist(rng));
          (void)DragHandleTo(b_item, b_model, b_to, steps_dist(rng));
          ++fuzzy_handoffs_;
          break;
        }
        case 6: {
          fuzzy_last_op_ = QStringLiteral("doubleClickReset");
          // Seed non-default via real handle drag, then double-click the handle.
          (void)DragHandleTo(a_item, a_model, 0.78, 6);
          FastDoubleClick(a_item, HandleNx(a_item, a_model), 0.5);
          ++fuzzy_double_clicks_;
          break;
        }
        case 7: {
          fuzzy_last_op_ = QStringLiteral("dragThenDoubleClick");
          (void)DragHandleTo(a_item, a_model, 0.70, 5);
          FastDoubleClick(a_item, HandleNx(a_item, a_model), 0.5);
          ++fuzzy_double_clicks_;
          break;
        }
        case 8: {
          fuzzy_last_op_ = QStringLiteral("trackClickBurst");
          for (int k = 0; k < 5; ++k) {
            FastClick(a_item, side_dist(rng) == 0 ? 0.06 : 0.94, 0.5);
            ++fuzzy_track_clicks_;
          }
          break;
        }
        default: {
          // Track click A (no-op seek) then immediate real handle drag on B.
          fuzzy_last_op_ = QStringLiteral("trackClickThenHandoffDrag");
          FastClick(a_item, 0.92, 0.5);
          ++fuzzy_track_clicks_;
          (void)DragHandleTo(b_item, b_model, 0.18 + nx_dist(rng) * 0.6, steps_dist(rng));
          ++fuzzy_handoffs_;
          break;
        }
      }

      const int op_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - op0)
                                             .count());
      fuzzy_max_op_ms_ = std::max(fuzzy_max_op_ms_, op_ms);
      ++fuzzy_ops_completed_;

      if (op_ms >= hang_threshold_ms) {
        if (contended && backend_) {
          backend_->StopRenderWorker();
        }
        markSequenceEnd(static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - sequence_t0)
                .count()));
        fuzzy_last_op_ = QStringLiteral("opHang:%1@%2ms").arg(fuzzy_last_op_).arg(op_ms);
        emit statsChanged();
        return false;
      }

      if (i == 0 || ((i + 1) % 250) == 0) {
        if (!pump_alive(hang_threshold_ms)) {
          if (contended && backend_) {
            backend_->StopRenderWorker();
          }
          markSequenceEnd(static_cast<int>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - sequence_t0)
                  .count()));
          fuzzy_last_op_ = QStringLiteral("eventLoopDead@%1").arg(i + 1);
          emit statsChanged();
          return false;
        }
        emit statsChanged();
      }
    }

    if (contended && backend_) {
      backend_->StopRenderWorker();
    }
    markSequenceEnd(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - sequence_t0)
                                         .count()));

    // Inert simulation guard: enough handle drags must have moved values.
    // Expect roughly half of ops to involve a drag; require a solid fraction
    // of those to actually change the model.
    if (fuzzy_handle_drags_ < iterations / 5) {
      fuzzy_last_op_ = QStringLiteral("tooFewHandleDrags:%1").arg(fuzzy_handle_drags_);
      emit statsChanged();
      return false;
    }
    if (fuzzy_value_changing_drags_ < fuzzy_handle_drags_ / 3) {
      fuzzy_last_op_ = QStringLiteral("dragsDidNotChangeValue:changed=%1/attempted=%2")
                           .arg(fuzzy_value_changing_drags_)
                           .arg(fuzzy_handle_drags_);
      emit statsChanged();
      return false;
    }

    emit statsChanged();
    return pump_alive(hang_threshold_ms);
  }

 signals:
  void statsChanged();

 private:
  static constexpr qreal kHandleSizePx = 22.0;

  static auto MapToWindow(QQuickItem* item, qreal nx, qreal ny) -> QPoint {
    const QPointF local(item->width() * nx, item->height() * ny);
    if (item->window() && item->window()->contentItem()) {
      return item->mapToItem(item->window()->contentItem(), local).toPoint();
    }
    return item->mapToScene(local).toPoint();
  }

  /// Normalized X of the handle center for the model's current value.
  static auto HandleNx(QQuickItem* slider, EditorAdjustmentValueModel* model) -> qreal {
    if (!slider || !model) {
      return 0.5;
    }
    const qreal w = slider->width();
    if (w <= 1.0) {
      return 0.5;
    }
    const qreal from  = model->minimum();
    const qreal to    = model->maximum();
    const qreal range = to - from;
    const qreal pos   = range != 0.0 ? (model->value() - from) / range : 0.5;
    const qreal clamped_pos = qBound(0.0, pos, 1.0);
    // Matches AdjustmentSlider handle geometry: padding 0, handle width 22.
    return (clamped_pos * (w - kHandleSizePx) + kHandleSizePx * 0.5) / w;
  }

  void NoteSubmitWall(std::chrono::steady_clock::time_point t0) {
    const auto ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    max_submit_ms_ = std::max(max_submit_ms_, ms);
    emit statsChanged();
  }

  void FastClick(QQuickItem* item, qreal nx, qreal ny) {
    if (!item || !item->window()) {
      return;
    }
    auto*        window = item->window();
    const QPoint pos    = MapToWindow(item, nx, ny);
    last_click_pos_     = pos;
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, pos);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
  }

  void SendMouseMove(QWindow* window, const QPoint& pos) {
    // Explicit LeftButton buttons mask so QML MouseArea onPositionChanged fires.
    const QPointF local  = pos;
    const QPointF global = window->mapToGlobal(pos);
    QMouseEvent   move_ev(QEvent::MouseMove, local, global, Qt::NoButton, Qt::LeftButton,
                          Qt::NoModifier);
    QGuiApplication::sendEvent(window, &move_ev);
  }

  /// Press on the real handle (from current model value), drag to to_nx with
  /// button held, release. Counts toward fuzzy_handle_drags_ / value-changing.
  auto DragHandleTo(QQuickItem* slider, EditorAdjustmentValueModel* model, qreal to_nx,
                    int steps) -> bool {
    if (!slider || !model || !slider->window() || steps < 1) {
      return false;
    }
    auto* window = slider->window();
    to_nx        = qBound(0.02, to_nx, 0.98);

    // Guarantee travel distance: if handle is already near target, seed away first
    // (programmatic setValue, no submit) so the subsequent pointer drag is real.
    const qreal from_nx = HandleNx(slider, model);
    if (qAbs(from_nx - to_nx) < 0.12) {
      const qreal seed_nx = to_nx < 0.5 ? 0.88 : 0.12;
      const qreal seed_v =
          model->minimum() + seed_nx * (model->maximum() - model->minimum());
      model->setValue(seed_v);
      QCoreApplication::processEvents(QEventLoop::AllEvents, 4);
    }

    const double before_press = model->value();
    const qreal  press_nx     = HandleNx(slider, model);
    const QPoint start        = MapToWindow(slider, press_nx, 0.5);
    const QPoint end          = MapToWindow(slider, to_nx, 0.5);

    ++fuzzy_handle_drags_;
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 2);

    for (int i = 1; i <= steps; ++i) {
      const qreal  t = static_cast<qreal>(i) / static_cast<qreal>(steps);
      const QPoint p(start.x() + static_cast<int>((end.x() - start.x()) * t),
                     start.y() + static_cast<int>((end.y() - start.y()) * t));
      SendMouseMove(window, p);
      QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    }

    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 4);

    const double after = model->value();
    // Step is 1.0 for these models; require at least one step of real motion.
    if (qAbs(after - before_press) >= 0.5) {
      ++fuzzy_value_changing_drags_;
      return true;
    }
    return false;
  }

  void FastDoubleClick(QQuickItem* item, qreal nx, qreal ny) {
    if (!item || !item->window()) {
      return;
    }
    auto*        window = item->window();
    const QPoint pos    = MapToWindow(item, nx, ny);
    last_click_pos_     = pos;
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, pos);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    QTest::qWait(40);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, pos);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
  }

  EditorSessionController*      session_ = nullptr;
  ProductionSessionBackend*     backend_ = nullptr;
  EditorAdjustmentValueModel*   sat_     = nullptr;
  EditorAdjustmentValueModel*   vib_     = nullptr;
  bool reentered_                        = false;
  bool contention_observed_              = false;
  int  max_submit_ms_                    = 0;
  int  sequence_ms_                      = 0;
  int  sequence_start_patches_           = 0;
  int  sequence_start_commits_           = 0;
  int  fuzzy_ops_completed_              = 0;
  int  fuzzy_max_op_ms_                  = 0;
  int  fuzzy_handle_drags_               = 0;
  int  fuzzy_value_changing_drags_       = 0;
  int  fuzzy_track_clicks_               = 0;
  int  fuzzy_handoffs_                   = 0;
  int  fuzzy_double_clicks_              = 0;
  QString fuzzy_last_op_;
  QPoint  last_click_pos_;
};

class QuickTestSetup : public QObject {
  Q_OBJECT
 public:
  QuickTestSetup() {
    // Backend must outlive controller notifier; both owned by setup.
    backend_ = std::make_unique<ProductionSessionBackend>(this);
    session_ = new EditorSessionController(nullptr, backend_.get(), this);

    sat_ = new EditorAdjustmentValueModel(this);
    sat_->setFieldKey(QStringLiteral("saturation"));
    sat_->setLabel(QStringLiteral("Saturation"));
    sat_->setMinimum(-100);
    sat_->setMaximum(100);
    sat_->setDefaultValue(0);
    sat_->setStep(1);
    sat_->setPrecision(0);
    sat_->setValue(0);
    sat_->setSubmitter(session_);

    vib_ = new EditorAdjustmentValueModel(this);
    vib_->setFieldKey(QStringLiteral("vibrance"));
    vib_->setLabel(QStringLiteral("Vibrance"));
    vib_->setMinimum(-100);
    vib_->setMaximum(100);
    vib_->setDefaultValue(0);
    vib_->setStep(1);
    vib_->setPrecision(0);
    vib_->setValue(0);
    vib_->setSubmitter(session_);

    probe_ = new HangProbe(session_, backend_.get(), sat_, vib_, this);
  }

  ~QuickTestSetup() override {
    if (backend_) {
      backend_->StopRenderWorker();
    }
  }

 public slots:
  void applicationAvailable() { QQuickStyle::setStyle(QStringLiteral("Basic")); }

  void qmlEngineAvailable(QQmlEngine* engine) {
    engine->addImportPath(SrcQmlDir());
    engine->rootContext()->setContextProperty(
        QStringLiteral("appTheme"),
        QVariant::fromValue(static_cast<QObject*>(&AppTheme::Instance())));
    engine->rootContext()->setContextProperty(QStringLiteral("satModel"), sat_);
    engine->rootContext()->setContextProperty(QStringLiteral("vibModel"), vib_);
    engine->rootContext()->setContextProperty(QStringLiteral("hangProbe"), probe_);
    engine->rootContext()->setContextProperty(QStringLiteral("editorSession"), session_);
    engine->rootContext()->setContextProperty(
        QStringLiteral("sliderSourceUrl"),
        QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/AdjustmentSlider.qml")));
  }

 private:
  std::unique_ptr<ProductionSessionBackend> backend_;
  EditorSessionController*                  session_ = nullptr;
  EditorAdjustmentValueModel*               sat_     = nullptr;
  EditorAdjustmentValueModel*               vib_     = nullptr;
  HangProbe*                                probe_   = nullptr;
};

}  // namespace
}  // namespace alcedo::ui::quicktest

int main(int argc, char** argv) {
  qputenv("QML_DISABLE_DISK_CACHE", QByteArray("1"));
  QApplication app(argc, argv);
  QQuickStyle::setStyle(QStringLiteral("Basic"));
  alcedo::ui::quicktest::QuickTestSetup setup;
  return quick_test_main_with_setup(argc, argv, "EditorMultiSliderQuickTest",
                                    QUICK_TEST_SOURCE_DIR, &setup);
}

#include "editor_multi_slider_quicktest.moc"
