//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include "edit/history/commit_types.hpp"

namespace alcedo {

/// Read-only projection of one named Version ref for the editor history panel.
/// The ref identity is stable while its head may advance.
struct EditorHistoryVersion {
  version_ref_id_t   version_id{};
  std::string        display_name;
  head_commit_hash_t head_commit_hash = std::nullopt;
  std::time_t        created_at       = 0;
  std::time_t        updated_at       = 0;
  bool               active           = false;
};

/// Read-only projection of one immutable commit on the active Version's
/// first-parent path. A merge row keeps its ordered second parent visible.
struct EditorHistoryCommit {
  commit_hash_t                commit_hash{};
  head_commit_hash_t           first_parent_hash = std::nullopt;
  std::optional<commit_hash_t> second_parent_hash;
  EditCommitKind               kind          = EditCommitKind::kEdit;
  std::uint64_t                created_at_ns = 0;
  std::string                  label;
  std::string                  field_key;
  bool                         current = false;
};

/// Stable, typed history projection consumed by QML models. Recovery journal
/// records are deliberately absent; only immutable graph commits are exposed.
struct EditorHistorySnapshot {
  version_ref_id_t                  active_version_id{};
  head_commit_hash_t                active_head = std::nullopt;
  std::vector<EditorHistoryVersion> versions;
  std::vector<EditorHistoryCommit>  commits;
  bool                              recovered_head = false;
  bool                              can_undo       = false;
  bool                              can_redo       = false;
};

}  // namespace alcedo
