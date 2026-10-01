//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <duckdb.h>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/version_ref.hpp"
#include "json.hpp"
#include "storage/mapper/sleeve/edit_history/edit_commit_mapper.hpp"
#include "storage/mapper/sleeve/edit_history/image_edit_state_mapper.hpp"
#include "storage/mapper/sleeve/edit_history/version_ref_mapper.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief History identity of the materialized state of one image, read without its checkpoint.
 *
 * The labels a reader compares to decide whether a document built earlier still equals the
 * stored history state.
 */
struct MaterializedHistoryLabel {
  root_id_t                root_id{};
  head_commit_hash_t       head_commit_hash;
  transaction_chain_hash_t transaction_chain_hash{};
};

/**
 * @brief Rows of the immutable history root of one image, encoded without storage access.
 *
 * Produced by CommitGraphStore::EncodeRootPipeline so the CPU work (root id, checkpoint and root
 * state JSON) runs before the database lock. CommitGraphStore::InsertRootPipelines writes it. The
 * value is algorithm output for a root that does not exist yet; nothing else owns this state.
 */
struct EncodedRootPipeline {
  sl_element_id_t            element_id = 0;
  root_id_t                  root_id{};
  /// Serialized EncodePipelineRootState document stored in PipelineRoot.
  std::string                root_state_json;
  /// Default Version and ImageEditState of the new root, validated at encode time.
  CommitGraphMaterialization materialization;
};

/**
 * @brief Persist and reload the mini-Git commit graph for one image.
 *
 * Persistence only accepts a validated CommitGraphMaterialization so Version head, materialized
 * head, chain hash, and serialized pipeline state cannot be written as mutually inconsistent values.
 */
class CommitGraphStore {
 public:
  explicit CommitGraphStore(duckdb_connection& conn);

  auto InsertCommitIfAbsent(const EditCommit& commit) -> bool;
  auto GetCommit(const commit_hash_t& commit_hash) -> std::optional<EditCommit>;
  auto CountCommits() -> std::uint64_t;
  auto CountCommitsForRoot(const root_id_t& root_id) -> std::uint64_t;

  auto GetVersionRef(const version_ref_id_t& version_id) -> std::optional<VersionRef>;
  auto ListVersionRefsForElement(sl_element_id_t element_id) -> std::vector<VersionRef>;
  auto GetImageEditState(sl_element_id_t element_id) -> std::optional<ImageEditState>;

  /**
   * @brief Read only the root id, materialized head, and chain of @p element_id.
   *
   * Unlike @ref GetImageEditState it does not read or parse the serialized checkpoint document.
   * @return nullopt when the image has no edit state.
   */
  auto GetMaterializedHistoryLabel(sl_element_id_t element_id)
      -> std::optional<MaterializedHistoryLabel>;

  /// Read the immutable root state and verify that it belongs to the requested image.
  auto GetRootSerializedPipelineState(sl_element_id_t element_id, const root_id_t& root_id)
      -> std::optional<nlohmann::json>;

  /// Atomically materialize commits, Version refs, and ImageEditState from a validated capture.
  /// Fails before commit when the capture is inconsistent; prior rows remain unchanged.
  void Materialize(const CommitGraphMaterialization& materialization);

  /// Reload a graph with full structural and materialized-state validation.
  auto LoadGraph(sl_element_id_t element_id) -> std::optional<CommitGraph>;

  /// List every element_id that has an ImageEditState row.
  auto ListImageElementIds() -> std::vector<sl_element_id_t>;

  /// Mark all Version heads along first-parent reachability, then delete unreachable EditCommit
  /// rows for one image.
  /// Safe on abnormal restart only when called during a clean project exit after the final save.
  /// @return number of commit rows deleted.
  auto DeleteUnreachableCommits(sl_element_id_t element_id) -> std::size_t;

  /// Run DeleteUnreachableCommits for every image with edit state.
  /// @return total commit rows deleted across the project.
  auto DeleteUnreachableCommitsForProject() -> std::size_t;

  /// Delete every persisted Mini-Git row owned by one image.
  ///
  /// This is used only after the image itself has been removed from the project. The Version refs,
  /// commit objects, image state, and immutable root snapshot are removed in one transaction.
  void DeleteGraphForElement(sl_element_id_t element_id);

  /// Persist an empty image edit state (infrastructure bootstrap helper).
  auto CreateEmptyPersisted(sl_element_id_t element_id,
                            std::string default_display_name = "Default") -> CommitGraph;

  /// Create an empty graph and persist the immutable root document in one DuckDB transaction.
  ///
  /// `root_id` is computed from @p element_id, @p root_document, and @p raw_color_context.
  /// The stored root is never overwritten. A later call for the same image is rejected.
  auto CreateRootPipelinePersisted(sl_element_id_t element_id, const PipelineDocument& root_document,
                                   std::optional<nlohmann::json> raw_color_context = std::nullopt,
                                   std::string default_display_name = "Default") -> CommitGraph;

  /// Encode the root rows of a new image without touching storage (see EncodedRootPipeline).
  static auto EncodeRootPipeline(sl_element_id_t element_id, const PipelineDocument& root_document,
                                 const std::optional<nlohmann::json>& raw_color_context,
                                 std::string default_display_name = "Default")
      -> EncodedRootPipeline;

  /// Write the root state, default Version and ImageEditState of every image in @p roots in
  /// one transaction, then run @p write_rows in the same transaction so rows that must exist
  /// only together with the roots commit with them. Throws, and writes nothing, when any of the
  /// images already has a root or when a write fails.
  void InsertRootPipelines(std::span<const EncodedRootPipeline> roots,
                           const std::function<void()>&         write_rows = {});

 private:
  duckdb_connection&   conn_;
  EditCommitMapper     commit_mapper_;
  VersionRefMapper     version_ref_mapper_;
  ImageEditStateMapper image_edit_state_mapper_;

  void UpsertVersionRef(const VersionRef& version_ref);
  void UpsertImageEditState(const ImageEditState& state);
  void        InsertRootSerializedPipelineStates(std::span<const EncodedRootPipeline> roots);
  static auto EncodeRootPipelineForGraph(const CommitGraph&                   graph,
                                         const PipelineDocument&              root_document,
                                         const std::optional<nlohmann::json>& raw_color_context)
      -> EncodedRootPipeline;

  static auto ToCommitParams(const EditCommit& commit) -> EditCommitMapperParams;
  static auto FromCommitParams(EditCommitMapperParams&& params) -> EditCommit;
  static auto ToVersionRefParams(const VersionRef& ref) -> VersionRefMapperParams;
  static auto FromVersionRefParams(VersionRefMapperParams&& params) -> VersionRef;
  static auto ToImageEditStateParams(const ImageEditState& state) -> ImageEditStateMapperParams;
  static auto FromImageEditStateParams(ImageEditStateMapperParams&& params) -> ImageEditState;
};

}  // namespace alcedo
