//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_viewport_placeholder_test.cpp
/// @brief The image of the editor viewport before the first frame of a photo (AI Denoise
///        integration plan, DN14): the held library thumbnail first, the camera preview only for
///        a Version without crop and rotation, a result for a photo that no longer waits dropped,
///        and the release when the photo stops waiting.

#include "ui/alcedo_main/album_backend/editor_viewport_placeholder.hpp"

#include <gtest/gtest.h>

#include <QColor>
#include <QImage>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace alcedo::ui {
namespace {

auto FilledImage(int width, int height, QColor color) -> QImage {
  QImage image(width, height, QImage::Format_RGBA8888);
  image.fill(color);
  return image;
}

/// Test services: element 1 has a held thumbnail; element 2 has none and no geometry; element 3
/// has none and a crop; element 4 has none and its check throws.
struct TestServices {
  std::atomic<int>  preview_reads{0};
  std::atomic<int>  geometry_checks{0};
  std::atomic<bool> read_on_gui_thread{false};
  QThread*          gui_thread = QThread::currentThread();
  QImage            thumbnail  = FilledImage(512, 341, Qt::red);
  QImage            preview    = FilledImage(800, 533, Qt::blue);

  auto              Make() -> EditorViewportPlaceholder::Services {
    EditorViewportPlaceholder::Services services;
    services.held_thumbnail =
        [this](
            sl_element_id_t element_id) -> std::optional<EditorViewportPlaceholder::EncodedImage> {
      if (element_id != 1) {
        return std::nullopt;
      }
      return EditorViewportPlaceholder::EncodedImage{thumbnail, ViewerDisplayConfig{}};
    };
    services.image_path = [](image_id_t image_id) -> std::filesystem::path {
      return image_id == 0 ? std::filesystem::path{}
                                        : std::filesystem::path("photo_" + std::to_string(image_id) + ".ARW");
    };
    services.crop_or_rotation_check = [this](sl_element_id_t element_id) -> std::function<bool()> {
      return [this, element_id] {
        ++geometry_checks;
        if (element_id == 4) {
          throw std::runtime_error("no history root");
        }
        return element_id == 3;
      };
    };
    services.read_camera_preview = [this](const std::filesystem::path&) {
      ++preview_reads;
      if (QThread::currentThread() == gui_thread) {
        read_on_gui_thread = true;
      }
      return preview;
    };
    return services;
  }
};

auto SourceBecomesSet(const EditorViewportPlaceholder& placeholder) -> bool {
  return QTest::qWaitFor([&] { return !placeholder.source().isEmpty(); }, 5000);
}

TEST(EditorViewportPlaceholderTest, PlaceholderUsesTheLargestHeldThumbnail) {
  TestServices              services;
  auto                      image = std::make_shared<EditorPlaceholderImage>();
  EditorViewportPlaceholder placeholder(services.Make(), image);
  QSignalSpy                changed(&placeholder, &EditorViewportPlaceholder::SourceChanged);

  placeholder.SetWaitingPhoto(1, 10);
  EXPECT_EQ(placeholder.kind(), QStringLiteral("thumbnail"));
  EXPECT_TRUE(placeholder.source().startsWith(QStringLiteral("image://alcedo-editor-placeholder/")))
      << placeholder.source().toStdString();
  EXPECT_EQ(changed.count(), 1);
  // The image is a copy: the store can drop its own.
  services.thumbnail = FilledImage(4, 4, Qt::green);
  EXPECT_EQ(image->Source().size(), QSize(512, 341));
  EXPECT_EQ(image->Source().pixelColor(0, 0), QColor(Qt::red));
  // The provider serves it (no destination: the source pixels).
  EditorPlaceholderImageProvider provider(image);
  QSize                          size;
  const auto served = provider.requestImage(QStringLiteral("1"), &size, QSize());
  EXPECT_EQ(size, QSize(512, 341));
  EXPECT_EQ(served.pixelColor(10, 10), QColor(Qt::red));
  // No camera preview is read when a thumbnail is held.
  QTest::qWait(50);
  EXPECT_EQ(services.preview_reads.load(), 0);
  EXPECT_EQ(services.geometry_checks.load(), 0);

  // The same photo again changes nothing.
  placeholder.SetWaitingPhoto(1, 10);
  EXPECT_EQ(changed.count(), 1);
}

TEST(EditorViewportPlaceholderTest, PlaceholderReadsTheCameraPreviewOnlyWithoutCropAndRotation) {
  TestServices              services;
  auto                      image = std::make_shared<EditorPlaceholderImage>();
  EditorViewportPlaceholder placeholder(services.Make(), image);

  // No thumbnail, no crop: the camera preview, read on a worker thread.
  placeholder.SetWaitingPhoto(2, 20);
  EXPECT_TRUE(placeholder.source().isEmpty());
  ASSERT_TRUE(SourceBecomesSet(placeholder));
  EXPECT_EQ(placeholder.kind(), QStringLiteral("camera_preview"));
  EXPECT_EQ(image->Source().size(), QSize(800, 533));
  EXPECT_EQ(services.preview_reads.load(), 1);
  EXPECT_FALSE(services.read_on_gui_thread.load());

  // A crop: no placeholder (the label shows).
  placeholder.SetWaitingPhoto(3, 30);
  EXPECT_TRUE(placeholder.source().isEmpty());
  EXPECT_TRUE(QTest::qWaitFor([&] { return services.geometry_checks.load() == 2; }, 5000));
  QTest::qWait(100);
  EXPECT_TRUE(placeholder.source().isEmpty());
  EXPECT_TRUE(image->Source().isNull());
  EXPECT_EQ(services.preview_reads.load(), 1);

  // A Version whose geometry cannot be read: no placeholder either.
  placeholder.SetWaitingPhoto(4, 40);
  EXPECT_TRUE(QTest::qWaitFor([&] { return services.geometry_checks.load() == 3; }, 5000));
  QTest::qWait(100);
  EXPECT_TRUE(placeholder.source().isEmpty());
}

TEST(EditorViewportPlaceholderTest, PlaceholderOfAPhotoThatIsNoLongerOpenIsDropped) {
  TestServices              services;
  auto                      image = std::make_shared<EditorPlaceholderImage>();
  EditorViewportPlaceholder placeholder(services.Make(), image);

  // The camera preview of photo 2 arrives after the photo stopped waiting.
  placeholder.SetWaitingPhoto(2, 20);
  placeholder.SetWaitingPhoto(0, 0);
  EXPECT_TRUE(QTest::qWaitFor([&] { return services.preview_reads.load() == 1; }, 5000));
  QTest::qWait(100);
  EXPECT_TRUE(placeholder.source().isEmpty());
  EXPECT_TRUE(image->Source().isNull());

  // Another photo opens before the preview of the first arrives: only its own image shows.
  placeholder.SetWaitingPhoto(2, 20);
  placeholder.SetWaitingPhoto(1, 10);
  EXPECT_EQ(placeholder.kind(), QStringLiteral("thumbnail"));
  QTest::qWait(150);
  EXPECT_EQ(placeholder.kind(), QStringLiteral("thumbnail"));
  EXPECT_EQ(image->Source().size(), QSize(512, 341));
}

TEST(EditorViewportPlaceholderTest, PlaceholderIsReleasedWhenThePhotoStopsWaiting) {
  TestServices              services;
  auto                      image = std::make_shared<EditorPlaceholderImage>();
  EditorViewportPlaceholder placeholder(services.Make(), image);
  QSignalSpy                changed(&placeholder, &EditorViewportPlaceholder::SourceChanged);

  placeholder.SetWaitingPhoto(1, 10);
  const auto first = placeholder.source();
  ASSERT_FALSE(first.isEmpty());
  // The editor closes the photo: the image is released.
  placeholder.SetWaitingPhoto(0, 0);
  EXPECT_TRUE(placeholder.source().isEmpty());
  EXPECT_TRUE(placeholder.kind().isEmpty());
  EXPECT_TRUE(image->Source().isNull());
  EXPECT_EQ(changed.count(), 2);

  // The next open has a new URL, so the QML image does not show a cached old image.
  placeholder.SetWaitingPhoto(1, 10);
  EXPECT_NE(placeholder.source(), first);
}

TEST(EditorViewportPlaceholderTest, PlaceholderStaysUntilTheViewportComposesTheFirstFrame) {
  TestServices              services;
  auto                      image = std::make_shared<EditorPlaceholderImage>();
  EditorViewportPlaceholder placeholder(services.Make(), image);

  placeholder.SetWaitingPhoto(1, 10);
  ASSERT_FALSE(placeholder.source().isEmpty());
  // The first frame is ready (Interactive), but the viewport has not composed it yet.
  placeholder.SetFirstFrameReady();
  EXPECT_FALSE(placeholder.source().isEmpty());
  EXPECT_FALSE(image->Source().isNull());
  // The viewport composed it (QML).
  placeholder.release();
  EXPECT_TRUE(placeholder.source().isEmpty());
  EXPECT_TRUE(image->Source().isNull());
  // The same photo stays open: it chooses no new placeholder.
  placeholder.SetWaitingPhoto(1, 10);
  EXPECT_TRUE(placeholder.source().isEmpty());
}

TEST(EditorViewportPlaceholderTest, CameraPreviewAfterTheFirstFrameIsDropped) {
  TestServices              services;
  auto                      image = std::make_shared<EditorPlaceholderImage>();
  EditorViewportPlaceholder placeholder(services.Make(), image);

  // The first frame is ready before the camera preview arrives.
  placeholder.SetWaitingPhoto(2, 20);
  placeholder.SetFirstFrameReady();
  EXPECT_TRUE(QTest::qWaitFor([&] { return services.preview_reads.load() == 1; }, 5000));
  QTest::qWait(100);
  EXPECT_TRUE(placeholder.source().isEmpty());
  EXPECT_TRUE(image->Source().isNull());
}

}  // namespace
}  // namespace alcedo::ui
