//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "edit/history/editor_transaction_journal.hpp"

namespace alcedo {

/// Small file boundary used by EditorJournalWriter. Implementations may
/// deliberately return short writes or failed flushes for deterministic fault
/// tests. Compaction uses CreateExclusive / AtomicReplace / FlushDirectory on
/// ports that support journal rotation; the active journal is never rewritten
/// in place.
class IEditorJournalFile {
 public:
  virtual ~IEditorJournalFile() = default;

  virtual auto Append(const std::uint8_t* data, std::size_t size, std::size_t* written,
                      std::string* error) -> bool = 0;
  virtual auto Flush(std::string* error) -> bool = 0;

  /// Optional recovery read. Test doubles that only exercise append/flush can
  /// leave this unavailable.
  virtual auto ReadAll(std::string* /*error*/) -> std::optional<std::vector<std::uint8_t>> {
    return std::nullopt;
  }

  /// Write a brand-new sibling journal (create-new compaction path). Default
  /// returns false so append-only test doubles stay narrow.
  virtual auto CreateExclusive(const std::filesystem::path& /*path*/,
                               const std::uint8_t* /*data*/, std::size_t /*size*/,
                               std::string* error) -> bool {
    if (error) {
      *error = "CreateExclusive is not supported by this journal file port";
    }
    return false;
  }

  /// Atomically replace the active journal path with a previously verified
  /// compact sibling. Default returns false.
  virtual auto AtomicReplace(const std::filesystem::path& /*source*/,
                             const std::filesystem::path& /*destination*/,
                             std::string* error) -> bool {
    if (error) {
      *error = "AtomicReplace is not supported by this journal file port";
    }
    return false;
  }

  /// Flush the containing directory after create/replace on platforms that
  /// require explicit directory durability. Default is a no-op success.
  virtual auto FlushDirectory(const std::filesystem::path& /*directory*/,
                              std::string* /*error*/) -> bool {
    return true;
  }
};

/// Deterministic fault injector for Phase 5H storage-failure tests.
class InjectedEditorJournalFile final : public IEditorJournalFile {
 public:
  explicit InjectedEditorJournalFile(std::vector<std::uint8_t> initial = {});

  std::size_t max_write                 = 0;
  int         append_calls              = 0;
  int         flush_calls               = 0;
  int         create_calls              = 0;
  int         replace_calls             = 0;
  bool        fail_flush                = false;
  bool        fail_create               = false;
  bool        fail_replace              = false;
  bool        fail_directory_flush      = false;
  bool        corrupt_on_read           = false;
  std::size_t corrupt_on_read_offset    = 0;

  auto bytes() const -> const std::vector<std::uint8_t>& { return bytes_; }
  auto compact_files() const
      -> const std::unordered_map<std::string, std::vector<std::uint8_t>>& {
    return compact_files_;
  }
  void SetBytes(std::vector<std::uint8_t> bytes) { bytes_ = std::move(bytes); }

  auto Append(const std::uint8_t* data, std::size_t size, std::size_t* written,
              std::string* error) -> bool override;
  auto Flush(std::string* error) -> bool override;
  auto ReadAll(std::string* error) -> std::optional<std::vector<std::uint8_t>> override;
  auto CreateExclusive(const std::filesystem::path& path, const std::uint8_t* data,
                       std::size_t size, std::string* error) -> bool override;
  auto AtomicReplace(const std::filesystem::path& source,
                     const std::filesystem::path& destination, std::string* error)
      -> bool override;
  auto FlushDirectory(const std::filesystem::path& directory, std::string* error)
      -> bool override;

 private:
  std::vector<std::uint8_t>                              bytes_;
  std::unordered_map<std::string, std::vector<std::uint8_t>> compact_files_;
};

/// Native append-only journal file. Flush maps to FlushFileBuffers on Windows
/// and fsync on POSIX.
class FileEditorJournalFile final : public IEditorJournalFile {
 public:
  explicit FileEditorJournalFile(std::filesystem::path path);
  ~FileEditorJournalFile() override;

  FileEditorJournalFile(const FileEditorJournalFile&)            = delete;
  FileEditorJournalFile& operator=(const FileEditorJournalFile&) = delete;

  auto Append(const std::uint8_t* data, std::size_t size, std::size_t* written,
              std::string* error) -> bool override;
  auto Flush(std::string* error) -> bool override;
  auto ReadAll(std::string* error) -> std::optional<std::vector<std::uint8_t>> override;
  auto CreateExclusive(const std::filesystem::path& path, const std::uint8_t* data,
                       std::size_t size, std::string* error) -> bool override;
  auto AtomicReplace(const std::filesystem::path& source,
                     const std::filesystem::path& destination, std::string* error)
      -> bool override;
  auto FlushDirectory(const std::filesystem::path& directory, std::string* error)
      -> bool override;

 private:
  std::filesystem::path path_;
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int file_descriptor_ = -1;
#endif
};

struct EditorJournalWriterState {
  std::uint64_t next_record_sequence       = 1;
  std::uint64_t written_record_sequence    = 0;
  std::uint64_t durable_batch_commit_sequence = 0;
  std::uint64_t durable_operation_sequence = 0;
};

struct EditorJournalCommitResult {
  bool          accepted = false;
  bool          durable  = false;
  bool          pending   = false;
  std::uint64_t batch_commit_sequence = 0;
  std::uint64_t durable_operation_sequence = 0;
  std::string   error;
};

/// Image-scoped append/flush owner for the redo-only recovery journal.
///
/// Operation records are appended to the in-memory model first, followed by a
/// JournalBatchCommit control record. The writer advances durable sequence
/// values only after the native file flush succeeds. A failed write or flush
/// leaves the complete pending byte range available to RetryPending(); it
/// never silently reports a durable edit.
class EditorJournalWriter final {
 public:
  explicit EditorJournalWriter(std::shared_ptr<IEditorJournalFile> file);
  EditorJournalWriter(EditorTransactionJournal* journal,
                      std::shared_ptr<IEditorJournalFile> file);
  EditorJournalWriter(EditorJournalIdentity identity, std::filesystem::path path);

  EditorJournalWriter(const EditorJournalWriter&)            = delete;
  EditorJournalWriter& operator=(const EditorJournalWriter&) = delete;

  [[nodiscard]] auto journal() const -> const EditorTransactionJournal& { return *journal_; }
  [[nodiscard]] auto mutable_journal() -> EditorTransactionJournal& { return *journal_; }
  [[nodiscard]] auto state() const -> EditorJournalWriterState;
  [[nodiscard]] auto has_pending_batch() const -> bool;
  [[nodiscard]] auto last_error() const -> std::string;

  auto SetIdentity(EditorJournalIdentity identity) -> bool;

  /// These methods queue complete operation records in the image-scoped
  /// journal. They return the record sequence, or zero when a previous failed
  /// batch must be retried first.
  auto AppendEdit(const EditorJournalIdentity& identity, const EditTransaction& transaction)
      -> std::uint64_t;
  auto AppendCursorMove(const EditorJournalIdentity& identity, std::uint64_t from_cursor,
                        std::uint64_t to_cursor) -> std::uint64_t;
  auto AppendRewriteTimeline(const EditorJournalIdentity& identity,
                             const Hash128& expected_timeline_hash,
                             const Hash128& discarded_tail_hash, std::uint64_t retained_cursor,
                             const EditTransaction& replacement) -> std::uint64_t;
  auto AppendMaterializedHead(const EditorJournalIdentity& identity, const Hash128& timeline_hash,
                              std::uint64_t applied_cursor,
                              const nlohmann::json& head_pipeline_params) -> std::uint64_t;
  auto AppendRecoveryMarker(const EditorJournalIdentity& identity,
                            std::uint64_t last_valid_sequence, std::string note)
      -> std::uint64_t;

  /// Flush all queued operation records as one batch. Calling it again after a
  /// failed write/flush resumes the same batch instead of duplicating frames.
  auto CommitQueued() -> EditorJournalCommitResult;
  auto RetryPending() -> EditorJournalCommitResult { return CommitQueued(); }

  /// Discard a queued tail that has not reached the file API. A partially
  /// written batch is retained until it can be retried durably.
  auto DiscardQueued(std::string* error = nullptr) -> bool;

  /// Create-new compaction: encode a clean CompactionCheckpoint + MaterializedHead
  /// batch for the fully-materialized head, flush and verify the sibling, then
  /// atomically replace the active journal. A failed replace leaves the previous
  /// journal recoverable. Requires no pending batch.
  auto CompactToMaterializedHead(const EditorJournalIdentity& identity,
                                 const Hash128& timeline_hash, std::uint64_t applied_cursor,
                                 const nlohmann::json& head_pipeline_params,
                                 const std::filesystem::path& active_path,
                                 const std::filesystem::path& compact_path,
                                 std::string* error = nullptr) -> bool;

  /// Preserve the original journal bytes under a diagnostic path when recovery
  /// cannot validate a record chain. Returns the diagnostic path on success.
  auto EmitDiagnosticBundle(const std::filesystem::path& journal_path,
                            const std::string& reason, std::string* error = nullptr)
      -> std::optional<std::filesystem::path>;

 private:
  struct PendingBatch {
    bool                    active = false;
    std::size_t             start_offset = 0;
    std::size_t             write_offset = 0;
    std::vector<std::uint8_t> bytes;
    std::uint64_t            first_sequence = 0;
    std::uint64_t            last_operation_sequence = 0;
    std::uint64_t            batch_commit_sequence = 0;
    EditorJournalBatchCommitPayload payload{};
  };

  auto QueueRecord(EditorJournalIdentity identity, std::uint64_t sequence,
                   bool edit_history_operation, std::size_t start_offset) -> std::uint64_t;
  auto CanQueue(const EditorJournalIdentity& identity) -> bool;
  auto PreparePendingBatch() -> bool;
  auto WritePending(std::string* error) -> bool;
  auto FinishPending() -> EditorJournalCommitResult;
  void InitializeStateFromJournal();
  void SetError(std::string error);

  std::shared_ptr<IEditorJournalFile> file_;
  EditorTransactionJournal            owned_journal_;
  EditorTransactionJournal*           journal_ = nullptr;
  EditorJournalIdentity                identity_{};
  EditorJournalWriterState             state_{};
  PendingBatch                         pending_{};
  std::string                          last_error_;
  mutable std::mutex                   mutex_;
};

}  // namespace alcedo
