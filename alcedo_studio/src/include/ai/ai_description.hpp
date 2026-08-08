//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <vector>

#include "type/type.hpp"  // sl_element_id_t = uint32_t

namespace alcedo {

// The image's own AI-generated *understanding* — the caption, tags, and scene/category
// hints a remote LLM produces from a rendered rendition of the image (Phase 5d/5e
// image-analysis sidecar). This is a standalone domain object: it lives in its own
// `ai/` module and is deliberately NOT a member of `Image` or `SleeveFile`. Its foreign
// key is `file_id`, the Sleeve element id / inode — the same key the CLIP embeddings
// bind to (not the image id), so deleting a file cascades cleanly and a re-import under
// a new image id still recovers the prior understanding.
//
// `task_id` distinguishes analysis runs (e.g. a prompt-profile version or a provider
// re-evaluation). At most one row per `(file_id, task_id)` is active-for-search; the
// `AiStore` upserts on the table's `PRIMARY KEY (file_id, task_id)` so a new
// active persist replaces the prior row for that pair, and a row whose `active_` is
// false is kept for history but excluded from full-text search.
//
// DB serialization/deserialization goes through the duckorm layer in `src/storage/`
// (`AiStore`); this header stays free of storage / ORM includes. `tags_` is
// stored as a JSON array string (e.g. `["sahara","dunes"]`) so the ORM binds a single
// VARCHAR; `Tags()` / `SetTags()` round-trip it to `std::vector<std::string>` for app use.
struct AiDescription {
  sl_element_id_t file_id_           = 0;    // FK -> Sleeve element id (inode)
  std::string    task_id_           {};      // analysis task identity (prompt-profile run, etc.)
  std::string    provider_id_       {};      // e.g. "openrouter", "volcengine_ark"
  std::string    model_id_          {};      // remote model that produced this understanding
  std::string    prompt_profile_id_ {};      // prompt/profile identity (rubric-change guard)
  std::string    rendition_kind_    {};      // selected rendition, e.g. "thumbnail_k1024"
  std::string    caption_           {};      // free-form LLM caption
  std::string    tags_json_         {};      // JSON array string; "" = no tags
  std::string    scene_             {};      // scene / category hint
  double         confidence_        = 0.0;   // describe-task confidence (0..1)
  bool           active_            = true;  // active-for-search flag

  // Parse `tags_json_` into a tag list. Returns an empty list when `tags_json_` is empty
  // or not a JSON string array (a malformed store is treated as "no tags", not an error).
  [[nodiscard]] auto Tags() const -> std::vector<std::string>;
  // Serialize a tag list into `tags_json_` as a JSON array string.
  void SetTags(const std::vector<std::string>& tags);
  // Identity check used at the persistence boundary: a description is storable only when
  // its file key and provider/model identity are present. Caption/tags/scene may be empty.
  [[nodiscard]] auto IsValid() const -> bool;
};

}  // namespace alcedo