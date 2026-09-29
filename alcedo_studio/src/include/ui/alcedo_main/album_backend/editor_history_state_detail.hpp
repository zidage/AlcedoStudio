//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "app/editor_adjustment_types.hpp"
#include "app/editor_panel_projection.hpp"
#include "app/editor_render_intent.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_types.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "json.hpp"
#include "type/hash_type.hpp"

namespace alcedo {
class MiniGitJournal;
class MiniGitWorkingHistory;
struct PipelineGuard;
class PipelineMgmtService;
}  // namespace alcedo

namespace alcedo::ui {

class EditorSessionPipelinePort;

/// Per-image history state owned by the queue-thread history unit. The command
/// queue is the sole mutation owner for graph, redo, and pending-sequence fields.
/// Live parameter writes go to pipeline_guard->document_ only.
/// Parameter writes, Version ops, and rendering share the executor render lock.
/// Load-only selected-node panel projection reads Models without that lock.
struct HistoryWorkingState {
  std::shared_ptr<alcedo::PipelineGuard> pipeline_guard;
  std::shared_ptr<alcedo::MiniGitJournal> journal;
  std::unique_ptr<alcedo::MiniGitWorkingHistory> history;
  /// First complete target of the current input sequence, keyed by field_key.
  struct DocumentFieldEdit {
    alcedo::EditorParameterTarget target;
    nlohmann::json                before_model_json;
    nlohmann::json                after_model_json;
  };
  std::unordered_map<std::string, DocumentFieldEdit> pending_document_sequence;
  /// True while an input sequence run through WithLockedLiveDocument (a Mask drag or Mask value
  /// edit) has left uncommitted values on the live document. Set from the operation's report
  /// after each call.
  bool                                                   locked_document_input_open = false;
  std::unordered_map<alcedo::Hash128, DocumentFieldEdit> document_edit_by_commit;
  /// Load-only panel values copied from live Models. Not a live Model pointer
  /// and not a writable parameter mirror. Selected-node copies do not take the
  /// render lock.
  alcedo::EditorPanelProjection panel_projection;
  /// Node last requested for panel projection. Empty means current-panel owners.
  alcedo::NodeId panel_projection_node_id;
  bool recovered_head = false;

  /// History state of the last committed snapshot published for this image.
  struct PublishedCommit {
    alcedo::PipelineLineageId        lineage;
    alcedo::head_commit_hash_t       head;
    alcedo::transaction_chain_hash_t chain{};
  };
  std::optional<PublishedCommit> last_published_commit;

  /// True while the live document holds any value that is not committed: a pending slider
  /// sequence or an open locked-document input sequence. One rule for every kind of input.
  [[nodiscard]] auto             HasUncommittedLiveValues() const -> bool {
    return !pending_document_sequence.empty() || locked_document_input_open;
  }
};

/// Owns per-image WorkingState acquisition, release, and service-path
/// resolution. Delegated by EditorSessionHistoryPort; does not contain
/// Mini-Git traversal or payload-formatting logic.
/// Project every panel field of @p document into @p out: the fields of
/// @p projection_node_id when @p document still holds that node, else the
/// current-panel owners. Clears @p projection_node_id when the node is gone.
/// Caller holds the render lock when @p document is live.
auto ProjectPanelFieldsForDocument(const alcedo::PipelineDocument& document,
                                   alcedo::NodeId*                 projection_node_id,
                                   alcedo::EditorPanelProjection* out, std::string* error) -> bool;

class EditorHistoryState {
 public:
  /// Path-resolution services used by state acquisition.
  struct Services {
    std::function<std::filesystem::path(sl_element_id_t)> mini_git_journal_path;
  };

  void SetServices(Services services);
  void SetPipelinePort(std::shared_ptr<EditorSessionPipelinePort> pipeline_port);

  /// Create the working history for one image: take editor ownership of its pipeline, attach
  /// the WAL, and bind the unique CommitGraph. Only the history port's Acquire calls this;
  /// it is the single place an image's editor history is loaded.
  auto AcquireWorkingState(sl_element_id_t element_id, std::string* error)
      -> std::shared_ptr<HistoryWorkingState>;

  /// Return the acquired working history for one image. Never loads: an image that was not
  /// acquired (or was already released) fails, so a stale caller cannot rebind history from
  /// storage. Also fails closed when the history no longer drives the live guard's CommitGraph.
  auto EnsureWorkingState(sl_element_id_t element_id, std::string* error)
      -> std::shared_ptr<HistoryWorkingState>;

  /// Return a prepared working state without loading. Null when prepare has
  /// not finished; snapshot readers must not create state on the GUI thread.
  [[nodiscard]] auto PeekWorkingState(sl_element_id_t element_id) const
      -> std::shared_ptr<HistoryWorkingState>;

  /// Drop the working history state for one image.
  void ReleaseState(sl_element_id_t element_id);

  /// Resolve the pipeline port (may be expired).
  [[nodiscard]] auto PipelinePort() const -> std::shared_ptr<EditorSessionPipelinePort>;

  /// Resolve the pipeline service through the current pipeline port.
  [[nodiscard]] auto PipelineMapper() const -> std::shared_ptr<alcedo::PipelineMgmtService>;

  /// Compare the live working head with the last materialized head.
  auto HasUnmaterializedChanges(sl_element_id_t element_id, std::string* error) -> bool;

  /// Return the journal-path resolver for checkpoint capture.
  [[nodiscard]] auto JournalPathResolver() const
      -> std::function<std::filesystem::path(sl_element_id_t)>;

  /**
   * @brief Publish the committed snapshot of @p element_id when its committed state changed.
   *
   * Freezes the live document and hands it to PipelineMgmtService::PublishCommitted, labelled
   * with the working head and chain. Does nothing while the live document holds uncommitted
   * values (@ref HistoryWorkingState::HasUncommittedLiveValues) or when lineage, head, and chain
   * equal the last publication. Call on the history owner thread after every history operation;
   * the document is written only on that thread, so the freeze needs no render lock.
   * A failed publication is logged; the history operation that preceded it stands.
   */
  void               PublishCommittedSnapshot(sl_element_id_t element_id);

  /// Record the render reason of the last successful mutation on this port.
  void RecordPublishedRenderReason(std::optional<alcedo::EditorRenderReason> reason);

  /// Last successful mutation's render reason. Nullopt means no pipeline render.
  [[nodiscard]] auto LastPublishedRenderReason() const -> std::optional<alcedo::EditorRenderReason>;

  /// Rebuild @p state's live document from the cached immutable root and @p head.
  ///
  /// Build-then-swap: replays a new document from the root, then binds it to the
  /// live guard under the render lock. Takes no copy of the prior document and
  /// does not move the Version ref. On failure the live document is left bound.
  auto ReplayWorkingDocumentFromImmutableRoot(HistoryWorkingState& state,
                                              const alcedo::head_commit_hash_t& head,
                                              std::string* error) -> bool;

 private:
  Services services_{};
  mutable std::mutex mutex_;
  std::weak_ptr<EditorSessionPipelinePort> pipeline_port_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<HistoryWorkingState>> working_states_;
  std::optional<alcedo::EditorRenderReason> last_published_render_reason_ =
      alcedo::EditorRenderReason::UndoRedo;
};

}  // namespace alcedo::ui
