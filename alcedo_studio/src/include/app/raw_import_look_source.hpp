//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/graph/pipeline_document.hpp"

namespace alcedo {

/**
 * @brief Where an import reads the RAW import look (#918).
 *
 * The import service reads the look once at the start of each import and builds the history root
 * of every RAW image of that import from it (@ref CreateDefaultPipelineDocument). The edit layer
 * sees only the @ref RawImportLook value; the source decides where the values are stored.
 *
 * Threading: @ref Read can run on any thread.
 */
class RawImportLookSource {
 public:
  virtual ~RawImportLookSource() = default;

  /// The look of the next import. Every value passes @ref DescribeRawImportLookError.
  [[nodiscard]] virtual auto Read() const -> RawImportLook = 0;
};

/// Source of the product Default look: a value-initialized @ref RawImportLook. Tests and tools
/// that import without user settings use it.
class ProductDefaultRawImportLookSource final : public RawImportLookSource {
 public:
  [[nodiscard]] auto Read() const -> RawImportLook override { return RawImportLook{}; }
};

}  // namespace alcedo
