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
#include "type/type.hpp"

namespace alcedo {
class EditorJournalWriter;
}

namespace alcedo::ui {

/// Owns per-image journal writers, image locks, append operations, and the
/// durability barrier. It does not materialize history, recover storage,
/// invalidate thumbnails, or publish tasks.
class EditorSessionJournalWriterPort final : public alcedo::IEditorJournalPort {
 public:
  struct Services {
    /// Resolve the per-image append-only journal path.
    std::function<std::filesystem::path(sl_element_id_t)> journal_path;
  };

  /// Construct a writer port with optional path resolution.
  explicit EditorSessionJournalWriterPort(Services services = {});
  /// Flush pending writer workers before releasing resources.
  ~EditorSessionJournalWriterPort() override;

  EditorSessionJournalWriterPort(const EditorSessionJournalWriterPort&)            = delete;
  EditorSessionJournalWriterPort& operator=(const EditorSessionJournalWriterPort&) = delete;

  /// Replace path resolution used by future journal operations.
  void                            SetServices(Services services);
  /// Finalize the currently open edit without performing durable I/O.
  auto FinalizeEdit(sl_element_id_t element_id, std::uint64_t session_generation,
                    std::string* error) -> bool override;
  /// Flush queued records for one image to durable journal storage.
  auto CommitJournal(sl_element_id_t element_id, std::uint64_t session_generation,
                     std::string* error) -> alcedo::EditorJournalCommitOutcome override;
  /// Schedule the journal durability barrier and report its terminal outcome.
  auto CommitJournalAsync(sl_element_id_t element_id, std::uint64_t session_generation,
                          alcedo::EditorJournalCommitCallback callback) -> bool override;
  /// Discard records that have not crossed the durability barrier.
  auto DiscardUnflushed(sl_element_id_t element_id, std::string* error) -> bool override;
 private:
  auto WriterFor(sl_element_id_t element_id, std::uint64_t session_generation, std::string* error)
      -> std::shared_ptr<alcedo::EditorJournalWriter>;
  auto               ImageLockFor(sl_element_id_t element_id) -> std::shared_ptr<std::mutex>;
  [[nodiscard]] auto HasJournalPathResolver() const -> bool;

  Services           services_{};
  mutable std::mutex mutex_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<alcedo::EditorJournalWriter>> writers_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<std::mutex>>                  image_locks_;
};

}  // namespace alcedo::ui
