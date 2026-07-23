//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_journal_writer_port.hpp"

#include <utility>

#include "edit/history/editor_journal_writer.hpp"

namespace alcedo::ui {

EditorSessionJournalWriterPort::EditorSessionJournalWriterPort(Services services)
    : services_(std::move(services)) {}

EditorSessionJournalWriterPort::~EditorSessionJournalWriterPort() = default;

void EditorSessionJournalWriterPort::SetServices(Services services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
}

auto EditorSessionJournalWriterPort::HasJournalPathResolver() const -> bool {
  std::scoped_lock lock(mutex_);
  return static_cast<bool>(services_.journal_path);
}

auto EditorSessionJournalWriterPort::ImageLockFor(sl_element_id_t element_id)
    -> std::shared_ptr<std::mutex> {
  std::scoped_lock lock(mutex_);
  auto             it = image_locks_.find(element_id);
  if (it != image_locks_.end()) return it->second;
  auto image_lock = std::make_shared<std::mutex>();
  image_locks_.emplace(element_id, image_lock);
  return image_lock;
}

auto EditorSessionJournalWriterPort::WriterFor(sl_element_id_t element_id,
                                               std::uint64_t session_generation, std::string* error)
    -> std::shared_ptr<alcedo::EditorJournalWriter> {
  std::scoped_lock lock(mutex_);
  if (!services_.journal_path) return nullptr;

  std::filesystem::path path;
  try {
    path = services_.journal_path(element_id);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return nullptr;
  } catch (...) {
    if (error) *error = "Failed to resolve editor journal path";
    return nullptr;
  }
  if (path.empty()) return nullptr;

  auto existing = writers_.find(element_id);
  if (existing != writers_.end() && existing->second->path() == path) {
    auto identity               = existing->second->identity();
    identity.session_generation = session_generation;
    if (!existing->second->SetIdentity(identity)) {
      if (error) *error = existing->second->last_error();
      return nullptr;
    }
    return existing->second;
  }
  if (existing != writers_.end()) writers_.erase(existing);

  try {
    const alcedo::EditorJournalIdentity identity{element_id, {}, session_generation, 1};
    auto writer = std::make_shared<alcedo::EditorJournalWriter>(identity, std::move(path));
    writers_.emplace(element_id, writer);
    return writer;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
  } catch (...) {
    if (error) *error = "Failed to open editor journal";
  }
  return nullptr;
}

auto EditorSessionJournalWriterPort::FinalizeEdit(sl_element_id_t /*element_id*/,
                                                  std::uint64_t /*session_generation*/,
                                                  std::string* /*error*/) -> bool {
  return true;
}

auto EditorSessionJournalWriterPort::CommitJournal(sl_element_id_t element_id,
                                                   std::uint64_t   session_generation,
                                                   std::string*    error)
    -> alcedo::EditorJournalCommitOutcome {
  if (!HasJournalPathResolver()) {
    if (error) *error = "Editor journal path is unavailable";
    return {false, false, false,
            0,     0,     error != nullptr ? *error : "Editor journal path is unavailable"};
  }
  const auto       image_lock = ImageLockFor(element_id);
  std::scoped_lock image_guard(*image_lock);
  auto             writer = WriterFor(element_id, session_generation, error);
  if (!writer) {
    return {false, false,
            false, 0,
            0,     error != nullptr && !error->empty() ? *error : "Editor journal unavailable"};
  }
  const auto result = writer->CommitQueued();
  return {result.accepted,
          result.durable,
          result.pending,
          result.batch_commit_sequence,
          result.durable_operation_sequence,
          result.error};
}

auto EditorSessionJournalWriterPort::CommitJournalAsync(
    sl_element_id_t element_id, std::uint64_t session_generation,
    alcedo::EditorJournalCommitCallback callback) -> bool {
  std::string error;
  auto        outcome = CommitJournal(element_id, session_generation, &error);
  if (outcome.error.empty()) outcome.error = std::move(error);
  if (callback) callback(std::move(outcome));
  return true;
}

auto EditorSessionJournalWriterPort::DiscardUnflushed(sl_element_id_t element_id,
                                                      std::string*    error) -> bool {
  const auto                                   image_lock = ImageLockFor(element_id);
  std::scoped_lock                             image_guard(*image_lock);
  std::shared_ptr<alcedo::EditorJournalWriter> writer;
  {
    std::scoped_lock lock(mutex_);
    auto             it = writers_.find(element_id);
    if (it == writers_.end()) return true;
    writer = it->second;
  }
  return writer->DiscardQueued(error);
}

auto EditorSessionJournalWriterPort::RecordEdit(sl_element_id_t                element_id,
                                                std::uint64_t                  session_generation,
                                                const alcedo::EditTransaction& transaction,
                                                std::string*                   error) -> bool {
  const auto       image_lock = ImageLockFor(element_id);
  std::scoped_lock image_guard(*image_lock);
  std::string      writer_error;
  auto             writer = WriterFor(element_id, session_generation, &writer_error);
  if (!writer) {
    if (error) *error = writer_error.empty() ? "Editor journal unavailable" : writer_error;
    return false;
  }
  const auto identity = writer->identity();
  if (writer->AppendEdit(identity, transaction) != 0) return true;
  if (error) *error = writer->last_error();
  return false;
}

auto EditorSessionJournalWriterPort::RecordCursorMove(sl_element_id_t element_id,
                                                      std::uint64_t   session_generation,
                                                      std::uint64_t   from_cursor,
                                                      std::uint64_t to_cursor, std::string* error)
    -> bool {
  const auto       image_lock = ImageLockFor(element_id);
  std::scoped_lock image_guard(*image_lock);
  std::string      writer_error;
  auto             writer = WriterFor(element_id, session_generation, &writer_error);
  if (!writer) {
    if (error) *error = writer_error.empty() ? "Editor journal unavailable" : writer_error;
    return false;
  }
  const auto identity = writer->identity();
  if (writer->AppendCursorMove(identity, from_cursor, to_cursor) != 0) return true;
  if (error) *error = writer->last_error();
  return false;
}

auto EditorSessionJournalWriterPort::RecordRewriteTimeline(
    sl_element_id_t element_id, std::uint64_t session_generation,
    const alcedo::Hash128& expected_timeline_hash, const alcedo::Hash128& discarded_tail_hash,
    std::uint64_t retained_cursor, const alcedo::EditTransaction& replacement, std::string* error)
    -> bool {
  const auto       image_lock = ImageLockFor(element_id);
  std::scoped_lock image_guard(*image_lock);
  std::string      writer_error;
  auto             writer = WriterFor(element_id, session_generation, &writer_error);
  if (!writer) {
    if (error) *error = writer_error.empty() ? "Editor journal unavailable" : writer_error;
    return false;
  }
  const auto identity = writer->identity();
  if (writer->AppendRewriteTimeline(identity, expected_timeline_hash, discarded_tail_hash,
                                    retained_cursor, replacement) != 0) {
    return true;
  }
  if (error) *error = writer->last_error();
  return false;
}

}  // namespace alcedo::ui
