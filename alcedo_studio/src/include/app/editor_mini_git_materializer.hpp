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

#include "app/editor_mini_git_commit_writer.hpp"
#include "app/editor_mini_git_journal_recovery.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "json.hpp"
#include "sleeve/storage_service.hpp"
#include "type/type.hpp"

namespace alcedo {

class EditorSaveCheckpointCoordinator;

/// Immutable capture taken at save-checkpoint start from the live pipeline
/// snapshot. Materialization never rebuilds or mutates a pipeline executor; it
/// only validates the journal fold against these values and writes DuckDB.
///
/// Owner/lifetime: built by the save path on the caller/GUI thread while the
/// project-owned SaveCheckpointLock is held, then copied into the worker
/// request. journal_records is a snapshot of the exact prefix being saved; an
/// edit finalized after capture must not appear in this range and must not be
/// removed by truncating it.
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

/// Thin facade for mini-Git journal materialization. Composes
/// EditorMiniGitJournalFold (pure algorithm), EditorMiniGitCommitWriter
/// (DuckDB transaction), and EditorMiniGitJournalRecovery (recovery +
/// truncation). This class owns no mutable state beyond its dependencies;
/// the three composed types own their own state.
///
/// Thread context: call Materialize from the save worker while the
/// project-owned SaveCheckpointLock is already held by
/// EditorSaveCheckpointService (Materialize does not re-acquire). Call
/// RecoverAndMaterialize from the recovery path; it acquires the project-owned
/// lock with AcquireBlocking and releases it before returning.
class EditorMiniGitMaterializer final {
 public:
  /// @param storage      Non-null StorageService used by the composed types.
  /// @param coordinator  Project-owned save lock. Required; materialization and
  ///                     recovery share one coordinator per open project.
  EditorMiniGitMaterializer(std::shared_ptr<StorageService>                  storage,
                            std::shared_ptr<EditorSaveCheckpointCoordinator> coordinator);

  /// Validate the journal fold against the capture and write commits, Version
  /// head, serialized pipeline state, and recovery metadata in one DuckDB
  /// transaction. Truncates the saved journal prefix only after DuckDB success.
  ///
  /// Caller thread: the save worker. Precondition: the project-owned
  /// SaveCheckpointLock for capture.element_id is already held by the save
  /// orchestrator (this method does not acquire it). On return the journal file
  /// is truncated iff materialized is true; on any failure the journal is left
  /// untouched and usable for retry or recovery. No pipeline executor is
  /// replayed or mutated.
  auto Materialize(const EditorMiniGitSaveCapture& capture, std::string* error = nullptr)
      -> EditorMiniGitMaterializeResult;

  /// Recover from the DB-commit / truncate crash window: load the DuckDB graph,
  /// skip the already-materialized journal prefix, fold remaining records,
  /// materialize, then truncate. Never inserts a commit twice.
  ///
  /// Caller thread: editor open / recovery path. Acquires the project-owned
  /// save lock with AcquireBlocking and holds it until return. On shutdown the
  /// lock is not granted and recovery fails without mutating DuckDB or the
  /// journal. On success the journal file is truncated iff materialized is
  /// true; on failure the journal is left intact.
  auto RecoverAndMaterialize(sl_element_id_t element_id, const std::filesystem::path& journal_path,
                             std::string* error = nullptr) -> EditorMiniGitMaterializeResult;

 private:
  std::shared_ptr<StorageService>                  storage_;
  std::shared_ptr<EditorSaveCheckpointCoordinator> coordinator_;
  std::unique_ptr<EditorMiniGitCommitWriter>       writer_;
  std::unique_ptr<EditorMiniGitJournalRecovery>    recovery_;
};

/// Build a serialized pipeline state document from live guard fields (no second executor).
auto MakeEditorSerializedPipelineState(const root_id_t& root_id, head_commit_hash_t head,
                                       const transaction_chain_hash_t& chain,
                                       const nlohmann::json& pipeline_params) -> nlohmann::json;

}  // namespace alcedo
