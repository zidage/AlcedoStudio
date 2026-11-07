//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "sidecar_client/dto/image_analysis.hpp"

namespace alcedo::sidecar_client {

class ImageAnalysisClient {
 public:
  virtual ~ImageAnalysisClient() = default;

  virtual auto DescribeImage(const ImageAnalysisRequest& request,
                             std::chrono::milliseconds timeout)
      -> ImageAnalysisUnderstandingResult = 0;
  virtual auto ScoreImage(const ImageAnalysisRequest& request, std::chrono::milliseconds timeout)
      -> ImageAnalysisRatingResult = 0;
  virtual auto AnalyzeImage(const ImageAnalysisRequest& request, std::chrono::milliseconds timeout)
      -> ImageAnalysisCombinedResult = 0;
  virtual auto BatchAnalyzeImage(const std::vector<ImageAnalysisRequest>& requests,
                                 std::chrono::milliseconds timeout)
      -> std::vector<ImageAnalysisCombinedResult> = 0;
  virtual auto ListModels(const std::string& provider_id, const std::string& credential_ref,
                          std::chrono::milliseconds timeout) -> ImageAnalysisListModelsResult = 0;
};

}  // namespace alcedo::sidecar_client
