//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <stdexcept>
#include <string>

#include "app/document_transfer.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "json.hpp"
#include "support/editor_parameter_target_test.hpp"

namespace alcedo::test {

inline auto PatchDocumentField(PipelineDocument* document, const EditorParameterTarget& target,
                               const nlohmann::json& params) -> void {
  std::string error;
  if (!ApplyEditorParameterPatch(*document, target, params, &error)) {
    throw std::runtime_error(error.empty() ? "Failed to patch document field" : error);
  }
}

inline auto DocumentWithExposureEv(double exposure_ev) -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  PatchDocumentField(&document, ColorGradeFieldTarget("exposure"),
                     nlohmann::json{{"exposure_ev", exposure_ev}});
  return document;
}

inline auto DocumentWithLutPath(std::string cube_path) -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  std::string error;
  const auto target = CompleteCurrentPanelParameterTarget(document, "lut", &error);
  if (!target.has_value()) {
    throw std::runtime_error(error.empty() ? "LUT target is missing" : error);
  }
  PatchDocumentField(&document, *target, nlohmann::json{{"cube_path", std::move(cube_path)}});
  return document;
}

inline auto CaptureDocumentPackage(const PipelineDocument& document, MaskStore* mask_store = nullptr)
    -> AdjustmentTransferPackage {
  return CaptureDocumentTransfer(document, mask_store);
}

inline auto MakeExposureTransferPackage(double exposure_ev) -> AdjustmentTransferPackage {
  return CaptureDocumentPackage(DocumentWithExposureEv(exposure_ev));
}

inline auto MakeLutTransferPackage(std::string cube_path) -> AdjustmentTransferPackage {
  return CaptureDocumentPackage(DocumentWithLutPath(std::move(cube_path)));
}

inline auto MakeDefaultDocumentTransferPackage() -> AdjustmentTransferPackage {
  return CaptureDocumentPackage(CreateDefaultPipelineDocument());
}

}  // namespace alcedo::test
