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
#include <vector>

#include "edit/history/editor_transaction_journal.hpp"

namespace alcedo {

/// Small file boundary used by EditorJournalWriter. Implementations may
/// deliberately return short writes or failed flushes for deterministic fault
/// tests.
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
