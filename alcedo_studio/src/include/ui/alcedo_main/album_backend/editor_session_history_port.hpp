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

#include "app/adjustment_transfer_types.hpp"
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
  /// Switch the checked-out Version, rebuild the live pipeline, refresh the snapshot.
  auto CheckoutVersion(const alcedo::EditorHistoryGuardHandle& guard,
                       const alcedo::Hash128& version_id, std::string* error) -> bool override;
  /// Project the named refs and active first-parent path for the QML model.
  auto ReadHistorySnapshot(const alcedo::EditorHistoryGuardHandle& guard,
                           alcedo::EditorHistorySnapshot* snapshot, std::string* error)
      -> bool override;
  /// Phase 7A: create a named ref at the image root, set it active, rebuild the
  /// pipeline, clear redo, and publish the clean root snapshot.
  auto CreateRootVersionAndCheckout(const alcedo::EditorHistoryGuardHandle& guard,
                                     std::string display_name,
                                     alcedo::version_ref_id_t* version_id,
                                     std::string* error) -> bool override;
  /// Phase 7A: create a named ref at an explicit commit, set it active,
  /// rebuild the pipeline, clear redo, and publish the matching snapshot.
  auto BranchFromCommitAndCheckout(const alcedo::EditorHistoryGuardHandle&  guard,
                                   const alcedo::commit_hash_t&            commit_id,
                                   std::string                              display_name,
                                   alcedo::version_ref_id_t*                version_id,
                                   std::string*                            error) -> bool override;
  auto RenameVersion(const alcedo::EditorHistoryGuardHandle& guard,
                     const alcedo::Hash128& version_id, std::string display_name,
                     std::string* error) -> bool override;
  auto RemoveVersion(const alcedo::EditorHistoryGuardHandle& guard,
                     const alcedo::Hash128& version_id, std::string* error) -> bool override;
  /// Apply one copied package through the editor's checked-out graph.
  auto PasteAdjustments(const alcedo::EditorHistoryGuardHandle&  guard,
                        const alcedo::AdjustmentTransferPackage& package,
                        std::string version_display_name, alcedo::AdjustmentPasteResult* result,
                        std::string* error) -> bool override;
  auto BeginMerge(const alcedo::EditorHistoryGuardHandle&  guard,
                  const alcedo::AdjustmentTransferPackage& package,
                  std::string                              incoming_version_display_name,
                  alcedo::AdjustmentMergePreview* preview, std::string* error) -> bool override;
  auto CompleteMerge(const alcedo::EditorHistoryGuardHandle&               guard,
                     const alcedo::AdjustmentMergePreview&                 preview,
                     const std::vector<alcedo::AdjustmentMergeResolution>& resolutions,
                     alcedo::AdjustmentMergeResult* result, std::string* error) -> bool override;
  auto CancelMerge(const alcedo::EditorHistoryGuardHandle& guard,
                   const alcedo::AdjustmentMergePreview& preview, std::string* error)
      -> bool override;
  /// Return the committed adjustment snapshot for rendering and the UI.
  auto ReadAdjustmentSnapshot(const alcedo::EditorHistoryGuardHandle& guard,
                              alcedo::EditorRenderAdjustmentSnapshot* snapshot, std::string* error)
      -> bool override;
  /// Return an immutable save capture with all records needed by the store.
  auto CaptureSaveCheckpoint(const alcedo::EditorHistoryGuardHandle& guard, std::string* error)
      -> std::shared_ptr<const alcedo::EditorMiniGitSaveCapture> override;
  /// Truncate the live Mini-Git journal through last_sequence after a successful
  /// materialize so same-session captures no longer include that prefix.
  auto DiscardMaterializedJournalThrough(const alcedo::EditorHistoryGuardHandle& guard,
                                         std::uint64_t last_sequence, std::string* error)
      -> bool override;

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
