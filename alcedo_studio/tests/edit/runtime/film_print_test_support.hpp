//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <utility>

#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"

/// Helpers of the GPU film print tests, shared by the CUDA and the OpenCL fixtures: the white
/// balance of the RAW decode that scene light As Shot reads (issue 661).
namespace alcedo::film_print_test {

/// Develop mode As Shot, with @p kelvin as the solved temperature of the RAW file.
inline void SetRawAsShot(PipelineDocument& document, double kelvin) {
  auto payload        = document.Develop()->Params().Params();
  payload.wb_mode     = "as_shot";
  payload.as_shot_cct = static_cast<float>(kelvin);
  payload.input.reset();
  document.Develop()->Params().ReplaceParams(std::move(payload));
}

/// Develop mode Custom with @p kelvin.
inline void SetRawCustom(PipelineDocument& document, double kelvin) {
  DevelopColorTemperatureUpdate update;
  update.wb_mode    = std::string{"custom"};
  update.custom_cct = static_cast<float>(kelvin);
  document.Develop()->Params().ApplyColorTemperatureUpdate(update);
}

/// A raster input (JPEG, PNG, TIFF), which has no RAW white balance.
inline void SetRasterInput(PipelineDocument& document) {
  auto payload  = document.Develop()->Params().Params();
  payload.input = DevelopRasterInput{};
  document.Develop()->Params().ReplaceParams(std::move(payload));
}

}  // namespace alcedo::film_print_test
