//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/runtime/content_key.hpp"
#include "edit/runtime/runtime_revision.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {

/**
 * @brief Pixel-affecting representation of one cached result.
 *
 * Identity is a mix of source, document epoch, geometry or canonical mapping,
 * decode/quality, and backend capability. It does not encode node parameter
 * values. @ref source_detail is the cached long-edge requirement; a higher
 * requested detail is incompatible.
 */
struct ResultRepresentation {
  std::uint64_t   identity       = 0;
  RuntimeRevision document_epoch = 0;
  ImageExtent     extent{};
  TextureFormat   format         = TextureFormat::Rgba32f;
  std::uint32_t   source_detail  = 0;

  [[nodiscard]] auto Empty() const -> bool { return identity == 0 && extent.width == 0; }
};

inline auto operator==(const ResultRepresentation& a, const ResultRepresentation& b) -> bool {
  return a.identity == b.identity && a.document_epoch == b.document_epoch && a.extent == b.extent &&
         a.format == b.format && a.source_detail == b.source_detail;
}

inline auto operator!=(const ResultRepresentation& a, const ResultRepresentation& b) -> bool {
  return !(a == b);
}

[[nodiscard]] inline auto MakeExtentRepresentation(ImageExtent extent, TextureFormat format)
    -> ResultRepresentation {
  ResultRepresentation repr;
  repr.extent = extent;
  repr.format = format;
  return repr;
}

/**
 * @brief True when @p cached can satisfy @p needed.
 *
 * Extent, format, identity, and epoch must match. Cached source detail must be
 * at least the requested detail.
 */
[[nodiscard]] inline auto RepresentationSatisfies(const ResultRepresentation& cached,
                                                  const ResultRepresentation& needed) -> bool {
  return cached.identity == needed.identity && cached.document_epoch == needed.document_epoch &&
         cached.extent == needed.extent && cached.format == needed.format &&
         cached.source_detail >= needed.source_detail;
}

}  // namespace alcedo
