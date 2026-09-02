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

#include "app/adjustment_transfer_types.hpp"
#include "app/editor_session_ports.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/mask/mask_store.hpp"
#include "json.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {

class EditorHistoryState;
class EditorHistoryProjection;
class EditorHistoryMutation;
class EditorHistoryVersionRefs;
class EditorHistoryTransfer;
class EditorHistoryCheckpoint;
class EditorSessionPipelinePort;

/// Narrow façade implementing IEditorHistoryPort. Delegates all operations to
/// internal responsibility units (state, projection, mutation, version refs,
/// transfer, checkpoint). Does not contain Mini-Git traversal, payload
/// presentation, merge resolution, or checkpoint persistence algorithms.
class EditorSessionHistoryPort final : public alcedo::IEditorHistoryPort {
 public:
  struct Services {
    /// Resolve the per-image Mini-Git journal path.
    std::function<std::filesystem::path(sl_element_id_t)> mini_git_journal_path;
  };

  EditorSessionHistoryPort();
  ~EditorSessionHistoryPort() override;

  /// Replace path resolution used by future history acquisitions.
  void SetServices(Services services);
  /// Set the pipeline port that owns the live editor guard.
  void SetPipelinePort(std::shared_ptr<EditorSessionPipelinePort> pipeline_port);

  /// Load or create the working history for one image.
  auto Acquire(sl_element_id_t element_id, std::string* error)
      -> alcedo::EditorHistoryGuardHandle override;
  /// Drop the working history state for one image.
  void Release(const alcedo::EditorHistoryGuardHandle& guard) override;
  auto CaptureAdjustmentBeforePreview(const alcedo::EditorHistoryGuardHandle& guard,
                                      const alcedo::EditorAdjustmentPatch& patch,
                                      std::string* error) -> bool override;
  auto CommitAdjustment(const alcedo::EditorHistoryGuardHandle& guard,
                        const alcedo::EditorAdjustmentPatch& patch, std::string* error)
      -> bool override;
  auto CommitPipelineEditBatch(const alcedo::EditorHistoryGuardHandle& guard,
                               alcedo::PipelineEditBatch batch, std::string* error) -> bool;
  auto AddColorGrade(const alcedo::EditorHistoryGuardHandle& guard,
                     const alcedo::NodeId& before_node_id, const alcedo::NodeId& new_id,
                     std::string* error) -> bool;
  auto RemoveColorGrade(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                        std::string* error) -> bool;
  auto ReconnectColorGrade(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                           const alcedo::NodeId& new_predecessor_id,
                           const alcedo::NodeId& new_successor_id, std::string* error) -> bool;
  auto RenameColorGrade(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                        std::string display_name, std::string* error) -> bool;
  auto SetColorGradeEnabled(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                            bool enabled, std::string* error) -> bool;
  auto SetColorGradeMix(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                        float mix, std::string* error) -> bool;
  auto AddMask(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
               alcedo::MaskModel mask, std::uint32_t display_index, std::string* error) -> bool;
  auto RemoveMask(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                  const alcedo::MaskId& mask_id, std::string* error) -> bool;
  auto ReplaceMaskSource(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                         const alcedo::MaskId& mask_id, nlohmann::json after_source,
                         std::string* error) -> bool;
  auto ReplaceMaskAsset(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                        const alcedo::MaskId& mask_id, nlohmann::json after_source,
                        alcedo::MaskStore& mask_store, std::string* error) -> bool;
  auto SetMaskField(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                    const alcedo::MaskId& mask_id, std::string field_key, nlohmann::json after_value,
                    std::string* error) -> bool;
  auto Undo(const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool override;
  auto Redo(const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool override;
  [[nodiscard]] auto LastPublishedRenderReason() const
      -> std::optional<alcedo::EditorRenderReason> override;
  auto MoveHeadToCommit(const alcedo::EditorHistoryGuardHandle& guard,
                        const alcedo::commit_hash_t& commit_id, std::string* error)
      -> bool override;
  auto CheckoutVersion(const alcedo::EditorHistoryGuardHandle& guard,
                       const alcedo::Hash128& version_id, std::string* error) -> bool override;
  auto ReadHistorySnapshot(const alcedo::EditorHistoryGuardHandle& guard,
                           alcedo::EditorHistorySnapshot* snapshot, std::string* error)
      -> bool override;
  auto HasUnmaterializedChanges(const alcedo::EditorHistoryGuardHandle& guard,
                                std::string* error) -> bool override;
  auto DiscardUnmaterializedChanges(const alcedo::EditorHistoryGuardHandle& guard,
                                    std::string* error) -> bool override;
  auto CreateRootVersionAndCheckout(const alcedo::EditorHistoryGuardHandle& guard,
                                     std::string display_name,
                                     alcedo::version_ref_id_t* version_id,
                                     std::string* error) -> bool override;
  auto BranchFromCommitAndCheckout(const alcedo::EditorHistoryGuardHandle& guard,
                                   const alcedo::commit_hash_t& commit_id,
                                   std::string display_name,
                                   alcedo::version_ref_id_t* version_id,
                                   std::string* error) -> bool override;
  auto RenameVersion(const alcedo::EditorHistoryGuardHandle& guard,
                     const alcedo::Hash128& version_id, std::string display_name,
                     std::string* error) -> bool override;
  auto RemoveVersion(const alcedo::EditorHistoryGuardHandle& guard,
                     const alcedo::Hash128& version_id, std::string* error) -> bool override;
  auto PasteLiveRootRelativeVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                    const alcedo::AdjustmentTransferPackage& package,
                                    std::string version_display_name,
                                    alcedo::AdjustmentPasteResult* result, std::string* error)
      -> bool override;
  auto CancelLivePaste(const alcedo::EditorHistoryGuardHandle& guard,
                       const alcedo::version_ref_id_t& prior_version_id,
                       const alcedo::version_ref_id_t& paste_version_id, std::string* error)
      -> bool override;
  auto ReadAdjustmentSnapshot(const alcedo::EditorHistoryGuardHandle& guard,
                              alcedo::EditorRenderAdjustmentSnapshot* snapshot, std::string* error)
      -> bool override;
  auto CaptureSaveCheckpoint(const alcedo::EditorHistoryGuardHandle& guard, std::string* error)
      -> std::shared_ptr<const alcedo::EditorMiniGitSaveCapture> override;
  auto DiscardMaterializedJournalThrough(const alcedo::EditorHistoryGuardHandle& guard,
                                         std::uint64_t last_sequence, std::string* error)
      -> bool override;
  auto SyncMaterializedStateAfterCheckpoint(const alcedo::EditorHistoryGuardHandle& guard,
                                            std::string* error) -> bool override;

 private:
  std::unique_ptr<EditorHistoryState>        state_;
  std::unique_ptr<EditorHistoryProjection>   projection_;
  std::unique_ptr<EditorHistoryMutation>     mutation_;
  std::unique_ptr<EditorHistoryVersionRefs>  version_refs_;
  std::unique_ptr<EditorHistoryTransfer>     transfer_;
  std::unique_ptr<EditorHistoryCheckpoint>   checkpoint_;
  mutable std::mutex                         mutex_;
};

}  // namespace alcedo::ui
