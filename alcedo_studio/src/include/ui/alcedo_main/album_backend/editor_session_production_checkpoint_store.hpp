//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/editor_mini_git_materializer.hpp"
#include "app/editor_session_ports.hpp"
#include "sleeve/storage_service.hpp"
#include "type/type.hpp"

namespace alcedo {
class EditorMiniGitMaterializer;
class EditorHistoryMaterializer;
class EditHistory;
}  // namespace alcedo

namespace alcedo::ui {

class EditorSessionProductionThumbnailPort;

/// Production implementation of IEditorCheckpointStore. Accepts an immutable
/// capture and calls the Mini-Git materializer/recovery facade. Owns no live
/// history, QML tasks, or journal writer state.
///
/// Thread context: MaterializeAsync spawns background workers through jthread;
/// all public methods are safe to call from the session service thread.
class EditorSessionProductionCheckpointStore final : public alcedo::IEditorCheckpointStore {
 public:
  /// Services required to resolve storage and journal paths.
  struct Services {
    std::function<std::shared_ptr<alcedo::StorageService>()>                     storage_service;
    std::function<std::shared_ptr<alcedo::EditHistory>(sl_element_id_t)>         load_history;
    std::function<std::shared_ptr<alcedo::CPUPipelineExecutor>(sl_element_id_t)> load_pipeline;
    std::function<std::filesystem::path(sl_element_id_t)>                        journal_path;
    std::function<std::filesystem::path(sl_element_id_t)> mini_git_journal_path;
  };

  EditorSessionProductionCheckpointStore();
  ~EditorSessionProductionCheckpointStore() override;

  EditorSessionProductionCheckpointStore(const EditorSessionProductionCheckpointStore&) = delete;
  EditorSessionProductionCheckpointStore& operator=(const EditorSessionProductionCheckpointStore&) =
      delete;

  void SetServices(Services services);
  void SetThumbnailPort(std::shared_ptr<EditorSessionProductionThumbnailPort> thumbnail_port);

  auto Materialize(sl_element_id_t element_id, std::uint64_t session_generation, std::string* error)
      -> alcedo::EditorMaterializeOutcome override;
  auto MaterializeAsync(sl_element_id_t element_id, std::uint64_t session_generation,
                        alcedo::EditorMaterializeCallback callback) -> bool override;
  auto RecoverAndMaterialize(sl_element_id_t element_id, std::uint64_t session_generation,
                             std::string* error) -> alcedo::EditorMaterializeOutcome override;

 private:
  auto HasJournalPathResolver() const -> bool;
  auto HasMiniGitPathResolver() const -> bool;
  auto EnsureMaterializer() -> std::shared_ptr<alcedo::EditorHistoryMaterializer>;
  auto EnsureMiniGitMaterializer() -> std::shared_ptr<alcedo::EditorMiniGitMaterializer>;
  auto LoadHeadPipelineParams(sl_element_id_t                             element_id,
                              const std::shared_ptr<alcedo::EditHistory>& history)
      -> std::optional<nlohmann::json>;
  auto MaterializeMiniGit(sl_element_id_t element_id, std::string* error)
      -> alcedo::EditorMaterializeOutcome;
  auto RecoverMiniGit(sl_element_id_t element_id, std::string* error)
      -> alcedo::EditorMaterializeOutcome;

  Services                                              services_{};
  mutable std::mutex                                    mutex_;
  std::shared_ptr<alcedo::EditorHistoryMaterializer>    materializer_;
  std::shared_ptr<alcedo::StorageService>               materializer_storage_;
  std::shared_ptr<alcedo::EditorMiniGitMaterializer>    mini_git_materializer_;
  std::shared_ptr<alcedo::StorageService>               mini_git_materializer_storage_;
  std::shared_ptr<EditorSessionProductionThumbnailPort> thumbnail_port_;
  std::vector<std::jthread>                             workers_;
  bool                                                  shutting_down_ = false;
};

}  // namespace alcedo::ui
