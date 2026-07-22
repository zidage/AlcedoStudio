//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
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
struct EditorMiniGitSaveCapture {
  sl_element_id_t              element_id = 0;
  std::uint64_t                session_generation = 0;
  head_commit_hash_t           working_head = std::nullopt;
  transaction_chain_hash_t     transaction_chain_hash{};
  /// Full capture of commits + Version refs + ImageEditState with serialized
  /// pipeline state. Built from the live CommitGraph without a second executor.
  CommitGraphMaterialization   materialization{};
  std::vector<MiniGitJournalRecord> journal_records;
  std::filesystem::path        journal_path;
  /// True when the journal is empty and the live head already equals the stored
  /// materialized head. Materialization still succeeds and truncates nothing.
  bool                         no_journal_changes = false;
};

struct EditorMiniGitMaterializeResult {
  bool        accepted     = false;
  bool        materialized = false;
  /// False when the journal was empty and the Version head was not rewritten.
  bool        head_moved   = false;
  std::string error;
};

/// Project-wide editor save coordinator for Phase 6C-5.
///
/// Serializes materialization so only one image save owns the global save lock
/// at a time. Capture happens outside DuckDB I/O; materialize validates the
/// journal fold, writes commits/Version/serialized state in one transaction,
/// then truncates the materialized journal prefix.
class EditorSaveCheckpointCoordinator final {
 public:
  class ScopedLock {
   public:
    ScopedLock() = default;
    ScopedLock(EditorSaveCheckpointCoordinator* owner, sl_element_id_t element_id, bool acquired);
    ScopedLock(const ScopedLock&)            = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
    ScopedLock(ScopedLock&& other) noexcept;
    ScopedLock& operator=(ScopedLock&& other) noexcept;
    ~ScopedLock();

    [[nodiscard]] auto owns_lock() const -> bool { return owns_; }
    void               Release();

   private:
    EditorSaveCheckpointCoordinator* owner_      = nullptr;
    sl_element_id_t                  element_id_ = 0;
    bool                             owns_       = false;
  };

  [[nodiscard]] auto TryAcquire(sl_element_id_t element_id) -> ScopedLock;
  [[nodiscard]] auto active_element_id() const -> sl_element_id_t;
  [[nodiscard]] auto is_saving() const -> bool;

 private:
  friend class ScopedLock;
  void Release(sl_element_id_t element_id);

  mutable std::mutex mutex_;
  bool               saving_         = false;
  sl_element_id_t    active_element_ = 0;
};

/// Pure history materialization for the mini-Git journal. Does not touch
/// pipeline executors, GPU state, or render caches.
class EditorMiniGitMaterializer final {
 public:
  explicit EditorMiniGitMaterializer(std::shared_ptr<StorageService> storage);

  /// Validate journal fold against the capture and write DuckDB in one
  /// transaction. Truncates the journal file only after DuckDB success.
  auto Materialize(const EditorMiniGitSaveCapture& capture, std::string* error = nullptr)
      -> EditorMiniGitMaterializeResult;

  /// Recover: load DuckDB graph, skip already-materialized journal prefix, fold
  /// remaining records, materialize, then truncate. Safe across the DB-commit /
  /// truncate crash window (no double commit insert).
  auto RecoverAndMaterialize(sl_element_id_t element_id, const std::filesystem::path& journal_path,
                             std::string* error = nullptr) -> EditorMiniGitMaterializeResult;

 private:
  auto MaterializeValidatedGraph(const EditorMiniGitSaveCapture& capture,
                                 CommitGraph&                    folded_graph,
                                 std::string*                    error)
      -> EditorMiniGitMaterializeResult;

  std::shared_ptr<StorageService>       storage_;
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
                                       const nlohmann::json&           pipeline_params)
    -> nlohmann::json;

}  // namespace alcedo
