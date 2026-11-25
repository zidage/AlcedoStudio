//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "app/editor_session_service.hpp"
#include "app/preset_library_service.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/browser_preview_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

namespace alcedo::ui {

/**
 * @brief The open photo and the preset library, as the preset previews read them.
 *
 * Production: an adapter over EditorSessionController and PresetLibraryService
 * (PresetPreviewController::SetEditorSession and SetLibrary). Tests supply their own. GUI thread.
 */
class PresetPreviewSource {
 public:
  virtual ~PresetPreviewSource()                              = default;
  /// Image ID of the open photo; 0 when no photo is open.
  [[nodiscard]] virtual auto ImageId() const -> std::uint64_t = 0;
  /// Fingerprint of the listed, available preset @p entry_id (PresetEntry::fingerprint); nullopt
  /// when the entry is not listed or not available, so it has no preview.
  [[nodiscard]] virtual auto EntryFingerprint(const QString& entry_id) const
      -> std::optional<std::string>                                                     = 0;
  /// PresetPreviewSensorKey of the working document of the open photo; empty without one.
  [[nodiscard]] virtual auto SensorKey() const -> std::string                           = 0;
  /// True while the editor viewport renders frames (EditorSessionController::renderBusy).
  [[nodiscard]] virtual auto ViewportRendering() const -> bool                          = 0;
  /// The checked package of @p entry_id (PresetLibraryService::PackageFor).
  [[nodiscard]] virtual auto PackageFor(const QString& entry_id) -> PresetPackageResult = 0;
  /**
   * @brief Submit one preview render (IEditorSessionBackend::RenderPresetPreview).
   * @return false with @p error set when the session refuses the command; @p on_complete is
   *         then never called. Otherwise @p on_complete runs once, on any thread.
   */
  virtual auto               RenderPresetPreview(EditorPresetPreviewRequest     request,
                                                 EditorBrowserPreviewCompletion on_complete, std::string* error)
      -> bool = 0;
  /**
   * @brief Show the previews on the library photo @p element_id, image @p image_id.
   *
   * Only a source of a library photo (LibraryPresetPreviewSource) has a photo to choose; the
   * default refuses, because the source of the editor follows the open photo.
   * @return false with @p error when the source refuses the photo.
   */
  virtual auto ShowLibraryPhoto(sl_element_id_t element_id, image_id_t image_id, std::string* error)
      -> bool;
};

/**
 * @brief The preset browser previews (edit presets plan 2.4): one image of the open photo per
 *        preset tile.
 *
 * A preview is the import root of the photo with the preset applied by the Paste planner and the
 * sensor settings of the current Version (IEditorSessionBackend::RenderPresetPreview). So it is the
 * result of an apply, except that the demosaic method, AI Denoise, and the other sensor settings
 * of the preset are not in it (plan D6).
 *
 * The entry key is the preset id, its fingerprint, and the sensor key of the working document
 * (PresetPreviewSensorKey). A change of the preset file (after a scan), of the photo, or of the
 * sensor settings of the current Version gives a new preview; an edit of a Color Grade or the
 * DRT of the current Version does not. The session keys the result with the sensor key of the
 * document that it rendered (@ref RenderedEntryKey), so a sensor edit that reaches the session
 * owner before the render never stores pixels under the key of the older settings.
 *
 * A preset whose package cannot be read fails its preview; the tile shows the warning icon and the
 * preset stays usable. Requests, the LRU, failures, and display: BrowserPreviewController. Images
 * are served by `image://alcedo-preset-preview/` (SharedPresetPreviewImageStore).
 *
 * Thread affinity: GUI thread.
 */
class PresetPreviewController final : public BrowserPreviewController {
  Q_OBJECT

 public:
  explicit PresetPreviewController(QObject* parent = nullptr);
  ~PresetPreviewController() override;

  /// Follow the open photo and the working document of @p session.
  void SetEditorSession(EditorSessionController* session);
  /// Follow the inventory of @p library.
  void SetLibrary(alcedo::PresetLibraryService* library);
  /// Use @p source (not owned; must outlive this object or be replaced) instead of the session
  /// and library. The caller calls @ref Refresh when the source changes.
  void SetSource(PresetPreviewSource* source);
  /// Use @p source and own it: the source of a library photo (the Apply Preset dialog).
  void             SetOwnedSource(std::unique_ptr<PresetPreviewSource> source);

  /**
   * @brief Show the previews on the library photo @p elementId, image @p imageId (the first
   *        target of the Apply Preset dialog, plan 2.3).
   *
   * Hands the photo to the source (PresetPreviewSource::ShowLibraryPhoto), then renders the
   * watched tiles again. A refused photo shows no previews and is written to the log.
   * @return false when the source refuses the photo.
   */
  Q_INVOKABLE bool showLibraryPhoto(uint elementId, uint imageId);

 protected:
  [[nodiscard]] auto PreviewImageId() const -> std::uint64_t override;
  [[nodiscard]] auto EntryKey(const QString& entry_id) const -> std::optional<QString> override;
  [[nodiscard]] auto ViewportRendering() const -> bool override;
  auto               StartRender(const QString& entry_id, std::uint64_t image_id,
                                 EditorBrowserPreviewCompletion on_complete, std::string* error) -> bool override;
  [[nodiscard]] auto RenderedEntryKey(const QString&                    requested_entry_key,
                                      const EditorBrowserPreviewResult& result) const
      -> QString override;
  [[nodiscard]] auto LogName() const -> QString override;

 private:
  void                                   RebuildSessionSource();

  QPointer<EditorSessionController>      session_;
  QPointer<alcedo::PresetLibraryService> library_;
  /// The source that this controller owns: the session source, or the one of SetOwnedSource.
  std::unique_ptr<PresetPreviewSource>   session_source_;
  PresetPreviewSource*                   source_          = nullptr;
  /// False after SetSource or SetOwnedSource: SetEditorSession and SetLibrary then keep the
  /// source and only follow the change signals.
  bool                                   follows_session_ = true;
};

}  // namespace alcedo::ui
