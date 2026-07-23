//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_checkpoint_store.hpp"

#include <utility>

#include "app/editor_mini_git_materializer.hpp"
#include "app/editor_save_checkpoint_coordinator.hpp"

namespace alcedo::ui {

EditorSessionCheckpointStore::~EditorSessionCheckpointStore() {
  std::vector<std::jthread> workers;
  {
    std::scoped_lock lock(mutex_);
    shutting_down_ = true;
    workers.swap(workers_);
  }
  workers.clear();
}

void EditorSessionCheckpointStore::SetServices(Services services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
  materializer_.reset();
  materializer_storage_.reset();
}

auto EditorSessionCheckpointStore::EnsureMaterializer()
    -> std::shared_ptr<alcedo::EditorMiniGitMaterializer> {
  std::scoped_lock lock(mutex_);
  if (!services_.storage_service) return nullptr;
  if (!services_.save_coordinator) return nullptr;
  auto storage = services_.storage_service();
  if (!storage) return nullptr;
  if (materializer_ && materializer_storage_ == storage) return materializer_;
  try {
    materializer_storage_ = storage;
    materializer_         = std::make_shared<alcedo::EditorMiniGitMaterializer>(
        std::move(storage), services_.save_coordinator);
  } catch (...) {
    materializer_.reset();
    materializer_storage_.reset();
  }
  return materializer_;
}

auto EditorSessionCheckpointStore::ResolveJournalPath(sl_element_id_t element_id,
                                                      std::string* error) -> std::filesystem::path {
  std::function<std::filesystem::path(sl_element_id_t)> resolver;
  {
    std::scoped_lock lock(mutex_);
    resolver = services_.mini_git_journal_path;
  }
  if (!resolver) return {};
  try {
    return resolver(element_id);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
  } catch (...) {
    if (error) *error = "Failed to resolve Mini-Git journal path";
  }
  return {};
}

auto EditorSessionCheckpointStore::Materialize(
    std::shared_ptr<const alcedo::EditorMiniGitSaveCapture> capture, std::string* error)
    -> alcedo::EditorMaterializeOutcome {
  if (!capture) return {true, true, 0, {}};
  auto materializer = EnsureMaterializer();
  if (!materializer) {
    if (error) *error = "Mini-Git storage is unavailable";
    return {false, false, 0, error != nullptr ? *error : "Mini-Git storage is unavailable"};
  }
  const auto result = materializer->Materialize(*capture, error);
  return {result.accepted, result.materialized, result.materialized ? 1u : 0u, result.error};
}

auto EditorSessionCheckpointStore::MaterializeAsync(
    std::shared_ptr<const alcedo::EditorMiniGitSaveCapture> capture,
    alcedo::EditorMaterializeCallback                       callback) -> bool {
  std::scoped_lock lock(mutex_);
  if (shutting_down_) return false;
  workers_.emplace_back(
      [this, capture = std::move(capture), callback = std::move(callback)]() mutable {
        std::string error;
        auto        outcome = Materialize(capture, &error);
        if (outcome.error.empty()) outcome.error = std::move(error);
        if (callback) callback(std::move(outcome));
      });
  return true;
}

auto EditorSessionCheckpointStore::RecoverAndMaterialize(sl_element_id_t element_id,
                                                         std::uint64_t /*session_generation*/,
                                                         std::string* error)
    -> alcedo::EditorMaterializeOutcome {
  auto journal_path = ResolveJournalPath(element_id, error);
  if (journal_path.empty()) {
    if (error && error->empty()) *error = "Mini-Git journal path is unavailable";
    return {false, false, 0, error != nullptr ? *error : "Mini-Git journal path is unavailable"};
  }
  auto materializer = EnsureMaterializer();
  if (!materializer) {
    if (error && error->empty()) *error = "Mini-Git storage is unavailable";
    return {false, false, 0, error != nullptr ? *error : "Mini-Git storage is unavailable"};
  }
  const auto result = materializer->RecoverAndMaterialize(element_id, journal_path, error);
  return {result.accepted, result.materialized, result.materialized ? 1u : 0u, result.error};
}

}  // namespace alcedo::ui
