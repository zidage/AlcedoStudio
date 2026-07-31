//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "app/editor_session_ports.hpp"

namespace alcedo::ui {

struct HistoryWorkingState;
class EditorHistoryState;

/// Extracted transfer unit. Handles Paste, Begin/Complete/Cancel Merge.
class EditorHistoryTransfer {
 public:
  explicit EditorHistoryTransfer(EditorHistoryState& state);

  /// Cancel an active merge preview / staged candidate.
  auto CancelMerge(const alcedo::EditorHistoryGuardHandle& guard,
                   const alcedo::AdjustmentMergePreview& preview, std::string* error) -> bool;

  /// Paste onto the live CommitGraph + WAL, then apply operators to the live
  /// pipeline executor. No shadow `HistoryTransferCandidate` graph is created.
  /// @param result On success carries new Version / head and `prior_version_id`
  ///   for CancelLivePaste.
  /// @pre Caller owns the editor session command queue for this element.
  auto PasteLiveRootRelativeVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                    const alcedo::AdjustmentTransferPackage& package,
                                    std::string version_display_name,
                                    alcedo::AdjustmentPasteResult* result, std::string* error)
      -> bool;

  /// Restore `prior_version_id`, remove the unused paste Version, and reinstall
  /// prior operator params on the live executor.
  auto CancelLivePaste(const alcedo::EditorHistoryGuardHandle& guard,
                       const alcedo::version_ref_id_t& prior_version_id,
                       const alcedo::version_ref_id_t& paste_version_id, std::string* error)
      -> bool;

  /// Detect merge conflicts from the live pipeline without mutating the graph.
  /// Does not create a shadow Version or `HistoryTransferCandidate`.
  auto BeginLiveMerge(const alcedo::EditorHistoryGuardHandle& guard,
                      const alcedo::AdjustmentTransferPackage& package,
                      alcedo::AdjustmentMergePreview* preview, std::string* error) -> bool;

  /// Apply merge resolutions to the live pipeline, append one merge commit + WAL,
  /// and regenerate the derived adjustment snapshot.
  auto CompleteLiveMerge(const alcedo::EditorHistoryGuardHandle& guard,
                         const alcedo::AdjustmentTransferPackage& package,
                         const alcedo::AdjustmentMergePreview& preview,
                         const std::vector<alcedo::AdjustmentMergeResolution>& resolutions,
                         alcedo::AdjustmentMergeResult* result, std::string* error) -> bool;

  /// Stage a Paste candidate without changing published history.
  /// @deprecated Prefer PasteLiveRootRelativeVersion; retained until merge rewrite (WU3/WU6).
  auto PreparePaste(const alcedo::EditorHistoryGuardHandle& guard,
                    const alcedo::AdjustmentTransferPackage& package,
                    std::string version_display_name, alcedo::AdjustmentPasteResult* result,
                    alcedo::EditorTransferCandidate* candidate, std::string* error) -> bool;

  /// Stage a Merge preview and candidate without changing published history.
  auto PrepareMerge(const alcedo::EditorHistoryGuardHandle& guard,
                    const alcedo::AdjustmentTransferPackage& package,
                    std::string incoming_version_display_name,
                    alcedo::AdjustmentMergePreview* preview,
                    alcedo::EditorTransferCandidate* candidate, std::string* error) -> bool;

  /// Validate the opaque preview against the live published base.
  auto ValidateMergeCandidate(const alcedo::EditorHistoryGuardHandle& guard,
                              const alcedo::AdjustmentMergePreview& preview,
                              const alcedo::EditorTransferCandidate& candidate,
                              std::string* error) -> bool;

  /// Apply Merge resolutions to the staged graph only.
  auto CompleteMergeCandidate(
      const alcedo::EditorHistoryGuardHandle& guard,
      const alcedo::AdjustmentMergePreview& preview,
      const std::vector<alcedo::AdjustmentMergeResolution>& resolutions,
      alcedo::EditorTransferCandidate* candidate, alcedo::AdjustmentMergeResult* result,
      std::string* error) -> bool;

  /// Capture one durable publication for a staged candidate.
  auto CaptureTransferSaveCheckpoint(
      const alcedo::EditorHistoryGuardHandle& guard,
      const alcedo::EditorTransferCandidate& candidate, std::string* error)
      -> std::shared_ptr<const alcedo::EditorMiniGitSaveCapture>;

  /// Publish a staged candidate after its durable capture has succeeded.
  auto PublishTransferCandidate(
      const alcedo::EditorHistoryGuardHandle& guard,
      const alcedo::EditorTransferCandidate& candidate,
      const alcedo::AdjustmentMergePreview* preview,
      const std::vector<alcedo::AdjustmentMergeResolution>& resolutions,
      alcedo::AdjustmentPasteResult* paste, alcedo::AdjustmentMergeResult* merge,
      std::string* error) -> bool;

  /// Drop a staged candidate without changing published history.
  auto DiscardTransferCandidate(const alcedo::EditorHistoryGuardHandle& guard,
                                const alcedo::EditorTransferCandidate& candidate,
                                std::string* error) -> bool;

 private:
  EditorHistoryState& state_;
};

}  // namespace alcedo::ui
