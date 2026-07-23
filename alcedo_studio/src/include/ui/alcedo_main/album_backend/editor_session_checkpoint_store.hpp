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
#include <thread>
#include <vector>

#include "app/editor_session_ports.hpp"
#include "type/type.hpp"

namespace alcedo {
class EditorMiniGitMaterializer;
class StorageService;
}  // namespace alcedo

namespace alcedo::ui {

/// Owns the Mini-Git materializer used by editor save checkpoints. It accepts
/// an immutable capture and owns only materializer/storage instances and its
/// worker lifetime; history, journal writers, task state, and thumbnail calls
/// remain in their respective ports.
class EditorSessionCheckpointStore final : public alcedo::IEditorCheckpointStore {
 public:
  struct Services {
    /// Resolve the storage service used for durable graph writes.
    std::function<std::shared_ptr<alcedo::StorageService>()> storage_service;
    /// Resolve the per-image journal path used during recovery.
    std::function<std::filesystem::path(sl_element_id_t)>    mini_git_journal_path;
  };

  /// Construct an unconfigured checkpoint store.
  EditorSessionCheckpointStore() = default;
  /// Stop and join all checkpoint workers.
  ~EditorSessionCheckpointStore() override;

  EditorSessionCheckpointStore(const EditorSessionCheckpointStore&)            = delete;
  EditorSessionCheckpointStore& operator=(const EditorSessionCheckpointStore&) = delete;

  /// Replace storage and recovery path dependencies.
  void                          SetServices(Services services);
  /// Materialize one immutable history capture synchronously.
  auto Materialize(std::shared_ptr<const alcedo::EditorMiniGitSaveCapture> capture,
                   std::string* error) -> alcedo::EditorMaterializeOutcome override;
  /// Materialize one immutable history capture on a worker.
  auto MaterializeAsync(std::shared_ptr<const alcedo::EditorMiniGitSaveCapture> capture,
                        alcedo::EditorMaterializeCallback callback) -> bool override;
  /// Recover a durable journal prefix for one image.
  auto RecoverAndMaterialize(sl_element_id_t element_id, std::uint64_t session_generation,
                             std::string* error) -> alcedo::EditorMaterializeOutcome override;

 private:
  auto EnsureMaterializer() -> std::shared_ptr<alcedo::EditorMiniGitMaterializer>;
  auto ResolveJournalPath(sl_element_id_t element_id, std::string* error) -> std::filesystem::path;

  Services                                           services_{};
  mutable std::mutex                                 mutex_;
  std::shared_ptr<alcedo::EditorMiniGitMaterializer> materializer_;
  std::shared_ptr<alcedo::StorageService>            materializer_storage_;
  std::vector<std::jthread>                          workers_;
  bool                                               shutting_down_ = false;
};

}  // namespace alcedo::ui
