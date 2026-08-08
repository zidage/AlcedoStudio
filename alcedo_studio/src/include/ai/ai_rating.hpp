//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

#include "type/type.hpp"  // sl_element_id_t = uint32_t

namespace alcedo {

// The image's own AI-generated *rating* — a single 1..5 integer star rating a remote LLM
// produces from a rendered rendition (Phase 5d/5e image-analysis sidecar, `ScoreImage`).
// Like `AiDescription`, this is a standalone domain object in the `ai/` module, NOT a
// member of `Image` or `SleeveFile`, and its foreign key is `file_id` (the Sleeve element
// id / inode). The remote LLM contract (Phase 5f) requires a 1..5 integer and does NOT
// return a confidence, so this object carries no confidence field — unlike
// `AiDescription`, which still reports the describe-task confidence.
//
// The 1..5 range aligns with the EXIF-standard `Rating` the app already stores per file
// (`image/metadata.hpp`: 0..5 stars, 0 = unrated, integer storage). The host maps a 1..5
// AI rating onto that 0..5 field directly; 0 here means "unset / not yet scored" and is
// rejected at the persistence boundary so a scored image is never confused with an
// unrated one. `rubric_id` / `rubric_version` carry rubric identity so a future rubric
// change does not silently reinterpret earlier ratings.
//
// Rating is intentionally NOT part of full-text search (Phase 5f): it is exposed for
// sort/filter/recommendation only, once a product rubric is approved. DB ser/deser goes
// through the duckorm layer in `src/storage/` (`AiStore`); this header stays
// free of storage / ORM includes.
struct AiRating {
  sl_element_id_t file_id_           = 0;    // FK -> Sleeve element id (inode)
  std::string    task_id_           {};      // analysis task identity (rubric run, etc.)
  std::string    provider_id_       {};
  std::string    model_id_          {};
  std::string    prompt_profile_id_ {};      // prompt/profile identity
  std::string    rendition_kind_    {};      // selected rendition, e.g. "thumbnail_k1024"
  int            rating_           = 0;      // 1..=5 on success; 0 = unset
  std::string    rubric_id_         {};      // which rubric produced this rating
  std::string    rubric_version_    {};      // rubric version
  std::string    reasons_           {};      // short human-readable rationale
  bool           active_            = true;  // active-for-rating flag

  // Remote LLM rating contract: 1..5 (0 means unrated / not scored). `NormalizeRating`
  // clamps an arbitrary int into range; the EXIF Rating field in `metadata.hpp` keeps its
  // own 0..5 semantics (0 = unrated) — these are intentionally separate constants.
  static constexpr int kMinRating = 1;
  static constexpr int kMaxRating = 5;
  static constexpr auto NormalizeRating(int rating) -> int {
    return rating < kMinRating ? kMinRating : (rating > kMaxRating ? kMaxRating : rating);
  }

  // Identity + range check used at the persistence boundary: a rating is storable only
  // when its file key, provider/model identity, and a 1..5 rating are present.
  [[nodiscard]] auto IsValid() const -> bool;

  // Phase 7a reasons-only gate. The product star rating is the EXIF/metadata `Rating`
  // value, written through the existing star-rating path; `AiStore` is used
  // only to persist the rating *reasons* (rationale text) plus provider/model/prompt/
  // rubric identity. Such a row carries `rating_ = 0` as a sentinel ("the truth is the
  // EXIF star, not this column"), so `IsValid()` — which requires 1..5—rejects it. This
  // looser check ignores the rating value and requires only the file key, provider/model
  // identity, and non-empty reasons. `IsValid()` stays strict so Phase 5f tests are
  // untouched.
  [[nodiscard]] auto IsValidReasonsOnly() const -> bool;
};

}  // namespace alcedo