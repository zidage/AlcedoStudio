//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QImage>
#include <QObject>
#include <QQuickImageProvider>
#include <QString>
#include <QtGlobal>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "type/type.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/edit_viewer/window_display_destination.hpp"

namespace alcedo::ui {

inline constexpr const char* kEditorPlaceholderImageProviderId = "alcedo-editor-placeholder";

/**
 * @brief The image that the editor viewport shows before the first frame of a photo, and its
 *        encoding (AI Denoise integration plan, DN14). Shared by the placeholder owner and its
 *        image provider.
 *
 * Thread: any thread (a mutex). The owner writes on the GUI thread; the provider reads on the QML
 * image thread.
 */
class EditorPlaceholderImage {
 public:
  using DestinationSource = std::function<std::shared_ptr<const DisplayDestination>()>;

  explicit EditorPlaceholderImage(DestinationSource destination = {});

  void               Set(QImage image, const ViewerDisplayConfig& encoding);
  void               Clear();
  /// The image converted to the current display destination (ConvertImageForDestination); the
  /// source pixels when the conversion fails; null without an image.
  [[nodiscard]] auto ForDisplay() const -> QImage;
  /// The source pixels; null without an image.
  [[nodiscard]] auto Source() const -> QImage;

 private:
  DestinationSource   destination_;
  mutable std::mutex  mutex_;
  QImage              image_;
  ViewerDisplayConfig encoding_{};
};

/// `image://alcedo-editor-placeholder/<revision>`: the placeholder image of the open photo.
class EditorPlaceholderImageProvider final : public QQuickImageProvider {
 public:
  explicit EditorPlaceholderImageProvider(std::shared_ptr<const EditorPlaceholderImage> image);

  QImage requestImage(const QString& id, QSize* size, const QSize& requested_size) override;

 private:
  std::shared_ptr<const EditorPlaceholderImage> image_;
};

/**
 * @brief Chooses and keeps the placeholder of the editor viewport (DN14).
 *
 * While a photo waits for its first frame, the viewport shows, in this order: the library
 * thumbnail of the photo that a view of the application holds (the largest tier; it has the
 * committed edit and its crop), else the camera preview of the RAW file when the committed
 * Version has no crop and no rotation, else nothing. The thumbnail is copied at once, because the
 * thumbnail store drops an image when its last view releases it. The check of the Version and the
 * read of the camera preview run on a worker thread; a result for a photo that no longer waits is
 * dropped. The image stays until the viewport has composed the first frame of the photo
 * (@ref release), so the viewport never shows an empty frame between the two.
 *
 * It keeps no copy of the session: the module host calls @ref SetWaitingPhoto and
 * @ref SetFirstFrameReady at the state changes of the editor session.
 *
 * Thread: the GUI thread. The services are called on the GUI thread, except the check that
 * `crop_or_rotation_check` returns and `read_camera_preview`, which run on a worker thread.
 */
class EditorViewportPlaceholder final : public QObject {
  Q_OBJECT
  /// `image://alcedo-editor-placeholder/<revision>`; empty without a placeholder.
  Q_PROPERTY(QString source READ source NOTIFY SourceChanged)
  /// `thumbnail`, `camera_preview`, or empty.
  Q_PROPERTY(QString kind READ kind NOTIFY SourceChanged)

 public:
  /// An image of the thumbnail store with the encoding of its pixels.
  struct EncodedImage {
    QImage              image;
    ViewerDisplayConfig encoding{};
  };

  struct Services {
    /// The largest thumbnail of the element that the thumbnail store holds; empty without one.
    std::function<std::optional<EncodedImage>(sl_element_id_t)> held_thumbnail;
    /// The file of an image of the project; empty when it is unknown.
    std::function<std::filesystem::path(image_id_t)>            image_path;
    /// A check, made on the GUI thread and run on a worker thread, that is true when the
    /// committed Version of the element has a crop or a rotation; empty without a project. The
    /// check can throw (the photo then gets no camera preview).
    std::function<std::function<bool()>(sl_element_id_t)>       crop_or_rotation_check;
    /// The camera preview of the RAW file (EmbeddedPreviewReader), upright, in sRGB; null when
    /// the file has none. Runs on a worker thread; it can throw.
    std::function<QImage(const std::filesystem::path&)>         read_camera_preview;
  };

  /// @param image The image that the provider serves; this object writes it.
  EditorViewportPlaceholder(Services services, std::shared_ptr<EditorPlaceholderImage> image,
                            QObject* parent = nullptr);

  /**
   * @brief The photo that waits for its first frame; (0, 0) when the editor has no photo.
   *
   * A new photo chooses its placeholder; the same photo changes nothing; (0, 0) releases the
   * image.
   */
  void                      SetWaitingPhoto(sl_element_id_t element_id, image_id_t image_id);

  /**
   * @brief The first frame of the waiting photo is ready (the session is Interactive).
   *
   * A camera preview that arrives later is dropped. A shown image stays until @ref release.
   */
  void                      SetFirstFrameReady();

  /// The viewport composed a frame of the photo after the placeholder showed: release the image.
  Q_INVOKABLE void          release();

  [[nodiscard]] auto        source() const -> QString { return source_; }
  [[nodiscard]] auto        kind() const -> QString { return kind_; }

  /// The encoding of a camera preview: sRGB primaries and the sRGB curve.
  [[nodiscard]] static auto CameraPreviewEncoding() -> ViewerDisplayConfig;

 signals:
  void SourceChanged();

 private:
  void     Show(QString kind, QImage image, const ViewerDisplayConfig& encoding);
  void     Release();
  /// The camera preview read for the photo (@p element_id, @p image_id); dropped when that photo
  /// no longer waits.
  void     ApplyCameraPreview(sl_element_id_t element_id, image_id_t image_id, QImage preview);

  Services services_;
  std::shared_ptr<EditorPlaceholderImage> image_;
  sl_element_id_t                         element_id_        = 0;
  image_id_t                              image_id_          = 0;
  /// The first frame of the photo is ready: no new placeholder for it.
  bool                                    first_frame_ready_ = false;
  quint64                                 revision_          = 0;
  QString                                 source_;
  QString                                 kind_;
};

}  // namespace alcedo::ui
