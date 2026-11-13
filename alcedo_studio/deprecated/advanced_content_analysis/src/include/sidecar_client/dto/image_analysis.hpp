//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace alcedo {

struct ImageAnalysisRendition {
  std::string kind;
  uint32_t    width    = 0;
  uint32_t    height   = 0;
  uint64_t    bytes    = 0;
  uint32_t    max_edge = 0;
};

struct ImageAnalysisUsage {
  int64_t input_tokens  = 0;
  int64_t output_tokens = 0;
  int64_t total_tokens  = 0;
};

struct ImageAnalysisRequest {
  std::string            request_id;
  std::vector<uint8_t>   image_bytes;
  std::string            image_format_hint;
  ImageAnalysisRendition rendition;
  std::string            provider_id;
  std::string            model_id;
  std::string            prompt_profile_id;
  std::string            credential_ref;
  std::string            rubric_id;
  // Host-resolved target language for generated caption/reasons ("" or "en" =
  // English; "zh" = Simplified Chinese). The host resolves a "follow app
  // language" preference before sending, so the sidecar never sees "follow".
  std::string            output_language;
  // Rating strictness persona for ScoreImage only: "" or "normal" = the default
  // balanced rubric; "lite" = generous; "high" = master-guided critique;
  // "xhigh" = 老法师; "max" = exacting 懂哥 connoisseur.
  // Selects the rating system prompt in the driver; does not change the JSON
  // contract. Ignored by DescribeImage.
  std::string            rating_severity;
  // Optional non-secret camera/EXIF context for gear/parameter-sensitive rating
  // personas. Other modes leave it empty.
  std::string            camera_context;
  bool                   include_understanding = true;
  bool                   include_rating        = false;
  bool                   include_rating_reasons = true;
};

struct ImageAnalysisUnderstandingResult {
  std::string              request_id;
  bool                     ok         = false;
  int                      status     = 0;
  int                      error_code = 0;
  std::string              error;
  std::string              caption;
  std::vector<std::string> tags;
  std::string              scene;
  double                   confidence = 0.0;
  std::string              provider;
  std::string              model_id;
  std::string              provider_request_id;
  std::string              prompt_profile_id;
  ImageAnalysisRendition   rendition;
  ImageAnalysisUsage       usage;
  uint64_t                 elapsed_ms = 0;
};

struct ImageAnalysisRatingResult {
  std::string            request_id;
  bool                   ok         = false;
  int                    status     = 0;
  int                    error_code = 0;
  std::string            error;
  int                    rating = 0;
  std::string            rubric_id;
  std::string            rubric_version;
  std::string            reasons;
  std::string            provider;
  std::string            model_id;
  std::string            provider_request_id;
  std::string            prompt_profile_id;
  ImageAnalysisRendition rendition;
  ImageAnalysisUsage     usage;
  uint64_t               elapsed_ms = 0;
};

struct ImageAnalysisCombinedResult {
  std::string                    request_id;
  bool                           ok         = false;
  int                            status     = 0;
  int                            error_code = 0;
  std::string                    error;
  std::string                    provider;
  std::string                    model_id;
  std::string                    provider_request_id;
  std::string                    prompt_profile_id;
  ImageAnalysisRendition         rendition;
  ImageAnalysisUsage             usage;
  uint64_t                       elapsed_ms = 0;
  bool                           has_understanding = false;
  bool                           has_rating        = false;
  ImageAnalysisUnderstandingResult understanding;
  ImageAnalysisRatingResult        rating;
};

struct AiDiscoveredModel {
  std::string model_id;
  std::string display_name;
  std::string source_provider_id;
};

struct ImageAnalysisListModelsResult {
  std::string                    request_id;
  bool                           ok         = false;
  int                            status     = 0;
  int                            error_code = 0;
  std::string                    error;
  std::string                    provider;
  uint64_t                       elapsed_ms = 0;
  std::vector<AiDiscoveredModel> models;
};

}  // namespace alcedo
