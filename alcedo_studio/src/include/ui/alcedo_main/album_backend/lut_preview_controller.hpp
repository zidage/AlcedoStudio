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

#include "app/editor_parameter_write.hpp"
#include "app/editor_session_service.hpp"
#include "app/lut_library_service.hpp"
#include "ui/alcedo_main/album_backend/browser_preview_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

namespace alcedo::ui {

/**
 * @brief The open photo and the LUT library, as the LUT previews read them.
 *
 * Production: an adapter over EditorSessionController and LutLibraryService
 * (LutPreviewController::setEditorSession and setLibrary). Tests supply their own. GUI thread.
 */
class LutPreviewSource {
 public:
  virtual ~LutPreviewSource()                                 = default;
  /// Image ID of the open photo; 0 when no photo is open.
  [[nodiscard]] virtual auto ImageId() const -> std::uint64_t = 0;
  /// The write that applying @p entry_id submits (LutLibraryController::ReadEntryWrite); false
  /// when the entry is not listed or cannot be applied, so it has no preview.
  [[nodiscard]] virtual auto EntryWrite(const QString& entry_id, EditorLutWrite* write) const
      -> bool                                                  = 0;
  /// True while the editor viewport renders frames (EditorSessionController::renderBusy).
  [[nodiscard]] virtual auto ViewportRendering() const -> bool = 0;
  /**
   * @brief Submit one preview render (IEditorSessionBackend::RenderLutPreview).
   * @return false with @p error set when the session refuses the command; @p on_complete is
   *         then never called. Otherwise @p on_complete runs once, on any thread.
   */
  virtual auto               RenderLutPreview(EditorLutPreviewRequest        request,
                                              EditorBrowserPreviewCompletion on_complete, std::string* error)
      -> bool = 0;
};

/**
 * @brief The LUT browser previews (issue #598): one image of the open photo per LUT tile.
 *
 * A preview is the import root of the photo with the LUT at full strength
 * (IEditorSessionBackend::RenderLutPreview), so it shows the direction of the LUT, not the
 * current edit. Its entry key is the library entry and the input and output encodings that a
 * click applies. Edits of the photo keep every preview; a change of the remembered encodings of
 * an entry makes its preview out of date. The first preview of a LUT also reads and parses its
 * .cube file on the editor render worker, as applying it does.
 *
 * Requests, the LRU, failures, and display: BrowserPreviewController. Images are served by
 * `image://alcedo-lut-preview/` (SharedLutPreviewImageStore).
 *
 * Thread affinity: GUI thread.
 */
class LutPreviewController : public BrowserPreviewController {
  Q_OBJECT
  Q_PROPERTY(alcedo::ui::EditorSessionController* editorSession READ editorSession WRITE
                 setEditorSession NOTIFY editorSessionChanged)
  Q_PROPERTY(alcedo::LutLibraryService* library READ library WRITE setLibrary NOTIFY libraryChanged)

 public:
  explicit LutPreviewController(QObject* parent = nullptr);
  ~LutPreviewController() override;

  [[nodiscard]] auto editorSession() const -> EditorSessionController* { return session_; }
  /// Follow the open photo of @p session. Together with @ref setLibrary, builds the production
  /// source.
  void               setEditorSession(EditorSessionController* session);
  [[nodiscard]] auto library() const -> alcedo::LutLibraryService* { return library_; }
  void               setLibrary(alcedo::LutLibraryService* library);
  /// Use @p source (not owned; must outlive this object or be replaced) instead of the session
  /// and library. The caller calls @ref Refresh when the source changes.
  void               SetSource(LutPreviewSource* source);

 signals:
  void editorSessionChanged();
  void libraryChanged();

 protected:
  [[nodiscard]] auto PreviewImageId() const -> std::uint64_t override;
  [[nodiscard]] auto EntryKey(const QString& entry_id) const -> std::optional<QString> override;
  [[nodiscard]] auto ViewportRendering() const -> bool override;
  auto               StartRender(const QString& entry_id, std::uint64_t image_id,
                                 EditorBrowserPreviewCompletion on_complete, std::string* error) -> bool override;
  [[nodiscard]] auto LogName() const -> QString override;

 private:
  void                                RebuildSessionSource();

  QPointer<EditorSessionController>   session_;
  QPointer<alcedo::LutLibraryService> library_;
  std::unique_ptr<LutPreviewSource>   session_source_;
  LutPreviewSource*                   source_ = nullptr;
};

}  // namespace alcedo::ui
