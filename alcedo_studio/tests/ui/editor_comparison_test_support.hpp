//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_comparison_test_support.hpp
/// @brief Rendered comparison images with real resolved geometry, and a QML image provider that
///        counts and can hold its reads, for the comparison presentation tests.

#pragma once

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QImage>
#include <QPointer>
#include <QQuickImageProvider>
#include <QSize>
#include <QString>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <utility>
#include <vector>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "edit/runtime/rendered_pipeline_image.hpp"
#include "image/image_buffer.hpp"
#include "ui/alcedo_main/album_backend/comparison_image_provider.hpp"

namespace alcedo::ui::test {

/// Real renderer geometry for a full-source render with @p crop and @p rotation_degrees.
inline auto ResolveComparisonGeometry(Extent2D source, NormalizedRect crop = {},
                                      float rotation_degrees = 0.0f, std::uint32_t max_edge = 0)
    -> ResolvedRenderGeometry {
  ImageGeometryParams image;
  image.crop_rect        = crop;
  image.rotation_degrees = rotation_degrees;
  ResolutionRequest resolution;
  resolution.max_edge = max_edge;
  return ResolveRenderGeometry(MakeSourceGeometry(source, source), image, ViewRequest{}, resolution,
                               SamplingFootprint{});
}

/// RGBA32F pixels of the render extent of @p geometry, filled with @p rgba.
inline auto FilledPixelsFor(const ResolvedRenderGeometry& geometry, cv::Scalar rgba) -> cv::Mat {
  return cv::Mat(static_cast<int>(geometry.render_extent.height),
                 static_cast<int>(geometry.render_extent.width), CV_32FC4, rgba);
}

/// A RenderedPipelineImage as Renderer::RenderImage returns it: host pixels and geometry.
inline auto MakeRenderedImage(cv::Mat pixels, const ResolvedRenderGeometry& geometry,
                              ViewerDisplayConfig display = {}) -> RenderedPipelineImage {
  RenderedPipelineImage rendered;
  rendered.pixels   = std::make_shared<ImageBuffer>(std::move(pixels));
  rendered.geometry = geometry;
  rendered.display  = display;
  return rendered;
}

inline auto MakeFilledRenderedImage(const ResolvedRenderGeometry& geometry, cv::Scalar rgba)
    -> RenderedPipelineImage {
  return MakeRenderedImage(FilledPixelsFor(geometry, rgba), geometry);
}

/**
 * @brief Wraps the production ComparisonImageProvider for QML tests. Counts reads and can hold
 *        the completion of one side after the store returned its image, until the test releases
 *        it.
 *
 * Asynchronous, so a held side does not block the other side: Qt's pixmap reader serves
 * synchronous provider reads one at a time.
 */
class ObservedComparisonImageProvider final : public QQuickAsyncImageProvider {
 public:
  class Response final : public QQuickImageResponse {
   public:
    explicit Response(QImage image) : image_(std::move(image)) {}
    QQuickTextureFactory* textureFactory() const override {
      return QQuickTextureFactory::textureFactoryForImage(image_);
    }
    void Finish() { emit finished(); }

   private:
    QImage image_;
  };

  struct Observer {
    std::mutex                      mutex;
    int                             reads      = 0;
    int                             held_reads = 0;
    bool                            hold_a     = false;
    bool                            hold_b     = false;
    std::vector<QPointer<Response>> held_a;
    std::vector<QPointer<Response>> held_b;
    QImage                          last_image_a;
    QImage                          last_image_b;

    /// Holds later reads of @p side, or finishes the held reads of @p side.
    void                            Hold(ComparisonSide side, bool hold) {
      std::vector<QPointer<Response>> release;
      {
        std::lock_guard lock(mutex);
        (side == ComparisonSide::A ? hold_a : hold_b) = hold;
        if (!hold) {
          release.swap(side == ComparisonSide::A ? held_a : held_b);
        }
      }
      for (auto& response : release) {
        if (response) {
          response->Finish();
        }
      }
    }
    auto Reads() -> int {
      std::lock_guard lock(mutex);
      return reads;
    }
    auto HeldReads() -> int {
      std::lock_guard lock(mutex);
      return held_reads;
    }
    void DropLastImages() {
      std::lock_guard lock(mutex);
      last_image_a = {};
      last_image_b = {};
    }
  };

  ObservedComparisonImageProvider(std::shared_ptr<ComparisonImageStore> store,
                                  std::shared_ptr<Observer>             observer)
      : inner_(std::move(store)), observer_(std::move(observer)) {}

  QQuickImageResponse* requestImageResponse(const QString& id,
                                            const QSize&   requested_size) override {
    QImage         image        = inner_.requestImage(id, nullptr, requested_size);
    std::uint64_t  operation_id = 0;
    ComparisonSide side         = ComparisonSide::A;
    (void)ComparisonImageStore::ParseProviderId(id, &operation_id, &side);
    auto* response = new Response(image);
    bool  held     = false;
    {
      std::lock_guard lock(observer_->mutex);
      ++observer_->reads;
      (side == ComparisonSide::A ? observer_->last_image_a : observer_->last_image_b) = image;
      if (side == ComparisonSide::A ? observer_->hold_a : observer_->hold_b) {
        ++observer_->held_reads;
        (side == ComparisonSide::A ? observer_->held_a : observer_->held_b).emplace_back(response);
        held = true;
      }
    }
    if (!held) {
      response->Finish();
    }
    return response;
  }

 private:
  ComparisonImageProvider   inner_;
  std::shared_ptr<Observer> observer_;
};

/// Processes Qt events until @p condition is true or @p timeout_ms passes.
inline auto WaitUntil(const std::function<bool()>& condition, int timeout_ms = 5000) -> bool {
  QDeadlineTimer deadline(timeout_ms);
  while (!condition()) {
    if (deadline.hasExpired()) {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
  return true;
}

inline auto ComparisonQmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

}  // namespace alcedo::ui::test
