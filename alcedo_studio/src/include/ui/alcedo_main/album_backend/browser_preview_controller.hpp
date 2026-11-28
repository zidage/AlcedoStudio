//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QHash>
#include <QImage>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QWindow>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "app/editor_session_service.hpp"
#include "ui/alcedo_main/album_backend/browser_preview_image_provider.hpp"
#include "ui/edit_viewer/window_display_destination.hpp"

namespace alcedo::ui {

/// Rendered previews that one browser preview controller keeps: about 50 MB of 256 px RGBA8
/// images.
inline constexpr std::size_t kBrowserPreviewCacheCapacity = 200;

/**
 * @brief The tile previews of one browser of the open photo: the request queue, the memory LRU,
 *        and the display images. The LUT browser (LutPreviewController, issue #598) and the
 *        preset browser (PresetPreviewController, edit presets plan 2.4) derive from it.
 *
 * A derived controller says which entries have a preview and with which key (@ref EntryKey), and
 * submits one render (@ref StartRender). This class owns everything else:
 *
 * - Requests: a tile calls @ref watch while it shows an entry and @ref unwatch when it stops. Only
 *   watched entries render, the most recently watched first, and only one preview is in flight. A
 *   preview starts only while the viewport does not render (@ref ViewportRendering), so a viewport
 *   frame waits for at most the one preview that is already running.
 * - Keys: the image of the previews (@ref PreviewImageId) and the entry key. A change of either
 *   makes the shown preview out of date; the tile shows the new one when it is rendered. A switch
 *   of the image hides every preview and forgets the failed keys.
 * - Results: a completed render goes into a memory LRU of kBrowserPreviewCacheCapacity images
 *   under its rendered key (@ref RenderedEntryKey); nothing is written to disk. A failed render is
 *   not tried again for the same key (@ref previewFailed). A rejected render renders again after
 *   the next @ref Refresh.
 * - Display: @ref previewUrl gives the image URL of the entry, or an empty string when none is
 *   ready. @ref revision changes whenever a URL or a failure changes. The pixels go through the
 *   BrowserPreviewImageStore, which converts them to the display of the main window.
 *
 * Thread affinity: GUI thread. Render results are posted back to this thread.
 */
class BrowserPreviewController : public QObject {
  Q_OBJECT
  /// Changes whenever the URL or the failure of a watched entry changes.
  Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

 public:
  /// @p store is the display table of the image provider of this browser.
  explicit BrowserPreviewController(std::shared_ptr<BrowserPreviewImageStore> store,
                                    QObject*                                  parent = nullptr);
  ~BrowserPreviewController() override;

  /// Replace the display store (tests). Null keeps the current store.
  void                SetImageStore(std::shared_ptr<BrowserPreviewImageStore> store);
  /**
   * @brief Convert the previews to the display destination of @p window.
   *
   * Follows WindowDisplayDestination::DestinationChanged of @p window: a new destination goes to
   * the store, and each shown preview gets a new URL; nothing renders again. Null converts
   * nothing.
   */
  void                SetDisplayWindow(QWindow* window);

  [[nodiscard]] auto  revision() const -> int { return revision_; }

  /// A tile shows @p entry_id. Calls nest: each watch needs one unwatch.
  Q_INVOKABLE void    watch(const QString& entry_id);
  /// The tile no longer shows @p entry_id.
  Q_INVOKABLE void    unwatch(const QString& entry_id);
  /// Image URL of the preview of @p entry_id, or empty. @p revision is only a binding dependency.
  Q_INVOKABLE QString previewUrl(const QString& entry_id, int revision = 0) const;
  /// True when the render of the current key of @p entry_id failed. @p revision is only a binding
  /// dependency.
  Q_INVOKABLE bool    previewFailed(const QString& entry_id, int revision = 0) const;

  /// Read the image and the keys of every watched entry again, and render what is missing.
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
  void revisionChanged();

 protected:
  /// Image of the previews; 0 when there is none.
  [[nodiscard]] virtual auto PreviewImageId() const -> std::uint64_t                           = 0;
  /// Key of the preview of @p entry_id for the image; nullopt when the entry has no preview.
  [[nodiscard]] virtual auto EntryKey(const QString& entry_id) const -> std::optional<QString> = 0;
  /// True while the editor viewport renders frames.
  [[nodiscard]] virtual auto ViewportRendering() const -> bool                                 = 0;
  /**
   * @brief Submit one render of the preview of @p entry_id of image @p image_id.
   * @return false with @p error set when the render cannot start; @p on_complete is then never
   *         called. Otherwise @p on_complete runs once, on any thread.
   */
  virtual auto               StartRender(const QString& entry_id, std::uint64_t image_id,
                                         EditorBrowserPreviewCompletion on_complete, std::string* error)
      -> bool = 0;
  /**
   * @brief The entry key that @p result belongs to.
   *
   * The default is @p requested_entry_key. A derived controller whose render reads a key part on
   * the session owner (the sensor key of a preset preview) gives the key of what was rendered.
   */
  [[nodiscard]] virtual auto RenderedEntryKey(const QString&                    requested_entry_key,
                                              const EditorBrowserPreviewResult& result) const
      -> QString;
  /// Name of the preview in log messages, such as "LUT preview".
  [[nodiscard]] virtual auto LogName() const -> QString = 0;

 private:
  /// One rendered preview: SDR pixels and their output encoding.
  struct RenderedPreview {
    QImage              image;
    ViewerDisplayConfig encoding{};
  };

  /// The preview that one entry shows now.
  struct ShownPreview {
    QString         key;
    std::uint64_t   display_id = 0;
    QString         url;
    RenderedPreview preview;
  };

  /// The render in flight.
  struct InFlight {
    QString entry_id;
    QString key;
  };

  /// Full key of the preview of @p entry_id for the image; nullopt when it has none.
  [[nodiscard]] auto KeyFor(const QString& entry_id) const -> std::optional<QString>;
  /// Show the cached preview of @p entry_id, or queue its render.
  void               RefreshEntry(const QString& entry_id);
  void Show(const QString& entry_id, const QString& key, const RenderedPreview& preview);
  void Hide(const QString& entry_id, bool notify);
  void Enqueue(const QString& entry_id);
  /// Start the next render when none is in flight and the viewport is idle.
  void Pump();
  /// Submit the render of the preview in flight.
  void SubmitRender(const QString& entry_id, const QString& key);
  void OnRendered(const QString& entry_id, const QString& key, EditorBrowserPreviewResult result);
  [[nodiscard]] auto CachedImage(const QString& key) -> std::optional<RenderedPreview>;
  void               CachePut(const QString& key, RenderedPreview preview);
  void               BumpRevision();
  /// Give the store the destination of the display window, and each shown preview a new URL.
  void               ApplyDisplayDestination();

  std::shared_ptr<BrowserPreviewImageStore>                                store_;
  std::uint64_t                                                            image_id_ = 0;
  QHash<QString, int>                                                      watched_;
  /// Entries waiting for a render; the front renders next.
  std::deque<QString>                                                      pending_;
  std::optional<InFlight>                                                  in_flight_;
  /// LRU of rendered previews; the front is the most recently used.
  std::list<std::pair<QString, RenderedPreview>>                           lru_;
  QHash<QString, std::list<std::pair<QString, RenderedPreview>>::iterator> lru_index_;
  /// Keys whose render failed; they are not rendered again for this image.
  QSet<QString>                                                            failed_;
  QHash<QString, ShownPreview>                                             shown_;
  std::uint64_t                                                            next_display_id_ = 1;
  std::uint64_t                                                            render_count_    = 0;
  int                                                                      revision_        = 0;

  /// The destination of the display window; null without a window.
  QPointer<WindowDisplayDestination>                                       display_source_;
  QMetaObject::Connection                                                  display_connection_;
};

}  // namespace alcedo::ui
