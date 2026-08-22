//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <string>

#include "edit/graph/pipeline_document.hpp"

namespace alcedo {

struct LegacyImportResult {
  std::optional<PipelineDocument> document;
  std::string                     error;

  [[nodiscard]] auto Ok() const -> bool { return document.has_value(); }
};

/**
 * @brief One-way importer from old stage JSON to PipelineDocument.
 *
 * Does not switch the product read path. Unknown operator types fail the import.
 */
class LegacyPipelineImporter {
 public:
  /**
   * @param stage_json CPUPipelineExecutor::ExportPipelineParams shape.
   * @return Document on success; error string and empty document on failure.
   */
  [[nodiscard]] static auto Import(const nlohmann::json& stage_json) -> LegacyImportResult;
};

}  // namespace alcedo
