//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "app/editor_parameter_write.hpp"
#include "app/editor_session_service.hpp"
#include "app/lut_library_service.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/lut_preview_image_provider.hpp"

namespace alcedo::ui {

/// Rendered LUT previews that the controller keeps: about 50 MB of 256 px RGBA8 images.
inline constexpr std::size_t kLutPreviewCacheCapacity = 200;

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
  virtual auto               RenderLutPreview(EditorLutPreviewRequest    request,
                                              EditorLutPreviewCompletion on_complete, std::string* error)
      -> bool = 0;
};

/**
 * @brief The LUT browser previews (issue #598): one image of the open photo per LUT tile.
 *
 * A preview is the import root of the photo with the LUT at full strength
 * (IEditorSessionBackend::RenderLutPreview), so it shows the direction of the LUT, not the
 * current edit. Its key is the image, the library entry, and the input and output encodings that
 * a click applies. Edits of the photo keep every preview; a change of the remembered encodings of
 * an entry makes its preview out of date, and the tile shows the new one when it is rendered.
 *
 * Requests: a tile calls @ref watch while it shows an entry in the grid layout and @ref unwatch
 * when it stops. Only watched entries render, the most recently watched first, and only one
 * preview is in flight. A preview starts only while the viewport does not render
 * (LutPreviewSource::ViewportRendering), so a viewport frame waits for at most the one preview
 * that is already running. The first preview of a LUT also reads and parses its .cube file on the
 * editor render worker, as applying it does. Rendered previews
 * stay in a memory LRU of kLutPreviewCacheCapacity images; nothing is written to disk. A failed
 * render is not tried again for the same key; the tile keeps its placeholder.
 *
 * Display: @ref previewUrl gives the image URL of the entry, or an empty string when none is
 * ready. @ref revision changes whenever a URL changes.
 *
 * Thread affinity: GUI thread. Render results are posted back to this thread.
 */
class LutPreviewController : public QObject {
  Q_OBJECT
  Q_PROPERTY(alcedo::ui::EditorSessionController* editorSession READ editorSession WRITE
                 setEditorSession NOTIFY editorSessionChanged)
  Q_PROPERTY(alcedo::LutLibraryService* library READ library WRITE setLibrary NOTIFY libraryChanged)
  /// Changes whenever the URL of a watched entry changes.
  Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

 public:
  explicit LutPreviewController(QObject* parent = nullptr);
  ~LutPreviewController() override;

  [[nodiscard]] auto  editorSession() const -> EditorSessionController* { return session_; }
  /// Follow the open photo of @p session. Together with @ref setLibrary, builds the production
  /// source.
  void                setEditorSession(EditorSessionController* session);
  [[nodiscard]] auto  library() const -> alcedo::LutLibraryService* { return library_; }
  void                setLibrary(alcedo::LutLibraryService* library);
  /// Use @p source (not owned; must outlive this object or be replaced) instead of the session
  /// and library. The caller calls @ref Refresh when the source changes.
  void                SetSource(LutPreviewSource* source);
  /// Replace the display store (tests); the default is SharedLutPreviewImageStore().
  void                SetImageStore(std::shared_ptr<LutPreviewImageStore> store);

  [[nodiscard]] auto  revision() const -> int { return revision_; }

  /// A tile shows @p entry_id in the grid layout. Calls nest: each watch needs one unwatch.
  Q_INVOKABLE void    watch(const QString& entry_id);
  /// The tile no longer shows @p entry_id.
  Q_INVOKABLE void    unwatch(const QString& entry_id);
  /// Image URL of the preview of @p entry_id, or empty. @p revision is only a binding dependency.
  Q_INVOKABLE QString previewUrl(const QString& entry_id, int revision = 0) const;

  /// Read the open photo and the keys of every watched entry again, and render what is missing.
  void                Refresh();
  /// The viewport stopped or started rendering: start the next preview when it is idle.
  void                ResumeAfterViewport();

  /// Renders submitted since construction (diagnostics and tests).
  [[nodiscard]] auto  render_count() const -> std::uint64_t { return render_count_; }
  /// Rendered previews in the LRU.
  [[nodiscard]] auto  cached_count() const -> std::size_t { return lru_.size(); }
  /// True while a render is in flight.
  [[nodiscard]] auto  rendering() const -> bool { return in_flight_.has_value(); }

 signals:
  void editorSessionChanged();
  void libraryChanged();
  void revisionChanged();

 private:
  /// The preview that one entry shows now.
  struct ShownPreview {
    QString       key;
    std::uint64_t display_id = 0;
    QString       url;
  };

  /// The render in flight.
  struct InFlight {
    QString entry_id;
    QString key;
  };

  void               RebuildSessionSource();
  /// Key of the preview of @p entry_id for the open photo; nullopt when it has none.
  [[nodiscard]] auto KeyFor(const QString& entry_id, EditorLutWrite* write) const
      -> std::optional<QString>;
  /// Show the cached preview of @p entry_id, or queue its render.
  void RefreshEntry(const QString& entry_id);
  void Show(const QString& entry_id, const QString& key, const QImage& image);
  void Hide(const QString& entry_id, bool notify);
  void Enqueue(const QString& entry_id);
  /// Start the next render when none is in flight and the viewport is idle.
  void Pump();
  /// Submit the render of the preview in flight.
  void SubmitRender(const QString& entry_id, const QString& key, EditorLutWrite write);
  void OnRendered(const QString& entry_id, const QString& key, EditorLutPreviewResult result);
  [[nodiscard]] auto                    CachedImage(const QString& key) -> std::optional<QImage>;
  void                                  CachePut(const QString& key, QImage image);
  void                                  BumpRevision();

  QPointer<EditorSessionController>     session_;
  QPointer<alcedo::LutLibraryService>   library_;
  std::unique_ptr<LutPreviewSource>     session_source_;
  LutPreviewSource*                     source_ = nullptr;
  std::shared_ptr<LutPreviewImageStore> store_;
  std::uint64_t                         image_id_ = 0;
  QHash<QString, int>                   watched_;
  /// Entries waiting for a render; the front renders next.
  std::deque<QString>                   pending_;
  std::optional<InFlight>               in_flight_;
  /// LRU of rendered previews; the front is the most recently used.
  std::list<std::pair<QString, QImage>> lru_;
  QHash<QString, std::list<std::pair<QString, QImage>>::iterator> lru_index_;
  /// Keys whose render failed; they are not rendered again for this photo.
  QSet<QString>                                                   failed_;
  QHash<QString, ShownPreview>                                    shown_;
  std::uint64_t                                                   next_display_id_ = 1;
  std::uint64_t                                                   render_count_    = 0;
  int                                                             revision_        = 0;
};

}  // namespace alcedo::ui
