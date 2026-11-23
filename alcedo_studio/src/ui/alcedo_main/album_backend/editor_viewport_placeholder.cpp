//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_viewport_placeholder.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <exception>
#include <utility>

#include "edit/operators/utils/color_utils.hpp"
#include "utils/logging/log.hpp"

namespace alcedo::ui {
namespace {

auto ProviderUrl(quint64 revision) -> QString {
  return QStringLiteral("image://%1/%2")
      .arg(QString::fromLatin1(kEditorPlaceholderImageProviderId))
      .arg(revision);
}

}  // namespace

EditorPlaceholderImage::EditorPlaceholderImage(DestinationSource destination)
    : destination_(std::move(destination)) {}

void EditorPlaceholderImage::Set(QImage image, const ViewerDisplayConfig& encoding) {
  std::scoped_lock lock(mutex_);
  image_    = std::move(image);
  encoding_ = encoding;
}

void EditorPlaceholderImage::Clear() {
  std::scoped_lock lock(mutex_);
  image_    = {};
  encoding_ = {};
}

auto EditorPlaceholderImage::Source() const -> QImage {
  std::scoped_lock lock(mutex_);
  return image_;
}

auto EditorPlaceholderImage::ForDisplay() const -> QImage {
  QImage              image;
  ViewerDisplayConfig encoding;
  {
    std::scoped_lock lock(mutex_);
    image    = image_;
    encoding = encoding_;
  }
  if (image.isNull()) {
    return image;
  }
  const auto destination = destination_ ? destination_() : nullptr;
  try {
    return ConvertImageForDestination(image.convertToFormat(QImage::Format_RGBA8888), encoding,
                                      destination.get());
  } catch (const std::exception& error) {
    ALCEDO_LOG_WARN(Render) << "Editor placeholder: the display conversion failed: "
                            << error.what();
    return image;
  }
}

EditorPlaceholderImageProvider::EditorPlaceholderImageProvider(
    std::shared_ptr<const EditorPlaceholderImage> image)
    : QQuickImageProvider(QQuickImageProvider::Image), image_(std::move(image)) {}

QImage EditorPlaceholderImageProvider::requestImage(const QString& /*id*/, QSize* size,
                                                    const QSize& requested_size) {
  QImage image = image_ ? image_->ForDisplay() : QImage{};
  if (!image.isNull() && requested_size.isValid() &&
      (image.width() > requested_size.width() || image.height() > requested_size.height())) {
    image = image.scaled(requested_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  }
  if (size != nullptr) {
    *size = image.size();
  }
  return image;
}

EditorViewportPlaceholder::EditorViewportPlaceholder(Services services,
                                                     std::shared_ptr<EditorPlaceholderImage> image,
                                                     QObject*                                parent)
    : QObject(parent), services_(std::move(services)), image_(std::move(image)) {}

auto EditorViewportPlaceholder::CameraPreviewEncoding() -> ViewerDisplayConfig {
  ViewerDisplayConfig encoding;
  encoding.encoding_space = ColorUtils::ColorSpace::REC709;
  encoding.encoding_eotf  = ColorUtils::EOTF::SRGB_PIECEWISE;
  return encoding;
}

void EditorViewportPlaceholder::SetWaitingPhoto(sl_element_id_t element_id, image_id_t image_id) {
  if (element_id == element_id_ && image_id == image_id_) {
    return;
  }
  element_id_        = element_id;
  image_id_          = image_id;
  first_frame_ready_ = false;
  Release();
  if (element_id == 0 || image_id == 0) {
    return;
  }

  // 1. The library thumbnail that a view holds: the committed edit with its crop.
  if (services_.held_thumbnail) {
    if (auto thumbnail = services_.held_thumbnail(element_id);
        thumbnail.has_value() && !thumbnail->image.isNull()) {
      Show(QStringLiteral("thumbnail"), std::move(thumbnail->image), thumbnail->encoding);
      return;
    }
  }

  // 2. The camera preview, only for a Version without crop and rotation: it shows the whole
  //    upright sensor image.
  if (!services_.image_path || !services_.crop_or_rotation_check ||
      !services_.read_camera_preview) {
    return;
  }
  const auto path = services_.image_path(image_id);
  if (path.empty()) {
    return;
  }
  auto has_geometry = services_.crop_or_rotation_check(element_id);
  if (!has_geometry) {
    return;
  }
  QPointer<EditorViewportPlaceholder> self(this);
  QThreadPool::globalInstance()->start([self, element_id, image_id, path,
                                        has_geometry = std::move(has_geometry),
                                        read         = services_.read_camera_preview]() {
    QImage preview;
    try {
      if (!has_geometry()) {
        preview = read(path);
      }
    } catch (const std::exception& error) {
      // The viewport keeps its label until the first frame.
      ALCEDO_LOG_WARN(Render) << "Editor placeholder: no camera preview: " << error.what();
    }
    QMetaObject::invokeMethod(
        self.data(),
        [self, element_id, image_id, preview = std::move(preview)]() mutable {
          if (self) {
            self->ApplyCameraPreview(element_id, image_id, std::move(preview));
          }
        },
        Qt::QueuedConnection);
  });
}

void EditorViewportPlaceholder::ApplyCameraPreview(sl_element_id_t element_id, image_id_t image_id,
                                                   QImage preview) {
  if (element_id != element_id_ || image_id != image_id_ || first_frame_ready_ ||
      preview.isNull()) {
    return;
  }
  Show(QStringLiteral("camera_preview"), std::move(preview), CameraPreviewEncoding());
}

void EditorViewportPlaceholder::Show(QString kind, QImage image,
                                     const ViewerDisplayConfig& encoding) {
  ALCEDO_LOG_INFO(Render) << "Editor placeholder: " << kind.toStdString() << " " << image.width()
                          << " x " << image.height() << " for element " << element_id_;
  image_->Set(std::move(image), encoding);
  ++revision_;
  source_ = ProviderUrl(revision_);
  kind_   = std::move(kind);
  emit SourceChanged();
}

void EditorViewportPlaceholder::SetFirstFrameReady() { first_frame_ready_ = true; }

void EditorViewportPlaceholder::release() { Release(); }

void EditorViewportPlaceholder::Release() {
  if (source_.isEmpty()) {
    return;
  }
  image_->Clear();
  source_.clear();
  kind_.clear();
  emit SourceChanged();
}

}  // namespace alcedo::ui
