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

/// Timeline position of one commit row on the active Version's visible history.
/// `Applied` rows are ancestors of the working head, `Current` is the working
/// head itself (at most one row), and `Future` rows belong to the in-memory redo
/// suffix. At the image root (no commits) no row is `Current`.
enum class EditorHistoryTimelinePosition : std::uint8_t {
  Applied = 0,
  Current = 1,
  Future  = 2,
};

/// Read-only projection of one immutable commit on the active Version's
/// first-parent path or in-memory redo suffix. A merge row keeps its ordered
/// second parent and resolved field keys visible.
///
/// Typed pipeline batches fill owner identity, saved display names, and
/// localization data from the stored batch. Ordinary-edit rows still carry
/// before/after JSON for the presentation helper. The port never infers a
/// target from a live selection or a legacy stage name.
struct EditorHistoryCommit {
  commit_hash_t                commit_hash{};
  head_commit_hash_t           first_parent_hash = std::nullopt;
  std::optional<commit_hash_t> second_parent_hash;
  EditCommitKind               kind          = EditCommitKind::kEdit;
  std::uint64_t                created_at_ns = 0;
  /// Stable editor field key (e.g. "exposure"); "merge" for merge commits.
  std::string                  field_key;
  /// Ordinary-edit semantic payload, serialized JSON (the operator params
  /// before and after the settled edit). Empty for merge commits. Carried as a
  /// string so the projection header has no JSON dependency; the UI
  /// presentation helper parses it in a focused, testable module.
  std::string                  before_value_json;
  std::string                  after_value_json;
  bool                         before_enabled = false;
  bool                         after_enabled  = true;
  /// Ordered resolved field keys carried by a merge commit; empty for edits.
  std::vector<std::string>     merge_field_keys;
  EditorHistoryTimelinePosition position = EditorHistoryTimelinePosition::Applied;
  /// Saved typed-batch operation kind text. Empty for ordinary and merge rows.
  std::string                  operation_kind;
  /// Stable localization key from the typed batch. Empty for ordinary rows.
  std::string                  presentation_key;
  /// Localization arguments JSON object from the typed batch. Empty when unused.
  std::string                  presentation_args_json;
  std::string                  node_id;
  std::string                  node_display_name;
  std::string                  adjustment_instance_id;
  std::string                  mask_id;
  std::string                  mask_display_name;
};

/// Stable, typed history projection consumed by QML models. Recovery journal
/// records are deliberately absent; only immutable graph commits and the
/// in-memory redo suffix are exposed.
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
