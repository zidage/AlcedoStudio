//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <duckdb.h>

#include <optional>
#include <string>
#include <vector>

#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/version_ref.hpp"
#include "storage/mapper/sleeve/edit_history/edit_commit_mapper.hpp"
#include "storage/mapper/sleeve/edit_history/image_edit_state_mapper.hpp"
#include "storage/mapper/sleeve/edit_history/version_ref_mapper.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Persist and reload the mini-Git commit graph for one image.
 *
 * Persistence only accepts a validated CommitGraphMaterialization so Version head, materialized
 * head, chain hash, and projection cannot be written as mutually inconsistent values.
 */
class CommitGraphService {
 public:
  explicit CommitGraphService(duckdb_connection& conn);

  auto InsertCommitIfAbsent(const EditCommit& commit) -> bool;
  auto GetCommit(const commit_hash_t& commit_hash) -> std::optional<EditCommit>;
  auto CountCommits() -> std::uint64_t;
  auto CountCommitsForRoot(const root_id_t& root_id) -> std::uint64_t;

  auto GetVersionRef(const version_ref_id_t& version_id) -> std::optional<VersionRef>;
  auto ListVersionRefsForElement(sl_element_id_t element_id) -> std::vector<VersionRef>;
  auto GetImageEditState(sl_element_id_t element_id) -> std::optional<ImageEditState>;

  /// Atomically materialize commits, Version refs, and ImageEditState from a validated capture.
  /// Fails before commit when the capture is inconsistent; prior rows remain unchanged.
  void Materialize(const CommitGraphMaterialization& materialization);

  /// Reload a graph with full structural and materialized-state validation.
  auto LoadGraph(sl_element_id_t element_id) -> std::optional<CommitGraph>;

  /// Persist an empty image edit state (infrastructure bootstrap helper).
  auto CreateEmptyPersisted(sl_element_id_t element_id,
                            std::string default_display_name = "Default") -> CommitGraph;

 private:
  duckdb_connection&   conn_;
  EditCommitMapper     commit_mapper_;
  VersionRefMapper     version_ref_mapper_;
  ImageEditStateMapper image_edit_state_mapper_;

  void UpsertVersionRef(const VersionRef& version_ref);
  void UpsertImageEditState(const ImageEditState& state);

  static auto ToCommitParams(const EditCommit& commit) -> EditCommitMapperParams;
  static auto FromCommitParams(EditCommitMapperParams&& params) -> EditCommit;
  static auto ToVersionRefParams(const VersionRef& ref) -> VersionRefMapperParams;
  static auto FromVersionRefParams(VersionRefMapperParams&& params) -> VersionRef;
  static auto ToImageEditStateParams(const ImageEditState& state) -> ImageEditStateMapperParams;
  static auto FromImageEditStateParams(ImageEditStateMapperParams&& params) -> ImageEditState;
};

}  // namespace alcedo
