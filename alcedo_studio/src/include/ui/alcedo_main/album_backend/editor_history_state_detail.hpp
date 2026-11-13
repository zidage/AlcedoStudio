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
#include "app/editor_working_document.hpp"
#include "app/pipeline_root_state.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "json.hpp"
#include "type/hash_type.hpp"

namespace alcedo {
class MiniGitJournal;
class MiniGitWorkingHistory;
class PipelineMgmtService;
}  // namespace alcedo

namespace alcedo::ui {

class EditorSessionPipelinePort;

/// Per-image history state of the editor session, taken with the editor lease.
///
/// Owner: the session owner thread (the command queue) is the only writer of every field, so the
/// working document, the graph, and the pending-sequence fields take no lock. Renders and GUI
/// readers never read the working document; they read the preview snapshot that the history
/// publishes after each write (EditorHistoryState::PublishWorkingSnapshots).
struct HistoryWorkingState {
  /// The only CommitGraph of the image. MiniGitWorkingHistory appends to the same object.
  std::shared_ptr<alcedo::CommitGraph>           graph;
  /// Decoded immutable root; every replay starts here.
  std::shared_ptr<const alcedo::LoadedRootState> root;
  /// Working document; parameter writes, Version replay, and Paste change only this.
  std::shared_ptr<alcedo::EditorWorkingDocument> document;
  std::shared_ptr<alcedo::MiniGitJournal> journal;
  std::unique_ptr<alcedo::MiniGitWorkingHistory> history;
  /// First complete target of the current input sequence, keyed by field_key.
  struct DocumentFieldEdit {
    alcedo::EditorParameterTarget target;
    nlohmann::json                before_model_json;
    nlohmann::json                after_model_json;
  };
  std::unordered_map<std::string, DocumentFieldEdit> pending_document_sequence;
  /// True while an input sequence run through WithWorkingDocument (a Mask drag or Mask value
  /// edit) has left uncommitted values on the live document. Set from the operation's report
  /// after each call.
  bool                                                   mask_input_open = false;
  std::unordered_map<alcedo::Hash128, DocumentFieldEdit> document_edit_by_commit;
  /// Load-only panel values copied from working-document Models. Not a Model pointer
  /// and not a writable parameter mirror.
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

  /// True while the working document holds any value that is not committed: a pending slider
  /// sequence or an open locked-document input sequence. One rule for every kind of input. Save
  /// capture refuses to run while it is true, and the committed snapshot is not published.
  [[nodiscard]] auto             HasUncommittedLiveValues() const -> bool {
    return !pending_document_sequence.empty() || mask_input_open;
  }
};

/// Project every panel field of @p document into @p out: the fields of
/// @p projection_node_id when @p document still holds that node, else the
/// current-panel owners. Clears @p projection_node_id when the node is gone.
auto ProjectPanelFieldsForDocument(const alcedo::PipelineDocument& document,
                                   alcedo::NodeId*                 projection_node_id,
                                   alcedo::EditorPanelProjection* out, std::string* error) -> bool;

/// Owns per-image WorkingState acquisition, release, and service-path
/// resolution. Delegated by EditorSessionHistoryPort; does not contain
/// Mini-Git traversal or payload-formatting logic.
class EditorHistoryState {
 public:
  /// Path-resolution services used by state acquisition.
  struct Services {
    std::function<std::filesystem::path(sl_element_id_t)> mini_git_journal_path;
  };

  void SetServices(Services services);
  void SetPipelinePort(std::shared_ptr<EditorSessionPipelinePort> pipeline_port);

  /// Create the working history for one image: take the editor lease, attach the WAL, and bind
  /// the unique CommitGraph. Only the history port's Acquire calls this; it is the single place
  /// an image's editor history is loaded. A failure returns the lease.
  auto AcquireWorkingState(sl_element_id_t element_id, std::string* error)
      -> std::shared_ptr<HistoryWorkingState>;

  /// Return the acquired working history for one image. Never loads: an image that was not
  /// acquired (or was already released) fails, so a stale caller cannot rebind history from
  /// storage.
  auto EnsureWorkingState(sl_element_id_t element_id, std::string* error)
      -> std::shared_ptr<HistoryWorkingState>;

  /// Return a prepared working state without loading. Null when prepare has
  /// not finished; snapshot readers must not create state on the GUI thread.
  [[nodiscard]] auto PeekWorkingState(sl_element_id_t element_id) const
      -> std::shared_ptr<HistoryWorkingState>;

  /// Drop the working history state for one image and return its editor lease.
  /// @p discard_unmaterialized first clears the recovery journal, so the commits that only the
  /// journal records are not recovered by the next acquire.
  void ReleaseState(sl_element_id_t element_id, bool discard_unmaterialized = false);

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
   * @brief Publish the snapshots of the working document of @p element_id after a write.
   *
   * Always publishes the preview snapshot (EditorWorkingDocument::PublishPreview) that the
   * editor render and the GUI read. When the working document holds no uncommitted value
   * (@ref HistoryWorkingState::HasUncommittedLiveValues) and lineage, head, or chain changed since
   * the last publication, the same frozen document is also published to
   * PipelineMgmtService::PublishCommitted, labelled with the working head and chain. Call on the
   * owner thread after every write. A failed publication is logged; the write that preceded it
   * stands.
   */
  void               PublishWorkingSnapshots(sl_element_id_t element_id);

  /// Record the render reason of the last successful mutation on this port.
  void RecordPublishedRenderReason(std::optional<alcedo::EditorRenderReason> reason);

  /// Last successful mutation's render reason. Nullopt means no pipeline render.
  [[nodiscard]] auto LastPublishedRenderReason() const -> std::optional<alcedo::EditorRenderReason>;

  /**
   * @brief Build the document of @p head from @p state's immutable root.
   *
   * The build step of build-then-swap, shared by Version checkout, Version creation, and WAL
   * recovery. Uses the same replay as every other consumer (BuildDocumentFromRoot). Changes no
   * state; the caller swaps the result in with EditorWorkingDocument::Replace.
   * @return The new document, or null with @p error set.
   */
  static auto        BuildDocumentForHead(const HistoryWorkingState&        state,
                                          const alcedo::head_commit_hash_t& head, std::string* error)
      -> std::shared_ptr<alcedo::PipelineDocument>;

 private:
  Services services_{};
  mutable std::mutex mutex_;
  std::weak_ptr<EditorSessionPipelinePort> pipeline_port_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<HistoryWorkingState>> working_states_;
  std::optional<alcedo::EditorRenderReason> last_published_render_reason_ =
      alcedo::EditorRenderReason::UndoRedo;
};

}  // namespace alcedo::ui
