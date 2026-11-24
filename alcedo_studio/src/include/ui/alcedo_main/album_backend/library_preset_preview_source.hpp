//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointer>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "app/editor_session_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/preset_library_service.hpp"
#include "app/thumbnail_service.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/preset_preview_controller.hpp"

namespace alcedo::ui {

/**
 * @brief The preset previews of one library photo (edit presets plan 2.3, PS4 step 3): the
 *        source of the PresetPreviewController of the Apply Preset dialog.
 *
 * A preview is the result of an apply of the preset to the photo, except for the sensor stage:
 * BuildPresetPreviewDocument over the stored history of the photo, with the sensor settings of
 * its active Version (its committed snapshot). It renders on the batch executors of the thumbnail
 * service (ThumbnailService::RenderPreview) at 256 px; the thumbnail caches do not change.
 *
 * The photo must not be open in the editor: the editor session holds its history, and
 * PipelineMgmtService::LoadHistorySnapshot refuses it, so each render fails. The dialog shows the
 * previews of the editor (PresetPreviewController::SetEditorSession) for that photo.
 *
 * The services are read when a render starts, so a project change needs no call here.
 * Thread affinity: GUI thread; the render completion runs on a worker of the thumbnail service.
 */
class LibraryPresetPreviewSource final : public PresetPreviewSource {
 public:
  using PipelineServiceLookup  = std::function<std::shared_ptr<alcedo::PipelineMgmtService>()>;
  using ThumbnailServiceLookup = std::function<std::shared_ptr<alcedo::ThumbnailService>()>;

  /**
   * @param library The preset folder; must outlive this object or be null.
   * @param pipelines The pipeline service of the open project, or null without one.
   * @param thumbnails The thumbnail service of the open project, or null without one.
   */
  LibraryPresetPreviewSource(alcedo::PresetLibraryService* library, PipelineServiceLookup pipelines,
                             ThumbnailServiceLookup thumbnails);

  [[nodiscard]] auto ImageId() const -> std::uint64_t override { return image_id_; }
  [[nodiscard]] auto EntryFingerprint(const QString& entry_id) const
      -> std::optional<std::string> override;
  /// PresetPreviewSensorKey of the committed snapshot of the photo, read by ShowLibraryPhoto.
  [[nodiscard]] auto SensorKey() const -> std::string override { return sensor_key_; }
  /// False: the thumbnail executors render beside the editor viewport, as thumbnails do.
  [[nodiscard]] auto ViewportRendering() const -> bool override { return false; }
  [[nodiscard]] auto PackageFor(const QString& entry_id) -> PresetPackageResult override;
  /// Render one preview through ThumbnailService::RenderPreview. The result carries the sensor
  /// key of the committed snapshot that the render read.
  auto               RenderPresetPreview(EditorPresetPreviewRequest     request,
                                         EditorBrowserPreviewCompletion on_complete, std::string* error)
      -> bool override;
  /**
   * @brief Show the previews on @p element_id, image @p image_id.
   *
   * Reads the committed snapshot of the photo (PipelineMgmtService::AcquireCommittedSnapshot) on
   * the calling thread for its sensor key: one storage read, when the dialog opens.
   * @return false with @p error when the photo has no history or no project is open; the source
   *         then shows no photo.
   */
  auto ShowLibraryPhoto(sl_element_id_t element_id, image_id_t image_id, std::string* error)
      -> bool override;

 private:
  QPointer<alcedo::PresetLibraryService> library_;
  PipelineServiceLookup                  pipelines_;
  ThumbnailServiceLookup                 thumbnails_;
  sl_element_id_t                        element_id_ = 0;
  image_id_t                             image_id_   = 0;
  std::string                            sensor_key_;
};

}  // namespace alcedo::ui
