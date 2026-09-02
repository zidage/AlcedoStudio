//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "edit/history/pipeline_history_format.hpp"
#include "type/hash_type.hpp"
#include "type/type.hpp"

namespace alcedo {

// =============================================================================
// Pipeline vs edit-history identity (binding)
//
// Final product semantics. Do not reintroduce a second "pipeline head", dual head
// caches, or per-SetOperator chain-hash steps. Roadmap authority:
// docs/roadmap/alcedo_studio/ui/editor_single_live_pipeline_wal_checkpoint_plan.md
// section "Final locked identity model".
//
// 1. Edit history owns HEAD.
//    - CommitGraph + VersionRef.head_commit_hash is the only working tip.
//    - Switching Version, undo, redo, and paste move history head only.
//    - PipelineGuard must not store a parallel working_head field.
//
// 2. PipelineDocument is the live DAG (plus executor).
//    - Live mutations apply typed batches to the one writable document.
//    - DuckDB serialized checkpoint JSON is that document labeled with the
//      history tip, not a second editable universe and not a second head.
//
// 3. transaction_chain_hash labels "which first-parent history tip this document
//    claims to match".
//    - Unit of advancement is one commit (or one head-move to an existing tip):
//        root_chain = H(chain_format_version, root_id)
//        next_chain = H(previous_chain, commit_hash)   // exactly once per commit
//    - Checkpoint / ImageEditState store (root, head, chain, document) together
//      so load can compare history tip ↔ checkpoint label. Match => install the
//      document (skip first-parent replay). Mismatch => history wins; rebuild
//      from root + first-parent typed batches, then write a fresh checkpoint.
//
// 4. Head is not "same after version change".
//    - "One head source of truth" means one place that knows the tip (history),
//      not that the tip value stays constant across Version switches.
// =============================================================================

/// Content-addressed identity of one immutable edit commit.
using commit_hash_t = Hash128;

/// Immutable identity of the image-specific root pipeline after import metadata resolution.
using root_id_t = Hash128;

/// Stable branch identity for a named Version ref. Independent of the current head commit.
using version_ref_id_t = Hash128;

/// History tip for a Version (working or materialized). std::nullopt = image root.
/// Owned only by edit history (VersionRef / CommitGraph), never by the pipeline executor.
using head_commit_hash_t = std::optional<commit_hash_t>;

/// First-parent chain fold from the image root to a given head.
/// Advances once per commit on that chain.
/// Used as the checkpoint label that pairs the saved document with a history tip.
using transaction_chain_hash_t = Hash128;


/// Encode a nullable head as a storage string. Empty means root.
inline auto HeadCommitHashToStorage(const head_commit_hash_t& head) -> std::string {
  return head.has_value() ? head->ToString() : std::string{};
}

inline auto HeadCommitHashFromStorage(const std::string& value) -> head_commit_hash_t {
  if (value.empty()) {
    return std::nullopt;
  }
  return Hash128::FromString(value);
}

}  // namespace alcedo
