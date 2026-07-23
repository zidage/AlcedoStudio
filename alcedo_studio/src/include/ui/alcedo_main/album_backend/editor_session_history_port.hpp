//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "app/editor_session_ports.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {

/// Owns live Mini-Git working history for each acquired image: adjustment
/// capture/commit, undo/redo, recovery replay, and immutable checkpoint capture.
/// The returned capture is transferred to the save service; no deferred map is
/// retained for another component to consume.
class EditorSessionHistoryPort final : public alcedo::IEditorHistoryPort {
 public:
  struct Services {
    /// Resolve the per-image Mini-Git journal path.
    std::function<std::filesystem::path(sl_element_id_t)> mini_git_journal_path;
  };

  /// Replace path resolution used by future history acquisitions.
  void SetServices(Services services);
  /// Set the pipeline port that owns the live editor guard.
  void SetPipelinePort(std::shared_ptr<EditorSessionPipelinePort> pipeline_port);

  /// Load or create the working history for one image.
  auto Acquire(sl_element_id_t element_id, std::string* error)
      -> alcedo::EditorHistoryGuardHandle override;
  /// Drop the working history state for one image.
  void Release(const alcedo::EditorHistoryGuardHandle& guard) override;
  /// Capture the committed operator value before interactive preview begins.
  auto CaptureAdjustmentBeforePreview(const alcedo::EditorHistoryGuardHandle& guard,
                                      const alcedo::EditorAdjustmentPatch&    patch,
                                      std::string* error) -> bool override;
  /// Append one settled adjustment and advance the live working head.
  auto CommitAdjustment(const alcedo::EditorHistoryGuardHandle& guard,
                        const alcedo::EditorAdjustmentPatch& patch, std::string* error)
      -> bool override;
  /// Move the working head to its first parent and apply the before value.
  auto Undo(const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool override;
  /// Move the working head to the redo child and apply the after value.
  auto Redo(const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool override;
  /// Return the committed adjustment snapshot for rendering and the UI.
  auto ReadAdjustmentSnapshot(const alcedo::EditorHistoryGuardHandle& guard,
                              alcedo::EditorRenderAdjustmentSnapshot* snapshot, std::string* error)
      -> bool override;
  /// Return an immutable save capture with all records needed by the store.
  auto CaptureSaveCheckpoint(const alcedo::EditorHistoryGuardHandle& guard, std::string* error)
      -> std::shared_ptr<const alcedo::EditorMiniGitSaveCapture> override;

 private:
  struct WorkingState;
  auto EnsureWorkingState(sl_element_id_t element_id, std::string* error)
      -> std::shared_ptr<WorkingState>;

  Services                                                           services_{};
  mutable std::mutex                                                 mutex_;
  std::weak_ptr<EditorSessionPipelinePort>                           pipeline_port_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<WorkingState>> working_states_;
};

}  // namespace alcedo::ui
