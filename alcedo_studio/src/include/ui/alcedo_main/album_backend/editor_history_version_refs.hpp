//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <string>

#include "app/editor_session_ports.hpp"
#include "app/editor_session_types.hpp"

namespace alcedo::ui {

struct HistoryWorkingState;
class EditorHistoryState;

/// Extracted Version reference unit. Handles root Version creation,
/// selected-commit branching, rename, and removal.
class EditorHistoryVersionRefs {
 public:
  explicit EditorHistoryVersionRefs(EditorHistoryState& state);

  /// Create a named ref at the image root, set it active, rebuild the pipeline.
  auto CreateRootVersionAndCheckout(const alcedo::EditorHistoryGuardHandle& guard,
                                    std::string display_name,
                                    alcedo::version_ref_id_t* version_id, std::string* error)
      -> bool;

  /// Create a named ref at an explicit commit, set it active, rebuild the pipeline.
  auto BranchFromCommitAndCheckout(const alcedo::EditorHistoryGuardHandle& guard,
                                   const alcedo::commit_hash_t& commit_id,
                                   std::string display_name,
                                   alcedo::version_ref_id_t* version_id, std::string* error)
      -> bool;

  /// Rename a Version in the live graph.
  auto RenameVersion(const alcedo::EditorHistoryGuardHandle& guard,
                     const alcedo::Hash128& version_id, std::string display_name,
                     std::string* error) -> bool;

  /// Remove a Version from the live graph.
  auto RemoveVersion(const alcedo::EditorHistoryGuardHandle& guard,
                     const alcedo::Hash128& version_id, std::string* error) -> bool;

 private:
  EditorHistoryState& state_;
};

}  // namespace alcedo::ui
