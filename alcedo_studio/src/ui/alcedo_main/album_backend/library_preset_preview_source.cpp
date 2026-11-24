//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/library_preset_preview_source.hpp"

#include <exception>
#include <opencv2/core.hpp>
#include <utility>

#include "app/browser_preview_render.hpp"
#include "app/preset_preview_image.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/runtime/rendered_pipeline_image.hpp"
#include "image/image_buffer.hpp"

namespace alcedo::ui {
namespace {

/**
 * The RGBA8 pixels of a preview render as the RGBA32F image of a browser preview result. The
 * reference and render extents are the pixel size: a browser tile uses the image only.
 */
auto BrowserPreviewImageOf(const alcedo::ThumbnailPreviewResult& result)
    -> std::optional<alcedo::RenderedPipelineImage> {
  if (!result.pixels || !result.pixels->cpu_data_valid_) return std::nullopt;
  const cv::Mat& rgba8 = result.pixels->GetCPUData();
  if (rgba8.empty() || rgba8.type() != CV_8UC4) return std::nullopt;
  cv::Mat rgba32f;
  rgba8.convertTo(rgba32f, CV_32FC4, 1.0 / 255.0);
  alcedo::RenderedPipelineImage image;
  image.pixels = std::make_shared<ImageBuffer>(std::move(rgba32f));
  const Extent2D extent{static_cast<std::uint32_t>(rgba8.cols),
                        static_cast<std::uint32_t>(rgba8.rows)};
  image.geometry.render_extent         = extent;
  image.geometry.full_reference_extent = extent;
  image.display                        = result.encoding;
  return image;
}

}  // namespace

LibraryPresetPreviewSource::LibraryPresetPreviewSource(alcedo::PresetLibraryService* library,
                                                       PipelineServiceLookup         pipelines,
                                                       ThumbnailServiceLookup        thumbnails)
    : library_(library), pipelines_(std::move(pipelines)), thumbnails_(std::move(thumbnails)) {}

auto LibraryPresetPreviewSource::EntryFingerprint(const QString& entry_id) const
    -> std::optional<std::string> {
  if (!library_) return std::nullopt;
  const auto* entry = library_->FindEntry(entry_id.toStdString());
  if (entry == nullptr || !entry->available || entry->fingerprint.empty()) return std::nullopt;
  return entry->fingerprint;
}

auto LibraryPresetPreviewSource::PackageFor(const QString& entry_id) -> PresetPackageResult {
  if (!library_) {
    PresetPackageResult refused;
    refused.message = "The preset library is not available";
    return refused;
  }
  return library_->PackageFor(entry_id.toStdString());
}

auto LibraryPresetPreviewSource::ShowLibraryPhoto(sl_element_id_t element_id, image_id_t image_id,
                                                  std::string* error) -> bool {
  element_id_ = 0;
  image_id_   = 0;
  sensor_key_.clear();
  const auto pipelines = pipelines_ ? pipelines_() : nullptr;
  if (!pipelines) {
    if (error != nullptr) *error = "No project is open";
    return false;
  }
  if (element_id == 0 || image_id == 0) {
    if (error != nullptr) *error = "No photo is given";
    return false;
  }
  try {
    const auto committed = pipelines->AcquireCommittedSnapshot(element_id);
    sensor_key_          = PresetPreviewSensorKey(committed->Document());
  } catch (const std::exception& e) {
    if (error != nullptr) *error = e.what();
    return false;
  }
  element_id_ = element_id;
  image_id_   = image_id;
  return true;
}

auto LibraryPresetPreviewSource::RenderPresetPreview(EditorPresetPreviewRequest     request,
                                                     EditorBrowserPreviewCompletion on_complete,
                                                     std::string*                   error) -> bool {
  const auto fail = [error](std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
  };
  if (element_id_ == 0 || request.image_id != image_id_) {
    return fail("The preset preview belongs to another photo");
  }
  if (!request.package) return fail("A preset preview requires a preset package");
  auto pipelines  = pipelines_ ? pipelines_() : nullptr;
  auto thumbnails = thumbnails_ ? thumbnails_() : nullptr;
  if (!pipelines || !thumbnails) return fail("No project is open");

  // Written by the document builder on the lookup worker, read by the completion after the
  // render on a later stage of the same request.
  auto                            sensor_key = std::make_shared<std::string>();
  alcedo::ThumbnailPreviewRequest preview;
  preview.element_id = element_id_;
  preview.image_id   = image_id_;
  preview.resolution = alcedo::ThumbnailResolution::k256;
  preview.build_document =
      [pipelines, element_id = element_id_, package = std::move(request.package),
       sensor_key](const std::shared_ptr<const alcedo::PipelineGraphSnapshot>& committed) {
        // The stored history of the photo; the editor session does not hold the photo.
        const auto history = pipelines->LoadHistorySnapshot(element_id);
        *sensor_key        = PresetPreviewSensorKey(committed->Document());
        auto document      = alcedo::BuildPresetPreviewDocument(*history.graph_, *history.root_,
                                                                committed, element_id, *package);
        auto output_color  = alcedo::BrowserPreviewOutputColor(document->Document());
        return alcedo::ThumbnailPreviewDocument{std::move(document), std::move(output_color)};
      };
  preview.on_complete =
      [sensor_key, on_complete = std::move(on_complete)](alcedo::ThumbnailPreviewResult done) {
        EditorBrowserPreviewResult result;
        result.sensor_key = *sensor_key;
        if (done.status == alcedo::ThumbnailRequestStatus::kReady) {
          if (auto image = BrowserPreviewImageOf(done)) {
            result.status = EditorBrowserPreviewStatus::Completed;
            result.image  = std::move(*image);
          } else {
            result.status  = EditorBrowserPreviewStatus::Failed;
            result.message = "The preset preview render has no RGBA8 pixels";
          }
        } else {
          result.status  = EditorBrowserPreviewStatus::Failed;
          result.message = done.message.empty() ? "The preset preview render failed" : done.message;
        }
        on_complete(std::move(result));
      };
  try {
    thumbnails->RenderPreview(std::move(preview));
  } catch (const std::exception& e) {
    return fail(e.what());
  }
  return true;
}

}  // namespace alcedo::ui
