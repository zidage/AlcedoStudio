//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "json.hpp"
#include "sleeve/storage_service.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Immutable capture taken at save-checkpoint start from the live pipeline
/// snapshot. Materialization never rebuilds or mutates a pipeline executor; it
/// only validates the journal fold against these values and writes DuckDB.
///
/// Owner/lifetime: built by the save path on the caller/GUI thread, then copied
/// into the worker request. journal_records is a snapshot of the exact prefix
/// being saved; an edit finalized after capture must not appear in this range
/// and must not be removed by truncating it.
struct EditorMiniGitSaveCapture {
  sl_element_id_t                   element_id         = 0;
  std::uint64_t                     session_generation = 0;
  head_commit_hash_t                working_head       = std::nullopt;
  transaction_chain_hash_t          transaction_chain_hash{};
  /// Full capture of commits + Version refs + ImageEditState with serialized
  /// pipeline state. Built from the live CommitGraph without a second executor.
  CommitGraphMaterialization        materialization{};
  std::vector<MiniGitJournalRecord> journal_records;
  std::filesystem::path             journal_path;
  /// True when the journal records have already been materialized (the
  /// in-memory journal contains no new commits or head moves that require
  /// a DuckDB write). Materialization still succeeds and truncates nothing.
  bool                              journal_already_materialized = false;
};

/// Outcome of one Materialize / RecoverAndMaterialize call.
///
/// - accepted: the capture was structurally valid and reached the DuckDB write
///   step. False means validation rejected before any durable write.
/// - materialized: DuckDB transaction committed. On success, the journal file
///   has been truncated for the saved prefix unless the result reports failure.
/// - head_moved: the checked-out Version head advanced (false for an empty
///   journal or a no-op recovery that only truncated leftover bytes).
/// - error: human-readable failure detail when accepted/materialized is false.
struct EditorMiniGitMaterializeResult {
  bool        accepted     = false;
  bool        materialized = false;
  /// False when the journal was empty and the Version head was not rewritten.
  bool        head_moved   = false;
  std::string error;
};

/// Coordinator that serializes editor save checkpoints. Defined in
/// editor_save_checkpoint_coordinator.hpp; held by the materializer as a shared
/// instance.
class EditorSaveCheckpointCoordinator;

/// Pure history materialization for the mini-Git journal. Does not touch
/// pipeline executors, GPU state, or render caches.
class EditorMiniGitMaterializer final {
 public:
  explicit EditorMiniGitMaterializer(std::shared_ptr<StorageService> storage);

  /// Validate the journal fold against the capture and write commits, Version
  /// head, serialized pipeline state, and recovery metadata in one DuckDB
  /// transaction. Truncates the saved journal prefix only after DuckDB success.
  ///
  /// Caller thread: the save worker (the global save lock is acquired here).
  /// On return the journal file is truncated iff materialized is true; on any
  /// failure the journal is left untouched and usable for retry or recovery.
  /// No pipeline executor is replayed or mutated.
  auto Materialize(const EditorMiniGitSaveCapture& capture, std::string* error = nullptr)
      -> EditorMiniGitMaterializeResult;

  /// Recover from the DB-commit / truncate crash window: load the DuckDB graph,
  /// skip the already-materialized journal prefix, fold remaining records,
  /// materialize, then truncate. Never inserts a commit twice.
  ///
  /// Caller thread: editor open / recovery path. On return the journal file is
  /// truncated iff materialized is true; on failure the journal is left intact.
  /// No pipeline executor is replayed or mutated.
  auto RecoverAndMaterialize(sl_element_id_t element_id, const std::filesystem::path& journal_path,
                             std::string* error = nullptr) -> EditorMiniGitMaterializeResult;

 private:
  std::shared_ptr<StorageService>                  storage_;
  std::shared_ptr<EditorSaveCheckpointCoordinator> coordinator_;
};

/// Fold journal records onto a graph loaded from DuckDB. Already-materialized
/// prefixes (crash after DuckDB commit, before truncate) are skipped when the
/// stored head has already advanced past them. Does not mutate pipelines.
auto FoldMiniGitJournalFromMaterializedBase(CommitGraph&                             graph,
                                            const std::vector<MiniGitJournalRecord>& records,
                                            std::string*                             error) -> bool;

/// Truncate a mini-Git journal file and clear its in-memory records after a
/// successful DuckDB materialization.
auto TruncateMiniGitJournal(MiniGitJournal& journal, std::string* error) -> bool;
auto TruncateMiniGitJournalFile(const std::filesystem::path& path, std::string* error) -> bool;

/// Build a serialized pipeline state document from live guard fields (no second executor).
auto MakeEditorSerializedPipelineState(const root_id_t& root_id, head_commit_hash_t head,
                                       const transaction_chain_hash_t& chain,
                                       const nlohmann::json& pipeline_params) -> nlohmann::json;

}  // namespace alcedo
