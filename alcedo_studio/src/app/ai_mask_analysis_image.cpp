//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_mask_analysis_image.hpp"

#include <xxhash.h>

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <opencv2/core.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"

namespace alcedo {
namespace {

auto ExtentText(std::uint32_t width, std::uint32_t height) -> std::string {
  return std::to_string(width) + " x " + std::to_string(height);
}

}  // namespace

auto BuildAiMaskAnalysisDocument(const CommitGraph& graph, const LoadedRootState& root,
                                 const std::shared_ptr<const PipelineGraphSnapshot>& current,
                                 sl_element_id_t                                     element_id)
    -> std::shared_ptr<const PipelineGraphSnapshot> {
  if (!current) {
    throw std::invalid_argument("The working state of the image is missing");
  }
  if (current->ElementId() != element_id) {
    throw std::invalid_argument("The working state belongs to another image");
  }
  std::string replay_error;
  auto        document = BuildDocumentFromRoot(graph, root, std::nullopt, &replay_error);
  if (!document) {
    throw std::runtime_error("The import root of the image cannot be rebuilt: " + replay_error);
  }
  ValidateProductDocument(*document, element_id);
  const auto* drt = document->Drt();
  if (drt != nullptr && IsHdrExportEncoding(*drt)) {
    throw std::invalid_argument(
        "The import root of the image uses an HDR encoding (ST 2084 or HLG); the AI Mask needs "
        "an SDR analysis image");
  }
  document->UseDevelopSettingsFrom(current->Document());
  (void)document->Develop()->Params().DngProfile().RequireBound();
  // The private document is frozen and dropped here; only the frozen copy leaves this call.
  return PipelineGraphSnapshot::Preview(document->Freeze(), current->ElementId(),
                                        current->Lineage(), transaction_chain_hash_t{});
}

auto MakeAiMaskAnalysisImageKey(sl_element_id_t element_id, image_id_t image_id,
                                const PipelineDocument& document) -> std::string {
  const auto  request = MakeAiMaskAnalysisRenderRequest();
  std::string canonical;
  canonical += "alcedo-ai-mask-analysis-image-key-1\n";
  canonical += "element:" + std::to_string(element_id) + "\n";
  canonical += "image:" + std::to_string(image_id) + "\n";
  canonical +=
      "geometry:" + std::to_string(static_cast<unsigned>(request.document_geometry)) + "\n";
  canonical += "max-edge:" + std::to_string(request.resolution.max_edge) + "\n";
  canonical +=
      "quality:" + std::to_string(static_cast<unsigned>(request.resolution.quality)) + "\n";
  canonical += document.ToJson().dump();
  const XXH128_hash_t digest = XXH3_128bits(canonical.data(), canonical.size());
  std::ostringstream  text;
  text << kAiMaskAnalysisImageKeyPrefix << std::hex << std::setfill('0') << std::setw(16)
       << digest.high64 << std::setw(16) << digest.low64;
  return text.str();
}

auto MakeAiMaskGeometryFingerprint(const DevelopParamsModel& develop) -> std::string {
  const nlohmann::json fields    = {{"demosaic_method", develop.DemosaicMethod()},
                                    {"highlights_reconstruct", develop.HighlightsReconstruct()},
                                    {"lens_enabled", develop.LensEnabled()},
                                    {"apply_distortion", develop.ApplyDistortion()},
                                    {"apply_tca", develop.ApplyTca()},
                                    {"apply_crop", develop.ApplyCrop()},
                                    {"auto_scale", develop.AutoScale()},
                                    {"use_user_scale", develop.UseUserScale()},
                                    {"user_scale", develop.UserScale()},
                                    {"projection_enabled", develop.ProjectionEnabled()},
                                    {"target_projection", develop.TargetProjection()},
                                    {"lens_maker", develop.LensMaker()},
                                    {"lens_model", develop.LensModel()}};
  const auto           canonical = fields.dump();
  const XXH128_hash_t  digest    = XXH3_128bits(canonical.data(), canonical.size());
  std::ostringstream   text;
  text << std::hex << std::setfill('0') << std::setw(16) << digest.high64 << std::setw(16)
       << digest.low64;
  return text.str();
}

auto MakeAiMaskAnalysisRenderRequest() -> RenderRequest {
  RenderRequest request;
  request.document_geometry   = DocumentGeometryUse::ReferenceSource;
  request.resolution.max_edge = kAiMaskAnalysisMaxLongEdge;
  request.resolution.quality  = RenderQuality::Preview;
  return request;
}

auto ConvertAiMaskAnalysisImage(const RenderedPipelineImage& rendered) -> SegmentationRgb8Image {
  if (!rendered.pixels || !rendered.pixels->cpu_data_valid_) {
    throw std::invalid_argument("The analysis image has no host pixels");
  }
  const cv::Mat& pixels = rendered.pixels->GetCPUData();
  if (pixels.empty() || pixels.type() != CV_32FC4) {
    throw std::invalid_argument("The analysis image pixels are not RGBA32F");
  }
  const auto width  = static_cast<std::uint32_t>(pixels.cols);
  const auto height = static_cast<std::uint32_t>(pixels.rows);
  if (width != rendered.geometry.render_extent.width ||
      height != rendered.geometry.render_extent.height) {
    throw std::invalid_argument(
        "The analysis image is " + ExtentText(width, height) +
        " pixels, but its render geometry is " +
        ExtentText(rendered.geometry.render_extent.width, rendered.geometry.render_extent.height));
  }
  if (std::max(width, height) > kAiMaskAnalysisMaxLongEdge) {
    throw std::invalid_argument("The analysis image is " + ExtentText(width, height) +
                                " pixels; the long edge limit is " +
                                std::to_string(kAiMaskAnalysisMaxLongEdge));
  }
  const auto eotf = rendered.display.encoding_eotf;
  if (eotf == ColorUtils::EOTF::ST2084 || eotf == ColorUtils::EOTF::HLG) {
    throw std::invalid_argument(std::string("The analysis image uses ") +
                                (eotf == ColorUtils::EOTF::ST2084 ? "ST 2084" : "HLG") +
                                " output; the AI Mask needs an SDR analysis image");
  }
  if (!cv::checkRange(pixels, /*quiet=*/true)) {
    throw std::invalid_argument("The analysis image contains a non-finite pixel value");
  }

  // The SDR rule of the editor comparison: saturate cast rounds to the nearest 8-bit value and
  // clamps to [0, 255].
  cv::Mat rgba8;
  pixels.convertTo(rgba8, CV_8UC4, 255.0);
  SegmentationRgb8Image image;
  image.width  = width;
  image.height = height;
  image.pixels.resize(static_cast<std::size_t>(width) * height * 3U);
  cv::Mat   rgb8(pixels.rows, pixels.cols, CV_8UC3, image.pixels.data());
  const int from_to[] = {0, 0, 1, 1, 2, 2};
  cv::mixChannels(&rgba8, 1, &rgb8, 1, from_to, 3);
  return image;
}

}  // namespace alcedo
