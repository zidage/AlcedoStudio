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

  /// Apply one copied package through the editor's checked-out graph.
  auto PasteAdjustments(const alcedo::EditorHistoryGuardHandle& guard,
                        const alcedo::AdjustmentTransferPackage& package,
                        std::string version_display_name, alcedo::AdjustmentPasteResult* result,
                        std::string* error) -> bool;

  /// Begin a merge with an incoming adjustment package.
  auto BeginMerge(const alcedo::EditorHistoryGuardHandle& guard,
                  const alcedo::AdjustmentTransferPackage& package,
                  std::string incoming_version_display_name,
                  alcedo::AdjustmentMergePreview* preview, std::string* error) -> bool;

  /// Complete a merge with the given resolutions.
  auto CompleteMerge(const alcedo::EditorHistoryGuardHandle& guard,
                     const alcedo::AdjustmentMergePreview& preview,
                     const std::vector<alcedo::AdjustmentMergeResolution>& resolutions,
                     alcedo::AdjustmentMergeResult* result, std::string* error) -> bool;

  /// Cancel an active merge.
  auto CancelMerge(const alcedo::EditorHistoryGuardHandle& guard,
                   const alcedo::AdjustmentMergePreview& preview, std::string* error) -> bool;

 private:
  EditorHistoryState& state_;
};

}  // namespace alcedo::ui
