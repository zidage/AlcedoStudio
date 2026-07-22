//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>

#include "edit/history/commit_types.hpp"
#include "json.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Named branch/ref over the immutable commit graph.
 *
 * Identity is stable across head moves. The ref does not own a transaction array or cursor;
 * only head_commit_hash is mutable.
 */
struct VersionRef {
  version_ref_id_t  version_id{};
  sl_element_id_t   element_id = 0;
  std::string       display_name;
  head_commit_hash_t head_commit_hash = std::nullopt;
  std::time_t       created_at        = 0;
  std::time_t       updated_at        = 0;

  auto ToJSON() const -> nlohmann::json;
  static auto FromJSON(const nlohmann::json& j) -> VersionRef;
};

/**
 * @brief Per-image edit state coordinating the root, active Version, and stored projection.
 *
 * Production editing is not yet routed through this structure; it is the target schema for the
 * mini-Git history model.
 */
struct ImageEditState {
  sl_element_id_t            element_id = 0;
  root_id_t                  root_id{};
  version_ref_id_t           active_version_id{};
  head_commit_hash_t         materialized_head_commit_hash = std::nullopt;
  transaction_chain_hash_t   materialized_transaction_chain_hash{};
  std::optional<nlohmann::json> stored_pipeline_projection = std::nullopt;
  std::uint32_t              project_schema_version        = kImageEditSchemaVersion;

  auto ToJSON() const -> nlohmann::json;
  static auto FromJSON(const nlohmann::json& j) -> ImageEditState;
};

/// Create a fresh root identity and default Version ref for an image with no commits.
auto CreateEmptyImageEditState(sl_element_id_t element_id, std::string default_display_name = "Default")
    -> std::pair<ImageEditState, VersionRef>;

/// Move a Version ref head without changing its stable version_id.
void MoveVersionRefHead(VersionRef& version_ref, head_commit_hash_t new_head, std::time_t updated_at);

}  // namespace alcedo
