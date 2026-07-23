//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "app/editor_session_ports.hpp"
#include "edit/history/editor_journal_writer.hpp"
#include "type/type.hpp"

namespace alcedo::ui {

/// Production journal writer port. Owns per-image journal look-up, image-scoped
/// mutexes, finalize/append/head-move/discard methods, and durability commit.
/// Performs no DuckDB materialization, recovery, thumbnail invalidation, or
/// QML task publication.
///
/// Thread context: edit finalization is synchronous on the caller thread;
/// CommitJournalAsync spawns a background jthread.
class EditorSessionProductionJournalWriterPort final : public alcedo::IEditorJournalPort {
 public:
  struct Services {
    std::function<std::filesystem::path(sl_element_id_t)> journal_path;
  };

  explicit EditorSessionProductionJournalWriterPort(Services services = {});
  ~EditorSessionProductionJournalWriterPort() override;

  EditorSessionProductionJournalWriterPort(const EditorSessionProductionJournalWriterPort&) =
      delete;
  EditorSessionProductionJournalWriterPort& operator=(
      const EditorSessionProductionJournalWriterPort&) = delete;

  void SetServices(Services services);

  auto FinalizeEdit(sl_element_id_t element_id, std::uint64_t session_generation,
                    std::string* error) -> bool override;
  auto CommitJournal(sl_element_id_t element_id, std::uint64_t session_generation,
                     std::string* error) -> alcedo::EditorJournalCommitOutcome override;
  auto CommitJournalAsync(sl_element_id_t element_id, std::uint64_t session_generation,
                          alcedo::EditorJournalCommitCallback callback) -> bool override;
  auto DiscardUnflushed(sl_element_id_t element_id, std::string* error) -> bool override;

  auto RecordEdit(sl_element_id_t element_id, std::uint64_t session_generation,
                  const alcedo::EditTransaction& transaction, std::string* error) -> bool override;
  auto RecordCursorMove(sl_element_id_t element_id, std::uint64_t session_generation,
                        std::uint64_t from_cursor, std::uint64_t to_cursor, std::string* error)
      -> bool override;
  auto RecordRewriteTimeline(sl_element_id_t element_id, std::uint64_t session_generation,
                             const alcedo::Hash128&         expected_timeline_hash,
                             const alcedo::Hash128&         discarded_tail_hash,
                             std::uint64_t                  retained_cursor,
                             const alcedo::EditTransaction& replacement, std::string* error)
      -> bool override;

 private:
  auto WriterFor(sl_element_id_t element_id, std::uint64_t session_generation, std::string* error)
      -> std::shared_ptr<alcedo::EditorJournalWriter>;
  auto               ImageLockFor(sl_element_id_t element_id) -> std::shared_ptr<std::mutex>;
  [[nodiscard]] auto HasJournalPathResolver() const -> bool;

  Services           services_{};
  mutable std::mutex mutex_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<alcedo::EditorJournalWriter>> writers_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<std::mutex>>                  image_locks_;
  std::vector<unsigned char>                                                        dummy_;
  bool shutting_down_ = false;
};

}  // namespace alcedo::ui
