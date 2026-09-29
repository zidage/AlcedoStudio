//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

#include "app/editor_history_types.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"

namespace alcedo::ui {

/// Extract the resolved field key from an edit commit.
auto CommitFieldKey(const alcedo::EditCommit& commit) -> std::string;

/// Build a history presentation row from an edit commit.
auto CommitRowFromEdit(const alcedo::EditCommit& commit,
                       alcedo::EditorHistoryTimelinePosition position)
    -> alcedo::EditorHistoryCommit;

/// Check whether a Version display name already exists in the graph.
auto VersionNameExists(const alcedo::CommitGraph& graph, const std::string& name,
                       const alcedo::version_ref_id_t* ignored = nullptr) -> bool;

/// Return a unique Version display name based on a requested name.
auto UniqueVersionName(const alcedo::CommitGraph& graph, std::string requested,
                       const alcedo::version_ref_id_t* ignored = nullptr) -> std::string;

}  // namespace alcedo::ui
