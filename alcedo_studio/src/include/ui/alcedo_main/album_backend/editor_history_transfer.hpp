//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "app/editor_session_ports.hpp"

namespace alcedo::ui {

struct HistoryWorkingState;
class EditorHistoryState;

/// Transfer unit: live-pipeline paste and merge. History records WAL + commits;
/// the single session pipeline is the only mutation target.
class EditorHistoryTransfer {
 public:
  explicit EditorHistoryTransfer(EditorHistoryState& state);

  /// Cancel a live merge preview. BeginLiveMerge does not stage graph state, so
  /// this is a successful no-op for the history port (session clears its package).
  auto CancelMerge(const alcedo::EditorHistoryGuardHandle& guard,
                   const alcedo::AdjustmentMergePreview& preview, std::string* error) -> bool;

  /// Paste onto the live CommitGraph + WAL, then apply operators to the live
  /// pipeline executor.
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

 private:
  EditorHistoryState& state_;
};

}  // namespace alcedo::ui
