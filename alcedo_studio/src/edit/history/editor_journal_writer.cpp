//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/editor_journal_writer.hpp"

#include "edit/history/editor_journal_recovery.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace alcedo {
namespace {

auto ErrorText(const char* operation, const std::filesystem::path& path) -> std::string {
  return std::string(operation) + " editor journal file " + path.string();
}

auto IsValidBatchCommit(const std::vector<EditorJournalDecodedRecord>& records,
                        const EditorJournalDecodedRecord&              record,
                        std::uint64_t previous_commit) -> bool {
  if (record.record_type != EditorJournalRecordType::JournalBatchCommit ||
      !record.batch_commit.has_value()) {
    return false;
  }
  const auto& payload = *record.batch_commit;
  if (payload.previous_batch_commit_sequence != previous_commit ||
      payload.first_covered_sequence != previous_commit + 1 ||
      payload.last_covered_sequence != record.sequence - 1 ||
      payload.first_covered_sequence > payload.last_covered_sequence ||
      payload.last_operation_sequence > payload.last_covered_sequence) {
    return false;
  }
  if (ComputeEditorJournalRecordChainHash(records, payload.last_covered_sequence) !=
      payload.record_chain_hash) {
    return false;
  }

  std::uint64_t last_operation = 0;
  for (const auto& covered : records) {
    if (covered.sequence >= payload.first_covered_sequence &&
        covered.sequence <= payload.last_covered_sequence &&
        IsEditorJournalEditHistoryRecord(covered.record_type)) {
      last_operation = covered.sequence;
    }
  }
  return last_operation == payload.last_operation_sequence;
}

}  // namespace

FileEditorJournalFile::FileEditorJournalFile(std::filesystem::path path)
    : path_(std::move(path)) {
  if (path_.empty()) {
    throw std::invalid_argument("FileEditorJournalFile requires a path");
  }
  if (path_.has_parent_path()) {
    std::filesystem::create_directories(path_.parent_path());
  }

#ifdef _WIN32
  const HANDLE handle = CreateFileW(
      path_.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw std::runtime_error(ErrorText("open", path_));
  }
  handle_ = handle;
  LARGE_INTEGER end{};
  if (!SetFilePointerEx(handle, end, nullptr, FILE_END)) {
    CloseHandle(handle);
    handle_ = nullptr;
    throw std::runtime_error(ErrorText("seek", path_));
  }
#else
  file_descriptor_ = ::open(path_.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
  if (file_descriptor_ < 0) {
    throw std::runtime_error(ErrorText("open", path_));
  }
#endif
}

FileEditorJournalFile::~FileEditorJournalFile() {
#ifdef _WIN32
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (file_descriptor_ >= 0) {
    ::close(file_descriptor_);
    file_descriptor_ = -1;
  }
#endif
}

auto FileEditorJournalFile::Append(const std::uint8_t* data, std::size_t size,
                                   std::size_t* written, std::string* error) -> bool {
  if (written == nullptr) {
    if (error) {
      *error = "editor journal append received a null write-count pointer";
    }
    return false;
  }
  *written = 0;
  if (size == 0) {
    return true;
  }
  if (data == nullptr) {
    if (error) {
      *error = "editor journal append received null data";
    }
    return false;
  }

#ifdef _WIN32
  if (handle_ == nullptr) {
    if (error) {
      *error = "editor journal file is closed";
    }
    return false;
  }
  const auto max_dword = static_cast<std::size_t>((std::numeric_limits<DWORD>::max)());
  const DWORD request = static_cast<DWORD>(size < max_dword ? size : max_dword);
  DWORD actual = 0;
  if (!WriteFile(static_cast<HANDLE>(handle_), data, request, &actual, nullptr)) {
    if (error) {
      *error = "WriteFile failed for editor journal";
    }
    return false;
  }
  *written = actual;
  return true;
#else
  if (file_descriptor_ < 0) {
    if (error) {
      *error = "editor journal file is closed";
    }
    return false;
  }
  const auto actual = ::write(file_descriptor_, data, size);
  if (actual < 0) {
    if (error) {
      *error = "write failed for editor journal";
    }
    return false;
  }
  *written = static_cast<std::size_t>(actual);
  return true;
#endif
}

auto FileEditorJournalFile::Flush(std::string* error) -> bool {
#ifdef _WIN32
  if (handle_ == nullptr || !FlushFileBuffers(static_cast<HANDLE>(handle_))) {
    if (error) {
      *error = "FlushFileBuffers failed for editor journal";
    }
    return false;
  }
  return true;
#else
  if (file_descriptor_ < 0 || ::fsync(file_descriptor_) != 0) {
    if (error) {
      *error = "fsync failed for editor journal";
    }
    return false;
  }
  return true;
#endif
}

auto FileEditorJournalFile::ReadAll(std::string* error)
    -> std::optional<std::vector<std::uint8_t>> {
  std::ifstream input(path_, std::ios::binary);
  if (!input) {
    if (error) {
      *error = ErrorText("read", path_);
    }
    return std::nullopt;
  }
  input.seekg(0, std::ios::end);
  const auto end = input.tellg();
  if (end < 0) {
    if (error) {
      *error = ErrorText("seek", path_);
    }
    return std::nullopt;
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
      if (error) {
        *error = ErrorText("read", path_);
      }
      return std::nullopt;
    }
  }
  return bytes;
}

auto FileEditorJournalFile::CreateExclusive(const std::filesystem::path& path,
                                            const std::uint8_t* data, std::size_t size,
                                            std::string* error) -> bool {
  if (path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      if (error) {
        *error = "failed to create journal parent directory: " + ec.message();
      }
      return false;
    }
  }
#ifdef _WIN32
  const HANDLE handle = CreateFileW(
      path.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (error) {
      *error = ErrorText("create-exclusive", path);
    }
    return false;
  }
  std::size_t offset = 0;
  while (offset < size) {
    const auto remaining = size - offset;
    const auto max_dword = static_cast<std::size_t>((std::numeric_limits<DWORD>::max)());
    const DWORD request  = static_cast<DWORD>(remaining < max_dword ? remaining : max_dword);
    DWORD       actual   = 0;
    if (!WriteFile(handle, data + offset, request, &actual, nullptr) || actual == 0) {
      CloseHandle(handle);
      if (error) {
        *error = ErrorText("write compact", path);
      }
      return false;
    }
    offset += actual;
  }
  if (!FlushFileBuffers(handle)) {
    CloseHandle(handle);
    if (error) {
      *error = ErrorText("flush compact", path);
    }
    return false;
  }
  CloseHandle(handle);
  return true;
#else
  const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
  if (fd < 0) {
    if (error) {
      *error = ErrorText("create-exclusive", path);
    }
    return false;
  }
  std::size_t offset = 0;
  while (offset < size) {
    const auto actual = ::write(fd, data + offset, size - offset);
    if (actual <= 0) {
      ::close(fd);
      if (error) {
        *error = ErrorText("write compact", path);
      }
      return false;
    }
    offset += static_cast<std::size_t>(actual);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    if (error) {
      *error = ErrorText("flush compact", path);
    }
    return false;
  }
  ::close(fd);
  return true;
#endif
}

auto FileEditorJournalFile::AtomicReplace(const std::filesystem::path& source,
                                          const std::filesystem::path& destination,
                                          std::string*                 error) -> bool {
  std::error_code ec;
  std::filesystem::rename(source, destination, ec);
  if (ec) {
    // Windows cannot rename over an existing file; remove destination first.
    std::filesystem::remove(destination, ec);
    std::filesystem::rename(source, destination, ec);
  }
  if (ec) {
    if (error) {
      *error = "atomic journal replace failed: " + ec.message();
    }
    return false;
  }
  return true;
}

auto FileEditorJournalFile::FlushDirectory(const std::filesystem::path& directory,
                                           std::string* error) -> bool {
#ifdef _WIN32
  const HANDLE handle = CreateFileW(
      directory.wstring().c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (error) {
      *error = ErrorText("open directory", directory);
    }
    return false;
  }
  const bool ok = FlushFileBuffers(handle) != 0;
  CloseHandle(handle);
  if (!ok && error) {
    *error = ErrorText("flush directory", directory);
  }
  return ok;
#else
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    if (error) {
      *error = ErrorText("open directory", directory);
    }
    return false;
  }
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  if (!ok && error) {
    *error = ErrorText("flush directory", directory);
  }
  return ok;
#endif
}

InjectedEditorJournalFile::InjectedEditorJournalFile(std::vector<std::uint8_t> initial)
    : bytes_(std::move(initial)) {}

auto InjectedEditorJournalFile::Append(const std::uint8_t* data, std::size_t size,
                                       std::size_t* written, std::string* error) -> bool {
  ++append_calls;
  if (written == nullptr) {
    if (error) {
      *error = "missing write count";
    }
    return false;
  }
  const auto count = max_write == 0 ? size : (size < max_write ? size : max_write);
  if (data != nullptr && count > 0) {
    bytes_.insert(bytes_.end(), data, data + count);
  }
  *written = count;
  return true;
}

auto InjectedEditorJournalFile::Flush(std::string* error) -> bool {
  ++flush_calls;
  if (fail_flush) {
    if (error) {
      *error = "injected flush failure";
    }
    return false;
  }
  return true;
}

auto InjectedEditorJournalFile::ReadAll(std::string* /*error*/)
    -> std::optional<std::vector<std::uint8_t>> {
  auto copy = bytes_;
  if (corrupt_on_read && !copy.empty()) {
    const auto offset =
        corrupt_on_read_offset < copy.size() ? corrupt_on_read_offset : (copy.size() - 1);
    copy[offset] ^= 0xFFu;
  }
  return copy;
}

auto InjectedEditorJournalFile::CreateExclusive(const std::filesystem::path& path,
                                                const std::uint8_t* data, std::size_t size,
                                                std::string* error) -> bool {
  ++create_calls;
  if (fail_create) {
    if (error) {
      *error = "injected create failure";
    }
    return false;
  }
  const auto key = path.string();
  if (compact_files_.contains(key)) {
    if (error) {
      *error = "compact journal already exists";
    }
    return false;
  }
  std::vector<std::uint8_t> body;
  if (data != nullptr && size > 0) {
    body.assign(data, data + size);
  }
  compact_files_[key] = std::move(body);
  return true;
}

auto InjectedEditorJournalFile::AtomicReplace(const std::filesystem::path& source,
                                              const std::filesystem::path& destination,
                                              std::string* error) -> bool {
  ++replace_calls;
  if (fail_replace) {
    if (error) {
      *error = "injected atomic replace failure";
    }
    return false;
  }
  const auto source_key = source.string();
  auto       it         = compact_files_.find(source_key);
  if (it == compact_files_.end()) {
    if (error) {
      *error = "compact source missing";
    }
    return false;
  }
  bytes_ = it->second;
  compact_files_.erase(it);
  (void)destination;
  return true;
}

auto InjectedEditorJournalFile::FlushDirectory(const std::filesystem::path& /*directory*/,
                                               std::string* error) -> bool {
  if (fail_directory_flush) {
    if (error) {
      *error = "injected directory flush failure";
    }
    return false;
  }
  return true;
}

EditorJournalWriter::EditorJournalWriter(std::shared_ptr<IEditorJournalFile> file)
    : file_(std::move(file)), journal_(&owned_journal_) {
  if (!file_) {
    throw std::invalid_argument("EditorJournalWriter requires a file port");
  }
  std::string error;
  const auto bytes = file_->ReadAll(&error);
  if (!bytes.has_value()) {
    // A port may intentionally omit recovery reads (the append/flush-only
    // test seam), but a native file port reports actual read failures.
    if (!error.empty()) {
      throw std::runtime_error(error);
    }
  } else if (!journal_->LoadBytes(*bytes, &error)) {
    throw std::runtime_error(error.empty() ? "failed to load editor journal" : error);
  }
  InitializeStateFromJournal();
}

EditorJournalWriter::EditorJournalWriter(EditorTransactionJournal* journal,
                                         std::shared_ptr<IEditorJournalFile> file)
    : file_(std::move(file)), journal_(journal != nullptr ? journal : &owned_journal_) {
  if (!file_) {
    throw std::invalid_argument("EditorJournalWriter requires a file port");
  }
  InitializeStateFromJournal();
}

EditorJournalWriter::EditorJournalWriter(EditorJournalIdentity identity,
                                         std::filesystem::path path)
    : EditorJournalWriter(std::make_shared<FileEditorJournalFile>(std::move(path))) {
  identity_ = identity;
}

auto EditorJournalWriter::state() const -> EditorJournalWriterState {
  std::scoped_lock lock(mutex_);
  auto state = state_;
  state.next_record_sequence = journal_->next_sequence();
  return state;
}

auto EditorJournalWriter::has_pending_batch() const -> bool {
  std::scoped_lock lock(mutex_);
  return pending_.active || pending_.first_sequence != 0;
}

auto EditorJournalWriter::last_error() const -> std::string {
  std::scoped_lock lock(mutex_);
  return last_error_;
}

auto EditorJournalWriter::SetIdentity(EditorJournalIdentity identity) -> bool {
  std::scoped_lock lock(mutex_);
  if (pending_.active || pending_.first_sequence != 0) {
    SetError("cannot change editor journal identity with a pending batch");
    return false;
  }
  if (identity_.element_id != 0 &&
      (identity_.element_id != identity.element_id || identity_.version_id != identity.version_id ||
       identity_.journal_generation != identity.journal_generation)) {
    SetError("editor journal identity changed without a new journal writer");
    return false;
  }
  identity_ = identity;
  return true;
}

auto EditorJournalWriter::CanQueue(const EditorJournalIdentity& identity) -> bool {
  if (!file_) {
    SetError("editor journal file port is missing");
    return false;
  }
  if (pending_.active) {
    SetError("editor journal has a pending batch; retry it before appending another operation");
    return false;
  }
  if (identity_.element_id != 0 &&
      (identity_.element_id != identity.element_id || identity_.version_id != identity.version_id ||
       identity_.journal_generation != identity.journal_generation)) {
    SetError("editor journal operation identity mismatch");
    return false;
  }
  if (identity_.element_id == 0) {
    identity_ = identity;
  } else {
    identity_.session_generation = identity.session_generation;
  }
  return true;
}

auto EditorJournalWriter::QueueRecord(EditorJournalIdentity identity, std::uint64_t sequence,
                                      bool edit_history_operation,
                                      std::size_t start_offset) -> std::uint64_t {
  if (pending_.first_sequence == 0) {
    pending_.start_offset = start_offset;
    pending_.first_sequence = sequence;
  }
  if (edit_history_operation) {
    pending_.last_operation_sequence = sequence;
  }
  identity_ = identity;
  state_.next_record_sequence = journal_->next_sequence();
  return sequence;
}

auto EditorJournalWriter::AppendEdit(const EditorJournalIdentity& identity,
                                     const EditTransaction&       transaction) -> std::uint64_t {
  std::scoped_lock lock(mutex_);
  if (!CanQueue(identity)) {
    return 0;
  }
  const auto start_offset = journal_->size();
  const auto sequence = journal_->AppendEdit(identity, transaction);
  return QueueRecord(identity, sequence, true, start_offset);
}

auto EditorJournalWriter::AppendCursorMove(const EditorJournalIdentity& identity,
                                           std::uint64_t from_cursor,
                                           std::uint64_t to_cursor) -> std::uint64_t {
  std::scoped_lock lock(mutex_);
  if (!CanQueue(identity)) {
    return 0;
  }
  const auto start_offset = journal_->size();
  const auto sequence = journal_->AppendCursorMove(identity, from_cursor, to_cursor);
  return QueueRecord(identity, sequence, true, start_offset);
}

auto EditorJournalWriter::AppendRewriteTimeline(
    const EditorJournalIdentity& identity, const Hash128& expected_timeline_hash,
    const Hash128& discarded_tail_hash, std::uint64_t retained_cursor,
    const EditTransaction& replacement) -> std::uint64_t {
  std::scoped_lock lock(mutex_);
  if (!CanQueue(identity)) {
    return 0;
  }
  const auto start_offset = journal_->size();
  const auto sequence = journal_->AppendRewriteTimeline(identity, expected_timeline_hash,
                                                        discarded_tail_hash, retained_cursor,
                                                        replacement);
  return QueueRecord(identity, sequence, true, start_offset);
}

auto EditorJournalWriter::AppendMaterializedHead(
    const EditorJournalIdentity& identity, const Hash128& timeline_hash,
    std::uint64_t applied_cursor, const nlohmann::json& head_pipeline_params) -> std::uint64_t {
  std::scoped_lock lock(mutex_);
  if (!CanQueue(identity)) {
    return 0;
  }
  const auto start_offset = journal_->size();
  const auto sequence = journal_->AppendMaterializedHead(identity, timeline_hash, applied_cursor,
                                                          head_pipeline_params);
  return QueueRecord(identity, sequence, false, start_offset);
}

auto EditorJournalWriter::AppendRecoveryMarker(const EditorJournalIdentity& identity,
                                               std::uint64_t last_valid_sequence,
                                               std::string note) -> std::uint64_t {
  std::scoped_lock lock(mutex_);
  if (!CanQueue(identity)) {
    return 0;
  }
  const auto start_offset = journal_->size();
  const auto sequence = journal_->AppendRecoveryMarker(identity, last_valid_sequence,
                                                       std::move(note));
  return QueueRecord(identity, sequence, false, start_offset);
}

auto EditorJournalWriter::PreparePendingBatch() -> bool {
  if (pending_.active || pending_.first_sequence == 0) {
    return pending_.active;
  }
  const auto decoded = journal_->DecodeRecordChain();
  if (decoded.stopped_on_corrupt_record || decoded.valid_chain_byte_count != journal_->size()) {
    SetError(decoded.message.empty() ? "cannot commit a corrupt editor journal tail"
                                     : decoded.message);
    return false;
  }
  const auto last_covered_sequence = journal_->next_sequence() - 1;
  if (last_covered_sequence < pending_.first_sequence) {
    SetError("editor journal pending range is empty");
    return false;
  }

  EditorJournalBatchCommitPayload payload;
  payload.previous_batch_commit_sequence = state_.durable_batch_commit_sequence;
  payload.first_covered_sequence         = pending_.first_sequence;
  payload.last_covered_sequence          = last_covered_sequence;
  payload.last_operation_sequence        = pending_.last_operation_sequence;
  payload.record_chain_hash =
      ComputeEditorJournalRecordChainHash(decoded.records, last_covered_sequence);

  const auto batch_sequence = journal_->AppendJournalBatchCommit(identity_, payload);
  pending_.payload                 = payload;
  pending_.batch_commit_sequence   = batch_sequence;
  pending_.bytes.assign(journal_->bytes().begin() +
                            static_cast<std::ptrdiff_t>(pending_.start_offset),
                        journal_->bytes().end());
  pending_.write_offset = 0;
  pending_.active       = true;
  state_.next_record_sequence = journal_->next_sequence();
  return true;
}

auto EditorJournalWriter::WritePending(std::string* error) -> bool {
  while (pending_.write_offset < pending_.bytes.size()) {
    std::size_t written = 0;
    const auto  remaining = pending_.bytes.size() - pending_.write_offset;
    std::string write_error;
    const bool  ok = file_->Append(pending_.bytes.data() + pending_.write_offset, remaining,
                                   &written, &write_error);
    if (written > remaining) {
      written = remaining;
    }
    pending_.write_offset += written;
    if (!ok || written == 0) {
      if (error) {
        *error = write_error.empty() ? "editor journal append failed" : write_error;
      }
      return false;
    }
  }
  return true;
}

auto EditorJournalWriter::FinishPending() -> EditorJournalCommitResult {
  state_.written_record_sequence       = pending_.batch_commit_sequence;
  state_.durable_batch_commit_sequence = pending_.batch_commit_sequence;
  state_.durable_operation_sequence    = pending_.payload.last_operation_sequence;
  state_.next_record_sequence          = journal_->next_sequence();

  EditorJournalCommitResult result;
  result.accepted                    = true;
  result.durable                     = true;
  result.pending                     = false;
  result.batch_commit_sequence       = state_.durable_batch_commit_sequence;
  result.durable_operation_sequence  = state_.durable_operation_sequence;
  pending_                           = {};
  last_error_.clear();
  return result;
}

auto EditorJournalWriter::CommitQueued() -> EditorJournalCommitResult {
  std::scoped_lock lock(mutex_);
  EditorJournalCommitResult result;
  if (!pending_.active && pending_.first_sequence == 0) {
    result.accepted                   = true;
    result.durable                    = true;
    result.batch_commit_sequence      = state_.durable_batch_commit_sequence;
    result.durable_operation_sequence = state_.durable_operation_sequence;
    return result;
  }
  if (!pending_.active && !PreparePendingBatch()) {
    result.accepted = true;
    result.pending  = true;
    result.error    = last_error_;
    return result;
  }
  result.accepted              = true;
  result.batch_commit_sequence = pending_.batch_commit_sequence;
  result.durable_operation_sequence = state_.durable_operation_sequence;

  std::string error;
  if (!WritePending(&error)) {
    SetError(error.empty() ? "editor journal flush failed" : error);
    result.pending = true;
    result.error   = last_error_;
    return result;
  }
  // The write reached the file API, but it is not durable until the flush
  // below succeeds. Keep these two sequence values independent.
  state_.written_record_sequence = pending_.batch_commit_sequence;
  if (!file_->Flush(&error)) {
    SetError(error.empty() ? "editor journal flush failed" : error);
    result.pending = true;
    result.error   = last_error_;
    return result;
  }
  return FinishPending();
}

auto EditorJournalWriter::DiscardQueued(std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  if (pending_.first_sequence == 0) {
    return true;
  }
  if (pending_.active && pending_.write_offset != 0) {
    SetError("cannot discard an editor journal batch after file writing started");
    if (error) {
      *error = last_error_;
    }
    return false;
  }
  std::string truncate_error;
  if (!journal_->Truncate(pending_.start_offset, &truncate_error)) {
    SetError(truncate_error.empty() ? "failed to discard editor journal tail" : truncate_error);
    if (error) {
      *error = last_error_;
    }
    return false;
  }
  pending_ = {};
  state_.next_record_sequence = journal_->next_sequence();
  last_error_.clear();
  return true;
}

void EditorJournalWriter::InitializeStateFromJournal() {
  state_ = {};
  state_.next_record_sequence = journal_->next_sequence();
  const auto decoded = journal_->DecodeRecordChain();
  if (!decoded.records.empty()) {
    state_.written_record_sequence = decoded.records.back().sequence;
  }

  std::uint64_t previous_commit = 0;
  for (const auto& record : decoded.records) {
    if (!IsValidBatchCommit(decoded.records, record, previous_commit)) {
      continue;
    }
    previous_commit                  = record.sequence;
    state_.durable_batch_commit_sequence = record.sequence;
    state_.durable_operation_sequence = record.batch_commit->last_operation_sequence;
  }
  if (identity_.element_id == 0 && !decoded.records.empty()) {
    identity_ = decoded.records.front().identity;
  }
}

void EditorJournalWriter::SetError(std::string error) { last_error_ = std::move(error); }

auto EditorJournalWriter::CompactToMaterializedHead(
    const EditorJournalIdentity& identity, const Hash128& timeline_hash,
    std::uint64_t applied_cursor, const nlohmann::json& head_pipeline_params,
    const std::filesystem::path& active_path, const std::filesystem::path& compact_path,
    std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  if (pending_.active || pending_.first_sequence != 0) {
    SetError("cannot compact editor journal with a pending batch");
    if (error) {
      *error = last_error_;
    }
    return false;
  }
  if (!file_) {
    SetError("editor journal file port is missing");
    if (error) {
      *error = last_error_;
    }
    return false;
  }

  // Compaction replaces the append-only growth with a verified checkpoint. DuckDB
  // recovery metadata remains authoritative for the transaction chain; the compact
  // journal only records that physical omission of prior rewrite tails completed.
  EditorTransactionJournal compact_journal;
  const auto checkpoint_seq = compact_journal.AppendCompactionCheckpoint(
      identity, state_.durable_operation_sequence,
      "compact-to-materialized-head timeline=" + timeline_hash.ToString() +
          " cursor=" + std::to_string(applied_cursor) +
          " params=" + ComputePipelineParameterHash(head_pipeline_params).ToString());
  EditorJournalBatchCommitPayload payload;
  payload.previous_batch_commit_sequence = 0;
  payload.first_covered_sequence         = 1;
  payload.last_covered_sequence          = checkpoint_seq;
  payload.last_operation_sequence        = 0;
  payload.record_chain_hash = ComputeEditorJournalRecordChainHash(
      compact_journal.DecodeRecordChain().records, checkpoint_seq);
  const auto commit_seq = compact_journal.AppendJournalBatchCommit(identity, payload);
  (void)commit_seq;

  // Verify the compact body before touching the active file.
  const auto decoded = compact_journal.DecodeRecordChain();
  if (decoded.stopped_on_corrupt_record || decoded.records.size() != 2 ||
      decoded.records[0].record_type != EditorJournalRecordType::CompactionCheckpoint ||
      decoded.records[1].record_type != EditorJournalRecordType::JournalBatchCommit ||
      !IsValidBatchCommit(decoded.records, decoded.records[1], 0)) {
    SetError(decoded.message.empty() ? "compact journal failed verification" : decoded.message);
    if (error) {
      *error = last_error_;
    }
    return false;
  }
  JournalTimelineSimulator verify(identity);
  const auto replay = verify.ReplayCommittedRecordChain(compact_journal);
  if (replay.status != EditorJournalApplyStatus::Applied) {
    SetError(replay.message.empty() ? "compact journal failed verification" : replay.message);
    if (error) {
      *error = last_error_;
    }
    return false;
  }

  std::string io_error;
  if (!file_->CreateExclusive(compact_path, compact_journal.bytes().data(),
                              compact_journal.bytes().size(), &io_error)) {
    SetError(io_error.empty() ? "failed to create compact journal" : io_error);
    if (error) {
      *error = last_error_;
    }
    return false;
  }
  if (!file_->AtomicReplace(compact_path, active_path, &io_error)) {
    SetError(io_error.empty() ? "failed to replace active journal" : io_error);
    if (error) {
      *error = last_error_;
    }
    return false;
  }
  const auto directory =
      active_path.has_parent_path() ? active_path.parent_path() : std::filesystem::path(".");
  if (!file_->FlushDirectory(directory, &io_error)) {
    SetError(io_error.empty() ? "failed to flush journal directory" : io_error);
    if (error) {
      *error = last_error_;
    }
    return false;
  }

  if (!journal_->LoadBytes(compact_journal.bytes(), &io_error)) {
    SetError(io_error.empty() ? "failed to reload compact journal" : io_error);
    if (error) {
      *error = last_error_;
    }
    return false;
  }
  identity_ = identity;
  InitializeStateFromJournal();
  last_error_.clear();
  return true;
}

auto EditorJournalWriter::EmitDiagnosticBundle(const std::filesystem::path& journal_path,
                                               const std::string& reason, std::string* error)
    -> std::optional<std::filesystem::path> {
  std::scoped_lock lock(mutex_);
  return WriteEditorJournalDiagnosticBundle(journal_path, journal_->bytes(), reason, error);
}

}  // namespace alcedo
