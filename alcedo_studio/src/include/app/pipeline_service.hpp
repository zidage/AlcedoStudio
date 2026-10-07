//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "app/committed_snapshot_cache.hpp"
#include "app/pipeline_root_state.hpp"
#include "decoders/processor/raw_color_context.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "edit/runtime/lut_resource_resolver.hpp"
#include "sleeve/storage.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Materialized history of one image, read from storage for a history user other than the
 *        editor (the Copy source and the Paste targets in the library).
 *
 * Why a copy: the Copy dialog reads the graph after the call returns while other writers may
 * change storage, and a Paste target edits its own graph and persists it only when the whole
 * paste succeeded. Captured: the CommitGraph at its materialized state and the decoded immutable
 * root with the image DNG profile bound. Both are immutable; a Paste copies graph_ into a private
 * CommitGraph before it edits. Released with the last reference; nothing is written back except
 * through @ref PipelineMgmtService::PersistHistory, which checks graph_ against storage first.
 */
/**
 * @brief Rows of the history root of one newly imported image and its element pipeline JSON.
 *
 * Produced by PipelineMgmtService::EncodeImageRoot without the database lock so import workers
 * encode in parallel; PipelineMgmtService::WriteImageRoots writes many of them in one
 * transaction. The image has no stored root yet, so this value duplicates no stored state.
 */
struct EncodedImageRoot {
  EncodedRootPipeline root;
  /// PipelineDocument::ToJson of the bound document, kept for older application versions.
  std::string         element_pipeline_json;
};

struct ImageHistorySnapshot {
  std::shared_ptr<const CommitGraph>     graph_;
  std::shared_ptr<const LoadedRootState> root_;
};

/**
 * @brief History of one image that the editor session takes over with its lease.
 *
 * Returned by @ref PipelineMgmtService::AcquireEditorLease. Not a copy of shared state: the
 * session becomes the only owner of the image's history and working document until it releases
 * the lease, and no other module loads or changes them in that time.
 * - graph_: the materialized CommitGraph; the session appends its commits to it.
 * - root_: the decoded immutable root with the image DNG profile bound; the start of every replay.
 * - document_: the document of the active Version head, from the matching checkpoint or from a
 *   replay of the root; the session's working document.
 */
struct EditorHistoryLease {
  CommitGraph                            graph_;
  std::shared_ptr<const LoadedRootState> root_;
  std::shared_ptr<PipelineDocument>      document_;
};

/**
 * @brief Owner of the edit documents and histories of the images in a project.
 *
 * Loads, replays, and persists histories and documents; hands out immutable committed snapshots
 * and receives the ones the editor publishes; keeps the single-writer lease of the image open in
 * the editor; creates history roots; deletes and collects history. Holds no executor and no GPU
 * state: renderers own their executors and receive snapshots from this service.
 *
 * Internal state: the committed snapshot cache (CPU values only) and the editor lease table.
 */
class PipelineMgmtService final {
 private:
  std::shared_ptr<Storage>            storage_;

  /// Guards editor_leases_ and editor_pipeline_history_rebuild_count_.
  std::mutex                          lock_;

  /// Fixed for the life of the service; a backend change constructs a new service with the
  /// project. Owners read it when they construct their executors.
  const AcceleratorBackendPreference  accelerator_preference_;

  /// Resolver for Color Grade LUT references, fixed for the life of the service. Owners pass it
  /// to the executors they construct, like the accelerator preference.
  const std::shared_ptr<const LutResourceResolver> lut_resources_;

  std::uint64_t                       editor_pipeline_history_rebuild_count_ = 0;

  CommittedSnapshotCache              committed_snapshots_;

  /// Images whose history the editor session holds (AcquireEditorLease .. ReleaseEditorLease).
  /// The single-writer lease table, guarded by lock_. It carries no executor and no document.
  std::unordered_set<sl_element_id_t> editor_leases_;

  /// True while the editor session holds the lease of @p id.
  [[nodiscard]] auto                  EditorHoldsImage(sl_element_id_t id) -> bool;

  /// Write the document of @p snapshot as the element pipeline JSON, kept for older versions of
  /// the application (plan decision 3). The history in storage stays the source of truth.
  void WriteElementPipelineJson(const PipelineGraphSnapshot& snapshot);

 public:
  PipelineMgmtService() = delete;
  /**
   * @param storage_service Project storage; shared with the other project services.
   * @param accelerator_preference Backend that the executors of this project use.
   * @param lut_resources LUT resolver that the executors of this project use (the application
   *        LUT library); null selects the file-path-only DefaultLutResourceResolver.
   */
  explicit PipelineMgmtService(
      std::shared_ptr<Storage>     storage_service,
      AcceleratorBackendPreference accelerator_preference      = AcceleratorBackendPreference::Auto,
      std::shared_ptr<const LutResourceResolver> lut_resources = nullptr)
      : storage_(storage_service),
        accelerator_preference_(accelerator_preference),
        lut_resources_(lut_resources ? std::move(lut_resources) : DefaultLutResourceResolver()),
        committed_snapshots_(storage_service) {}

  /**
   * @brief Committed pipeline graph snapshot of @p id, for renders that must not see
   *        uncommitted editor values (thumbnails, analysis, export).
   *
   * For the image the editor holds, returns the snapshot the editor published last
   * (@ref PublishCommitted); when it has published none yet, the stored state. For every other
   * image, builds the snapshot from the materialized history in storage, or reuses the cached one
   * while its head and chain still equal the stored labels. Never takes a render lock and never
   * reads the element pipeline JSON.
   *
   * Thread: any thread; storage reads run on the calling thread, so call it off the UI thread.
   * @throws std::runtime_error when the image has no history root or its history cannot be built.
   */
  [[nodiscard]] auto AcquireCommittedSnapshot(sl_element_id_t id)
      -> std::shared_ptr<const PipelineGraphSnapshot>;

  /**
   * @brief Publish the editor's snapshot of a committed state of the image it holds.
   *
   * Called by the editor history after each change of its committed state. Later
   * @ref AcquireCommittedSnapshot calls for that image return it until the next publication or
   * until the editor releases the image.
   * @throws std::invalid_argument when @p snapshot is null or not committed.
   */
  void               PublishCommitted(std::shared_ptr<const PipelineGraphSnapshot> snapshot);

  /**
   * @brief Read the materialized history and the immutable root of @p id from storage.
   *
   * For history users other than the editor: the Copy source and the Paste targets in the
   * library. Constructs no executor and changes no state.
   *
   * Thread: any thread; storage reads run on the calling thread.
   * @throws std::runtime_error when the editor session holds @p id (its history may have commits
   *         that storage does not have yet; read it through the session), when the image has no
   *         history root, or when the stored history or root cannot be decoded or disagrees with
   *         the active Version.
   */
  [[nodiscard]] auto LoadHistorySnapshot(sl_element_id_t id) -> ImageHistorySnapshot;

  /**
   * @brief Persist @p graph, an edited copy of @p base, as the history of its image in one
   *        storage transaction, and publish its committed snapshot.
   *
   * Replays the document of the active Version of @p graph from the root of @p base, then writes
   * the new commits, the Version refs, the image edit state, and the checkpoint of that document
   * together. The returned snapshot is also stored in the committed snapshot cache, so the next
   * thumbnail or export of the image renders it without a replay. Constructs no executor. The
   * element pipeline JSON is not written.
   *
   * Thread: any thread; storage writes run on the calling thread.
   * @pre @p base came from @ref LoadHistorySnapshot for the same image as @p graph.
   * @throws std::runtime_error when the editor session holds the image, when @p graph belongs to
   *         another image or root, when replay fails, or when the stored history no longer equals
   *         the materialized state of @p base (another writer changed it). Storage is unchanged
   *         on every failure.
   */
  auto               PersistHistory(const ImageHistorySnapshot& base, const CommitGraph& graph)
      -> std::shared_ptr<const PipelineGraphSnapshot>;

  /// Test/instrumentation: committed snapshots built from storage since construction.
  [[nodiscard]] auto CommittedSnapshotStorageLoadCount() const -> std::size_t {
    return committed_snapshots_.StorageLoadCount();
  }

  /**
   * @brief Take the single-writer lease of @p id for the editor session and read its history.
   *
   * Reads the materialized history and the immutable root, then builds the document of the
   * active Version head: from the stored checkpoint when its root, head, and chain labels equal
   * the history tip, otherwise by replay from the root. While the lease is held, thumbnails,
   * analysis, and export read the snapshots the editor publishes (@ref PublishCommitted), and
   * @ref LoadHistorySnapshot and @ref PersistHistory refuse the image. Constructs no executor.
   *
   * Thread: any thread; storage reads run on the calling thread.
   * @throws std::runtime_error when the lease is already held, when the image has no history root
   *         (every imported image has one), or when its history or root cannot be decoded or
   *         replayed. The lease is not taken on failure.
   */
  [[nodiscard]] auto AcquireEditorLease(sl_element_id_t id) -> EditorHistoryLease;

  /**
   * @brief End the editor lease of @p id.
   *
   * The last committed snapshot the editor published becomes an ordinary cache entry that is
   * checked against storage. The document of the materialized history state is written as the
   * element pipeline JSON: commits that only the editor journal recorded (a Close with Discard)
   * never reach it. No effect when the lease is not held.
   */
  void               ReleaseEditorLease(sl_element_id_t id);

  /**
   * @brief Persist the editor's @p graph and the checkpoint of @p document in one transaction.
   *
   * @p document must be the document of the active Version head of @p graph; the checkpoint is
   * labelled with that head and its chain. Writes only when storage still holds
   * @p expected_materialized_state, so a concurrent writer is never overwritten. On success the
   * materialized state of @p graph is advanced to what was written.
   *
   * Thread: the editor session owner thread (the only writer of @p graph).
   * @return false with @p error set when the lease is not held, the graph belongs to another
   *         image, the stored state changed, or the write failed. Storage and @p graph are
   *         unchanged on failure.
   */
  auto PersistEditorHistory(CommitGraph& graph, const ImageEditState& expected_materialized_state,
                            const PipelineDocument& document, std::string* error = nullptr) -> bool;

  /// Test/instrumentation counter: increments each time AcquireEditorLease rebuilds the document
  /// from first-parent history instead of importing the serialized checkpoint.
  [[nodiscard]] auto EditorPipelineHistoryRebuildCount() const -> std::uint64_t {
    return editor_pipeline_history_rebuild_count_;
  }
  void ResetEditorPipelineHistoryRebuildCountForTesting() {
    editor_pipeline_history_rebuild_count_ = 0;
  }

  /**
   * @brief Create the immutable history root of a newly imported image from @p document.
   *
   * Binds the camera profile onto the caller's private document: the source DNG profile, then
   * the RAW color context, or the Rec.709 working-space profile when @p raw_color_context is null
   * (non-RAW RGB files). Then writes the root state, the default Version, and the image edit state
   * in one storage transaction, and writes the document as the element pipeline JSON (kept for
   * older versions of the application). Constructs no executor.
   *
   * @throws std::runtime_error when the document is not a valid product graph or the image
   *         already has a root (the stored root is never replaced). The element pipeline JSON is
   *         written only after the root.
   */
  void               InitializeImageRoot(sl_element_id_t id, PipelineDocument document,
                                         const RawRuntimeColorContext* raw_color_context);

  /**
   * @brief Bind and validate @p document like InitializeImageRoot and encode its rows; writes
   *        nothing. Safe to call from several threads at once.
   * @throws std::runtime_error when the document is not a valid product graph.
   */
  auto               EncodeImageRoot(sl_element_id_t id, PipelineDocument document,
                                     const RawRuntimeColorContext* raw_color_context) const -> EncodedImageRoot;

  /**
   * @brief Write the roots, default Versions, image edit states and element pipeline JSON of
   *        @p roots in one storage transaction.
   * @throws std::runtime_error when any image already has a root or a write fails; then nothing
   *         of @p roots is written.
   */
  void               WriteImageRoots(std::vector<EncodedImageRoot> roots);

  /// Clean project-exit garbage collection: mark from every Version head through first-parent
  /// reachability and delete unreachable EditCommit rows. Must run only after the final
  /// successful save; abnormal shutdown must not call this.
  /// @return number of deleted commit rows.
  auto               CollectUnreachableEditCommits() -> std::size_t;

  void               DeletePipeline(sl_element_id_t id);
  void               DeletePipelines(std::span<const sl_element_id_t> ids);

  /// LUT resolver for the executors of this project; never null.
  [[nodiscard]] auto LutResources() const -> const std::shared_ptr<const LutResourceResolver>& {
    return lut_resources_;
  }
  [[nodiscard]] auto GetAcceleratorBackendPreference() const -> AcceleratorBackendPreference {
    return accelerator_preference_;
  }

  /// Write the element pipeline JSON of every image the editor holds and published, from its
  /// materialized history state.
  void Sync();
};

}  // namespace alcedo
