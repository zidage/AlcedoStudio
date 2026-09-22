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

/// Transfer unit: live-pipeline Paste. History records WAL + one typed commit;
/// the single session pipeline is the only mutation target.
class EditorHistoryTransfer {
 public:
  explicit EditorHistoryTransfer(EditorHistoryState& state);

  /// Validate a transfer document, remap identities, apply one typed Paste batch
  /// to the live document from the immutable root, and publish one WAL commit.
  ///
  /// @param result On success carries the new Version, its head, and `prior_version_id`.
  /// @pre Caller owns the editor session command queue for this element.
  auto PasteLiveRootRelativeVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                    const alcedo::AdjustmentTransferPackage& package,
                                    std::string version_display_name,
                                    alcedo::AdjustmentPasteResult* result, std::string* error)
      -> bool;

 private:
  EditorHistoryState& state_;
};

}  // namespace alcedo::ui
