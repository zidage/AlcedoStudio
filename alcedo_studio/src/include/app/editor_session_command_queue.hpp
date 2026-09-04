//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

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
  AddColorGrade,
  RemoveColorGrade,
  RenameColorGrade,
  ReconnectColorGrade,
  EditNodeGraph,
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

/// Fully typed user command envelope. The queue stamps `operation.command_id`
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
  NodeId                                  before_node_id;
  NodeId                                  predecessor_node_id;
  NodeId                                  successor_node_id;
  NodeGraphTopologyChange                 topology_change{};
};

/// Typed worker completion envelope. Payload-specific values are kept as
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

  /// True when the caller is already running on the owning thread.
  [[nodiscard]] virtual auto IsOwnerThread() const -> bool    = 0;
};

/// Deterministic executor used by unit and integration tests. Posted work runs
/// only when `DrainOne` or `DrainAll` is called from the owner thread.
class EditorSessionManualCommandExecutor final : public IEditorSessionCommandExecutor {
 public:
  EditorSessionManualCommandExecutor();

  void               Post(std::function<void()> task) override;
  [[nodiscard]] auto IsOwnerThread() const -> bool override;

  /// Run one posted task and return false when no task is pending.
  auto               DrainOne() -> bool;
  /// Run all currently posted work, including work posted by a drained task.
  void               DrainAll();
  [[nodiscard]] auto pending() const -> std::size_t;

 private:
  mutable std::mutex                mutex_;
  std::queue<std::function<void()>> pending_;
  std::thread::id                   owner_thread_;
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
