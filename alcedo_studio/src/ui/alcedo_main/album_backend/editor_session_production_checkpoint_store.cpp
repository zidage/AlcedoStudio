//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_production_checkpoint_store.hpp"

#include <stdexcept>
#include <utility>

#include "app/editor_history_materializer.hpp"
#include "app/editor_session_ports.hpp"
#include "edit/history/editor_journal_writer.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "ui/alcedo_main/album_backend/editor_session_production_thumbnail_port.hpp"

namespace alcedo::ui {

// ── Construction / destruction ──────────────────────────────────────────────

EditorSessionProductionCheckpointStore::EditorSessionProductionCheckpointStore() = default;

EditorSessionProductionCheckpointStore::~EditorSessionProductionCheckpointStore() {
  std::vector<std::jthread> workers;
  {
    std::scoped_lock lock(mutex_);
    shutting_down_ = true;
    workers.swap(workers_);
  }
  workers.clear();
}

void EditorSessionProductionCheckpointStore::SetServices(Services services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
  materializer_.reset();
  materializer_storage_.reset();
  mini_git_materializer_.reset();
  mini_git_materializer_storage_.reset();
}

void EditorSessionProductionCheckpointStore::SetThumbnailPort(
    std::shared_ptr<EditorSessionProductionThumbnailPort> thumbnail_port) {
  std::scoped_lock lock(mutex_);
  thumbnail_port_ = std::move(thumbnail_port);
}

// ── Path resolver helpers ───────────────────────────────────────────────────

auto EditorSessionProductionCheckpointStore::HasJournalPathResolver() const -> bool {
  std::scoped_lock lock(mutex_);
  return static_cast<bool>(services_.journal_path);
}

auto EditorSessionProductionCheckpointStore::HasMiniGitPathResolver() const -> bool {
  std::scoped_lock lock(mutex_);
  return static_cast<bool>(services_.mini_git_journal_path);
}

// ── Materializer factories ──────────────────────────────────────────────────

auto EditorSessionProductionCheckpointStore::EnsureMaterializer()
    -> std::shared_ptr<alcedo::EditorHistoryMaterializer> {
  std::scoped_lock lock(mutex_);
  if (!services_.storage_service) return nullptr;
  auto storage = services_.storage_service();
  if (!storage) return nullptr;
  if (materializer_ && materializer_storage_ == storage) return materializer_;
  try {
    materializer_storage_ = storage;
    materializer_         = std::make_shared<alcedo::EditorHistoryMaterializer>(std::move(storage));
  } catch (...) {
    materializer_ = nullptr;
    materializer_storage_.reset();
  }
  return materializer_;
}

auto EditorSessionProductionCheckpointStore::EnsureMiniGitMaterializer()
    -> std::shared_ptr<alcedo::EditorMiniGitMaterializer> {
  std::scoped_lock lock(mutex_);
  if (!services_.storage_service) return nullptr;
  auto storage = services_.storage_service();
  if (!storage) return nullptr;
  if (mini_git_materializer_ && mini_git_materializer_storage_ == storage)
    return mini_git_materializer_;
  try {
    mini_git_materializer_storage_ = storage;
    mini_git_materializer_ =
        std::make_shared<alcedo::EditorMiniGitMaterializer>(std::move(storage));
  } catch (...) {
    mini_git_materializer_ = nullptr;
    mini_git_materializer_storage_.reset();
  }
  return mini_git_materializer_;
}

// ── Pipeline params ─────────────────────────────────────────────────────────

auto EditorSessionProductionCheckpointStore::LoadHeadPipelineParams(
    sl_element_id_t element_id, const std::shared_ptr<alcedo::EditHistory>& history)
    -> std::optional<nlohmann::json> {
  if (services_.load_pipeline) {
    if (auto pipeline = services_.load_pipeline(element_id)) {
      try {
        return pipeline->ExportPipelineParams();
      } catch (...) {
      }
    }
  }
  if (history) {
    if (auto materialized = history->GetActiveVersion().GetMaterializedParams()) {
      return materialized;
    }
    return history->GetImportPipelineParams();
  }
  return std::nullopt;
}

// ── Materialization ─────────────────────────────────────────────────────────

auto EditorSessionProductionCheckpointStore::Materialize(sl_element_id_t element_id,
                                                         std::uint64_t /*session_generation*/,
                                                         std::string* error)
    -> alcedo::EditorMaterializeOutcome {
  bool use_mini_git = HasMiniGitPathResolver();
  if (use_mini_git) {
    return MaterializeMiniGit(element_id, error);
  }
  // Legacy path: not extracted for Phase 1C-2 (kept in original journal port).
  if (!HasJournalPathResolver()) {
    return {true, true, 0, {}};
  }
  return {true, true, 0, {}};
}

auto EditorSessionProductionCheckpointStore::MaterializeAsync(
    sl_element_id_t element_id, std::uint64_t session_generation,
    alcedo::EditorMaterializeCallback callback) -> bool {
  std::scoped_lock lock(mutex_);
  if (shutting_down_) return false;
  std::jthread worker(
      [this, element_id, session_generation, callback = std::move(callback)]() mutable {
        std::string error;
        auto        outcome = Materialize(element_id, session_generation, &error);
        if (outcome.error.empty()) outcome.error = std::move(error);
        if (callback) callback(std::move(outcome));
      });
  workers_.push_back(std::move(worker));
  return true;
}

auto EditorSessionProductionCheckpointStore::RecoverAndMaterialize(
    sl_element_id_t element_id, std::uint64_t /*session_generation*/, std::string* error)
    -> alcedo::EditorMaterializeOutcome {
  bool use_mini_git = HasMiniGitPathResolver();
  if (use_mini_git) {
    return RecoverMiniGit(element_id, error);
  }
  if (!HasJournalPathResolver()) {
    return {true, true, 0, {}};
  }
  return {true, true, 0, {}};
}

// ── Mini-Git materialization ────────────────────────────────────────────────

auto EditorSessionProductionCheckpointStore::MaterializeMiniGit(sl_element_id_t element_id,
                                                                std::string*    error)
    -> alcedo::EditorMaterializeOutcome {
  // Without a history port to consume captures from, the empty-journal path
  // goes through recovery which handles empty or leftover journals.
  auto materializer = EnsureMiniGitMaterializer();
  if (!materializer) {
    return {true, true, 0, {}};
  }
  std::filesystem::path journal_path;
  {
    std::scoped_lock lock(mutex_);
    if (services_.mini_git_journal_path) {
      try {
        journal_path = services_.mini_git_journal_path(element_id);
      } catch (...) {
      }
    }
  }
  const auto recovered = materializer->RecoverAndMaterialize(element_id, journal_path, error);
  alcedo::EditorMaterializeOutcome outcome{recovered.accepted, recovered.materialized, 0,
                                           recovered.error};
  if (outcome.accepted && outcome.materialized) {
    if (thumbnail_port_) thumbnail_port_->Invalidate(element_id);
  }
  return outcome;
}

auto EditorSessionProductionCheckpointStore::RecoverMiniGit(sl_element_id_t element_id,
                                                            std::string*    error)
    -> alcedo::EditorMaterializeOutcome {
  std::filesystem::path journal_path;
  {
    std::scoped_lock lock(mutex_);
    if (!services_.mini_git_journal_path) {
      return {true, true, 0, {}};
    }
    try {
      journal_path = services_.mini_git_journal_path(element_id);
    } catch (const std::exception& ex) {
      return {false, false, 0, ex.what()};
    }
  }
  auto materializer = EnsureMiniGitMaterializer();
  if (!materializer) {
    return {true, true, 0, {}};
  }
  const auto result = materializer->RecoverAndMaterialize(element_id, journal_path, error);
  alcedo::EditorMaterializeOutcome outcome{result.accepted, result.materialized, 0, result.error};
  if (outcome.accepted && outcome.materialized) {
    if (thumbnail_port_) thumbnail_port_->Invalidate(element_id);
  }
  return outcome;
}

}  // namespace alcedo::ui
