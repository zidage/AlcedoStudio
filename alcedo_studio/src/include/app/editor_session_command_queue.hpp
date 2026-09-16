//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "app/editor_render_intent.hpp"
#include "app/editor_session_render_controller.hpp"
#include "app/editor_session_request_ids.hpp"
#include "app/editor_session_types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/pipeline_edit_batch.hpp"

namespace alcedo {

/// Stable identifier assigned when a user command enters the session queue.
struct EditorSessionOperationId {
  std::uint64_t   command_id  = 0;
  sl_element_id_t element_id  = 0;
  image_id_t      image_id    = 0;

  [[nodiscard]] auto valid() const -> bool { return command_id != 0; }
};

/// User mutations accepted by EditorSessionCommandQueue.
enum class EditorSessionCommandKind : std::uint8_t {
  OpenImage = 0,
  SelectImage,
  CloseEditor,
  Shutdown,
  PreviewAdjustment,
  CommitAdjustment,
  Undo,
  Redo,
  MoveHead,
  DiscardChanges,
  CheckoutVersion,
  CreateRootVersion,
  BranchVersion,
  RenameVersion,
  RemoveVersion,
  ApplyPaste,
  RetrySave,
  DiscardAndContinue,
  CancelPendingNavigation,
  RequestViewChange,
  SetPresentationTarget,
  SetPresentationSize,
  SetGeometryOverlay,
  SetAdjustmentProjectionNode,
  RenameColorGrade,
  EditNodeGraph,
  /// Mask Groups: insert one clean Color Grade at the top of the backbone.
  InsertColorGradeAtTop,
  /// Mask Groups: remove one Color Grade and bridge its backbone neighbors.
  RemoveColorGradeAndBridge,
  PersistCurrent,
};

/// Worker messages that are delivered back to the session owner.
enum class EditorSessionCompletionKind : std::uint8_t {
  ImageStateLoaded = 0,
  JournalCommitFinished,
  MaterializationFinished,
  RenderResult,
  ThumbnailRefreshFinished,
  PipelineSnapshotBuilt,
  WorkerRequestFailed,
  NavigationFinished,
  /// A history save checkpoint (Rename/Remove/Paste publication or a
  /// dirty-journal flush before a retained transfer) reached its terminal
  /// outcome. Carries success, session generation, last journal sequence, and
  /// the operation that started the checkpoint.
  SaveCheckpointFinished,
};

/// State of the session command admission gate.
enum class EditorSessionQueueState : std::uint8_t {
  Accepting = 0,
  ShuttingDown,
  Stopped,
};

/// Fully typed user command. The queue stamps `operation.command_id`
/// before reduction; all other fields are immutable command input.
struct EditorSessionCommand {
  EditorSessionOperationId                operation{};
  EditorSessionCommandKind                kind              = EditorSessionCommandKind::OpenImage;
  sl_element_id_t                         element_id        = 0;
  image_id_t                              image_id          = 0;
  EditorAdjustmentPatch                   patch{};
  EditorRenderReason                      view_reason = EditorRenderReason::ZoomPan;
  std::optional<ViewportRenderRegion>     view_region;
  version_ref_id_t                        version_id{};
  commit_hash_t                           commit_id{};
  AdjustmentTransferPackage               transfer_package{};
  std::string                             text;
  bool                                    persist_changes = true;
  /// Presentation binding for SetPresentationTarget / SetPresentationSize /
  /// SetGeometryOverlay. Unused by other command kinds.
  PresentationSinkId                      presentation_sink_id = 0;
  int                                     presentation_width   = 0;
  int                                     presentation_height  = 0;
  bool                                    geometry_overlay_active = false;
  NodeId                                  node_id;
  /// InsertColorGradeAtTop only: the backbone node the caller expects to
  /// follow Develop. Re-verified on the owner thread so a queued request can
  /// never insert behind a successor that changed after submission.
  NodeId                                  expected_successor_id;
  NodeGraphTopologyChange                 topology_change{};
};

/// Typed worker completion. Payload-specific values are kept as
/// immutable fields so the worker never needs a callback into session code.
struct EditorSessionCompletion {
  EditorSessionCompletionKind  kind = EditorSessionCompletionKind::WorkerRequestFailed;
  EditorSessionOperationId     operation{};
  std::uint64_t                request_id         = 0;
  std::uint64_t                task_id            = 0;
  ImageLoadRequestId           image_load_request{};
  sl_element_id_t              element_id         = 0;
  image_id_t                   image_id           = 0;
  bool                         success            = false;
  bool                         durable            = false;
  bool                         materialized       = false;
  std::optional<std::uint64_t> last_journal_sequence;
  EditorRenderResult           render_result{};
  EditorRenderEvent            render_event{};
  bool                         navigation_success = false;
  bool                         retained_image     = false;
  std::string                  message;
};

/// Delivery port for the thread that owns the editor session reducer.
class IEditorSessionCommandExecutor {
 public:
  virtual ~IEditorSessionCommandExecutor()                    = default;

  /// Post work to the owning thread. Implementations must never invoke `task`
  /// inline from this method.
  virtual void               Post(std::function<void()> task) = 0;

  /// Post work to the owning thread after `delay` has elapsed. Implementations
  /// that have no timer may park the task for explicit test-driven delivery
  /// (manual executor) or deliver it on their own schedule; they must still
  /// never invoke `task` inline from this method.
  virtual void               PostDelayed(std::function<void()>    task,
                                         std::chrono::nanoseconds delay) = 0;

  /// True when the caller is already running on the owning thread.
  [[nodiscard]] virtual auto IsOwnerThread() const -> bool    = 0;

  /// Stop accepting work and wait for in-flight tasks to finish. Executors
  /// with a dedicated worker thread must join it here; executors that only
  /// run when explicitly driven keep the default no-op. Idempotent.
  virtual void               Shutdown() {}
};

/// Deterministic executor used by unit and integration tests. Posted work runs
/// only when `DrainOne` or `DrainAll` is called from the owner thread. Delayed
/// work is parked separately: tests deliver it explicitly through
/// `DrainDelayedOne` / `DrainDelayedAll` after advancing their manual clock, so
/// a parked deadline never fires implicitly inside `DrainAll`.
class EditorSessionManualCommandExecutor final : public IEditorSessionCommandExecutor {
 public:
  EditorSessionManualCommandExecutor();

  void               Post(std::function<void()> task) override;
  void               PostDelayed(std::function<void()>    task,
                                 std::chrono::nanoseconds delay) override;
  [[nodiscard]] auto IsOwnerThread() const -> bool override;

  /// Run one posted task and return false when no task is pending.
  auto               DrainOne() -> bool;
  /// Run all currently posted work, including work posted by a drained task.
  void               DrainAll();
  /// Run one parked delayed task and return false when none is parked.
  auto               DrainDelayedOne() -> bool;
  /// Run all currently parked delayed work.
  void               DrainDelayedAll();
  [[nodiscard]] auto pending() const -> std::size_t;
  [[nodiscard]] auto pending_delayed() const -> std::size_t;

 private:
  mutable std::mutex                mutex_;
  std::queue<std::function<void()>> pending_;
  std::queue<std::function<void()>> delayed_;
  std::thread::id                   owner_thread_;
};

/// Production session-owner executor. A dedicated thread runs admitted work in
/// admission order; `PostDelayed` waits on a condition variable until the
/// deadline elapses, so interactive pacing deadlines no longer depend on the
/// GUI thread's timer or event-loop availability. `Stop` lets the thread
/// finish due work and exit; destruction joins.
class EditorSessionThreadedCommandExecutor final : public IEditorSessionCommandExecutor {
 public:
  EditorSessionThreadedCommandExecutor();
  ~EditorSessionThreadedCommandExecutor() override;

  EditorSessionThreadedCommandExecutor(const EditorSessionThreadedCommandExecutor&) = delete;
  auto operator=(const EditorSessionThreadedCommandExecutor&)
      -> EditorSessionThreadedCommandExecutor&                                      = delete;

  void               Post(std::function<void()> task) override;
  void               PostDelayed(std::function<void()>    task,
                                 std::chrono::nanoseconds delay) override;
  [[nodiscard]] auto IsOwnerThread() const -> bool override;
  /// Stops the worker and joins it. Safe to call from any thread except the
  /// worker itself; pending tasks whose deadline has elapsed still run.
  void               Shutdown() override;

  /// Stop waiting for future work. The thread runs every task whose deadline
  /// has already elapsed, then exits. Tasks posted after the thread exits are
  /// discarded with the executor.
  void               Stop();

 private:
  struct ScheduledTask {
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t                         sequence = 0;
    std::function<void()>                 task;
  };

  void               Run();
  void               ScheduleLocked(ScheduledTask scheduled);

  mutable std::mutex            mutex_;
  std::condition_variable       cv_;
  // Min-heap ordered by (deadline, sequence); front is the earliest deadline.
  std::vector<ScheduledTask>    scheduled_;
  std::uint64_t                 next_sequence_ = 0;
  bool                          stopping_      = false;
  std::thread                   thread_;
  std::atomic<std::thread::id>  owner_thread_{};
};

/// Serialized actor queue for editor-session command reduction and completion
/// delivery. The reducer runs on the executor owner thread and never holds the
/// queue mutex while invoking user code.
class EditorSessionCommandQueue final {
 public:
  using CommandHandler = std::function<void(EditorSessionCommand)>;
  using Task           = std::function<void()>;

  struct Submission {
    EditorSessionOperationId operation{};
    bool                     accepted = false;
    bool                     executed = false;
  };

  explicit EditorSessionCommandQueue(
      std::shared_ptr<IEditorSessionCommandExecutor> executor = nullptr);
  ~EditorSessionCommandQueue();

  EditorSessionCommandQueue(const EditorSessionCommandQueue&)            = delete;
  EditorSessionCommandQueue& operator=(const EditorSessionCommandQueue&) = delete;

  /// Admit one user command and reduce it in queue order. An owner-thread
  /// submission may execute before this method returns; a nested submission is
  /// retained until the active reduction returns.
  auto               Submit(EditorSessionCommand command, CommandHandler handler) -> Submission;

  /// Post a completion to the owner thread. Unlike Submit, this path never
  /// runs the task inline, including when called from the owner thread.
  void               PostCompletion(Task task);

  /// Post a completion that the owner thread runs after `delay` elapses.
  /// Interactive pacing deadlines use this path so the session owner, not a
  /// GUI-thread timer, decides when the next serial consume happens.
  void               PostCompletionDelayed(Task task, std::chrono::nanoseconds delay);

  /// Stop admitting user commands while allowing already-posted completion
  /// work to finish during shutdown.
  void               BeginShutdown();

  /// Stop the queue and discard work that has not started.
  void               Stop();

  [[nodiscard]] auto state() const -> EditorSessionQueueState;
  [[nodiscard]] auto pending() const -> std::size_t;
  [[nodiscard]] auto IsOwnerThread() const -> bool;
  [[nodiscard]] auto executor() const -> const std::shared_ptr<IEditorSessionCommandExecutor>& {
    return executor_;
  }

 private:
  struct SharedState;

  static void EnqueueAndDrain(const std::shared_ptr<SharedState>& state, Task task);

  std::shared_ptr<IEditorSessionCommandExecutor> executor_;
  std::shared_ptr<SharedState>                   state_;
};

}  // namespace alcedo
