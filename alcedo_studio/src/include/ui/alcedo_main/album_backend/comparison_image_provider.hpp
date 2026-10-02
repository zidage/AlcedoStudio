//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QImage>
#include <QQuickImageProvider>
#include <QSize>
#include <QString>
#include <QVariantMap>
#include <cstdint>
#include <memory>
#include <mutex>

#include "edit/runtime/rendered_pipeline_image.hpp"
#include "ui/alcedo_main/album_backend/comparison_presentation_image.hpp"

namespace alcedo::ui {

inline constexpr const char* kComparisonImageProviderId = "alcedo-comparison";

/// Position of an image in a comparison pair.
enum class ComparisonSide { A, B };

/**
 * @brief The one ready comparison pair that QML can load, keyed by its comparison operation.
 *
 * Holds two completed 8-bit images and nothing else: no document, render result, or float
 * pixels. PublishPair replaces both images in one locked write, so a reader never sees a new
 * image on one side and an old image on the other. A URL contains the operation id and the
 * side; a request for an operation that is no longer stored returns a null image.
 *
 * Thread: all members are safe to call from any thread. The QML image provider reads on the
 * Qt image loader thread while the GUI thread publishes or clears.
 *
 * Lifetime: a QImage that Get returned shares its pixels with the store (implicit sharing).
 * It stays valid after Clear or a later PublishPair; the pixels are released when the last
 * holder drops it.
 */
class ComparisonImageStore {
 public:
  /// Provider URLs of one published pair.
  struct PairUrls {
    QString a;
    QString b;
  };

  /**
   * @brief Replaces the stored pair with @p a and @p b for @p operation_id.
   * @throws std::invalid_argument when @p operation_id is 0 or an image is null. The stored
   *         pair is unchanged on failure.
   */
  auto                      PublishPair(std::uint64_t operation_id, QImage a, QImage b) -> PairUrls;

  /// Releases the stored pair. Images already returned by Get stay valid.
  void                      Clear();

  /// The image of @p side when @p operation_id is the stored operation; otherwise null.
  [[nodiscard]] auto        Get(std::uint64_t operation_id, ComparisonSide side) const -> QImage;

  /// Number of stored images: 0 or 2.
  [[nodiscard]] auto        ImageCount() const -> int;

  [[nodiscard]] static auto MakeUrl(std::uint64_t operation_id, ComparisonSide side) -> QString;
  /// Parses the provider id `<operation>/<a|b>` of a URL from MakeUrl.
  [[nodiscard]] static auto ParseProviderId(const QString& id, std::uint64_t* operation_id,
                                            ComparisonSide* side) -> bool;

 private:
  mutable std::mutex mutex_;
  std::uint64_t      operation_id_ = 0;
  QImage             a_;
  QImage             b_;
};

/**
 * @brief Serves stored comparison images to QML `Image` items.
 *
 * Only returns completed images from the store. It never decodes or renders, and it ignores
 * the requested size so that the image keeps its exact render extent.
 */
class ComparisonImageProvider final : public QQuickImageProvider {
 public:
  explicit ComparisonImageProvider(std::shared_ptr<ComparisonImageStore> store);

  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

 private:
  std::shared_ptr<ComparisonImageStore> store_;
};

/// The application's comparison image store, shared by the provider and the comparison UI.
[[nodiscard]] auto SharedComparisonImageStore() -> std::shared_ptr<ComparisonImageStore>;

/**
 * @brief A published pair as QML reads it: provider URLs and the placement of each image.
 */
struct ComparisonPairPublication {
  std::uint64_t            operation_id = 0;
  QString                  a_url;
  QString                  b_url;
  ComparisonImagePlacement a_placement{};
  ComparisonImagePlacement b_placement{};

  /// `operationId`, `aSource`, `bSource`, `aPlacement`, and `bPlacement` for QML.
  [[nodiscard]] auto       ToVariantMap() const -> QVariantMap;
};

/**
 * @brief Converts a rendered pair to SDR images and publishes both into @p store together.
 *
 * Both images are converted before the store changes. Both must have the same full reference
 * extent: they are two adjustment states of one image.
 *
 * @throws std::invalid_argument with the reason when either conversion fails or the reference
 *         extents differ. The store keeps its previous pair, so a failure never publishes one
 *         side.
 */
[[nodiscard]] auto PublishComparisonPair(ComparisonImageStore& store, std::uint64_t operation_id,
                                         const RenderedPipelineImage& a,
                                         const RenderedPipelineImage& b)
    -> ComparisonPairPublication;

}  // namespace alcedo::ui
