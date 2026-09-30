//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <variant>

#include "json.hpp"

namespace alcedo {

/// An official package LUT, resolved to the package's currently installed content.
/// Package updates keep this reference valid; the bytes it renders may change.
struct OfficialLutReference {
  std::string package_id;
  std::string lut_id;

  friend auto operator==(const OfficialLutReference&, const OfficialLutReference&)
      -> bool = default;
};

/// A file in the LUT library, by its root-relative `/`-separated path (the inventory
/// entry identity). Root selection and migration keep this reference valid.
struct LibraryLutReference {
  std::string relative_path;

  friend auto operator==(const LibraryLutReference&, const LibraryLutReference&) -> bool = default;
};

/// A file by its absolute path, as stored by projects that predate the library
/// (the legacy `cube_path` value). Resolved by exact path, then through recorded
/// previous library roots.
struct FileLutReference {
  std::string path;

  friend auto operator==(const FileLutReference&, const FileLutReference&) -> bool = default;
};

/**
 * @brief The LUT an LMT adjustment applies: none, or exactly one reference form.
 *
 * Only the applicable form is stored. A reference is not a file hash: two
 * different byte contents can be revisions of the same official LUT.
 */
using LutReference =
    std::variant<std::monostate, OfficialLutReference, LibraryLutReference, FileLutReference>;

[[nodiscard]] inline auto IsEmptyLutReference(const LutReference& reference) -> bool {
  return std::holds_alternative<std::monostate>(reference);
}

/// Empty when @p reference is valid, else the reason. Official IDs and paths must be
/// non-empty and contain no NUL or line break; a library path must be a safe relative
/// `/` path without `.` or `..` segments.
[[nodiscard]] auto ValidateLutReference(const LutReference& reference) -> std::string;

/// Stable text of @p reference for diagnostics and one-request-per-reference keys,
/// such as `official:spectral_film_lut/kodak-5207` or `library:user/look.cube`.
[[nodiscard]] auto DescribeLutReference(const LutReference& reference) -> std::string;

/// Tagged JSON object of an official or library reference
/// (`{"kind":"official","package_id":...,"lut_id":...}` or `{"kind":"library","path":...}`).
/// File references and the empty reference are stored as the legacy `cube_path` value
/// instead; calling this for them throws std::invalid_argument.
[[nodiscard]] auto TaggedLutReferenceToJson(const LutReference& reference) -> nlohmann::json;

/// Parse the tagged object written by TaggedLutReferenceToJson.
/// @throws std::invalid_argument for an unknown kind, missing or unknown keys, or an
///         invalid value. Nothing is returned for an invalid object.
[[nodiscard]] auto TaggedLutReferenceFromJson(const nlohmann::json& json) -> LutReference;

}  // namespace alcedo
