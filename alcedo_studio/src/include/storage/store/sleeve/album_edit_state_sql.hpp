//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SQL meaning of "edited" and "edit time" for library reads. Used by ElementStore and the album
// query SQL only. Every read that asks whether a photo is edited uses these fragments, so the
// library groups, the edit-time sort, the project overview count, and the last edited photo
// agree.

#pragma once

#include <string>
#include <string_view>

#include "storage/mapper/duckorm/duckdb_expr.hpp"

namespace alcedo::album_edit_state_sql {

/**
 * @brief Join chain from each image edit state to the commit at the head of its active Version.
 *
 * @details Aliases: `ies` (ImageEditState), `vr` (VersionRef), `hc` (EditCommit). A Version
 * whose head is the image root stores an empty head hash, which matches no commit, so only
 * edited photos have a row. Every join uses a primary key (`element_id`, `version_id`,
 * `commit_hash`). Commits that are no longer reachable from the head (undone edits) are not
 * read.
 */
inline constexpr const char* kActiveHeadCommitJoin =
    "ImageEditState ies "
    "JOIN VersionRef vr ON vr.version_id = ies.active_version_id "
    "JOIN EditCommit hc ON hc.commit_hash = NULLIF(vr.head_commit_hash, '')";

/// `SELECT ies.element_id FROM <kActiveHeadCommitJoin>`: the file ids of edited photos.
[[nodiscard]] auto EditedFileIdsQuery() -> std::string;

/**
 * @brief `edit_state AS (...)`: one row per edited file of @p match_set_table with columns
 *        `file_id`, `edited_at_ns` (head commit time, UTC epoch nanoseconds), and `edit_day`.
 *
 * @param time_zone IANA zone of `edit_day`. Empty yields a NULL DATE.
 * @param match_set_table Table whose `file_id` column limits the relation.
 * @return The definition without `WITH`, for a common table expression list.
 */
[[nodiscard]] auto EditStateRelation(const std::string& time_zone, std::string_view match_set_table)
    -> duckorm::SqlFragment;

}  // namespace alcedo::album_edit_state_sql
