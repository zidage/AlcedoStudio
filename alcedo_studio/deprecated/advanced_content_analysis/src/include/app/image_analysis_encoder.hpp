//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "app/thumbnail_service.hpp"

namespace alcedo {

// Encoded image rendition produced for a remote multimodal provider (Phase 5d). The
// host owns rendition selection and records these fields in the result metadata; the
// `format_hint` travels on the wire as `DescribeImageRequest.image_format_hint` /
// `ScoreImageRequest.image_format_hint`.
struct EncodedRendition {
  std::vector<uint8_t> bytes;
  std::string          mime_type;       // "image/jpeg"
  std::string          format_hint;     // "image/jpeg;max_edge=<N>"
  std::string          rendition_kind;  // "thumbnail"
  uint32_t             width    = 0;    // actual encoded width
  uint32_t             height   = 0;    // actual encoded height
  uint32_t             max_edge = 0;    // actual longest side (max(width, height))
  int                  quality  = 0;
  bool                 ok = false;
  std::string          error;
};

// Encodes a host-rendered thumbnail (a k1024 rendition) to JPEG bytes for a remote
// multimodal provider. Uses OpenImageIO as the primary codec path (NOT OpenCV
// imgcodecs); the raw RGBA8 conversion stays isolated to the local CLIP embedding
// path (`MaterializeThumbnailRgba8`). OpenImageIO writes to a scoped temp file under
// `temp_dir` (the in-memory sink is unproven on this MSVC/DLL build) and the bytes are
// read back; the temp file is removed on every exit path (RAII), so a cancellation or
// failure cannot leak it. `max_edge_hint` is recorded for diagnostics; the returned
// `max_edge`/`width`/`height` reflect the actual encoded image. JPEG is the only
// format in Phase 5d (PNG fallback is a later fast-follow).
auto EncodeThumbnailForRemoteAnalysis(const ThumbnailGuard&        guard,
                                      int                         quality,
                                      uint32_t                    max_edge_hint,
                                      const std::filesystem::path& temp_dir,
                                      std::string*                error) -> EncodedRendition;

}  // namespace alcedo
