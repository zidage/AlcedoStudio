//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/comparison_image_provider.hpp"

#include <QStringList>
#include <stdexcept>
#include <string>
#include <utility>

namespace alcedo::ui {
namespace {

auto SideName(ComparisonSide side) -> QLatin1String {
  return side == ComparisonSide::A ? QLatin1String("a") : QLatin1String("b");
}

auto ConvertSide(const RenderedPipelineImage& rendered, const char* side_name)
    -> ComparisonPresentationImage {
  try {
    return ConvertRenderedImageForComparison(rendered);
  } catch (const std::invalid_argument& error) {
    throw std::invalid_argument(std::string("Comparison image ") + side_name + ": " + error.what());
  }
}

}  // namespace

auto ComparisonImageStore::MakeUrl(std::uint64_t operation_id, ComparisonSide side,
                                   std::uint64_t destination_revision) -> QString {
  return QStringLiteral("image://%1/%2/%3/%4")
      .arg(QLatin1String(kComparisonImageProviderId))
      .arg(operation_id)
      .arg(SideName(side))
      .arg(destination_revision);
}

auto ComparisonImageStore::ParseProviderId(const QString& id, std::uint64_t* operation_id,
                                           ComparisonSide* side) -> bool {
  const QStringList parts = id.split(QLatin1Char('/'));
  if (parts.size() != 2 && parts.size() != 3) {
    return false;
  }
  bool       ok     = false;
  const auto parsed = parts[0].toULongLong(&ok);
  if (!ok || parsed == 0) {
    return false;
  }
  if (parts.size() == 3) {
    // The destination revision is for the QML image cache only.
    (void)parts[2].toULongLong(&ok);
    if (!ok) {
      return false;
    }
  }
  ComparisonSide parsed_side = ComparisonSide::A;
  if (parts[1] == SideName(ComparisonSide::A)) {
    parsed_side = ComparisonSide::A;
  } else if (parts[1] == SideName(ComparisonSide::B)) {
    parsed_side = ComparisonSide::B;
  } else {
    return false;
  }
  if (operation_id != nullptr) {
    *operation_id = parsed;
  }
  if (side != nullptr) {
    *side = parsed_side;
  }
  return true;
}

auto ComparisonImageStore::PublishPair(std::uint64_t operation_id, QImage a,
                                       const ViewerDisplayConfig& a_encoding, QImage b,
                                       const ViewerDisplayConfig& b_encoding) -> PairUrls {
  if (operation_id == 0) {
    throw std::invalid_argument("Comparison pair needs a comparison operation id.");
  }
  if (a.isNull() || b.isNull()) {
    throw std::invalid_argument("Comparison pair needs two images.");
  }
  QImage        released_a;
  QImage        released_b;
  std::uint64_t revision = 0;
  {
    std::lock_guard lock(mutex_);
    operation_id_ = operation_id;
    released_a    = std::exchange(a_, std::move(a));
    released_b    = std::exchange(b_, std::move(b));
    a_encoding_   = a_encoding;
    b_encoding_   = b_encoding;
    revision      = destination_revision_;
  }
  // The previous images are released outside the lock.
  return PairUrls{MakeUrl(operation_id, ComparisonSide::A, revision),
                  MakeUrl(operation_id, ComparisonSide::B, revision)};
}

auto ComparisonImageStore::SetDestination(std::shared_ptr<const DisplayDestination> destination)
    -> std::optional<PairUrls> {
  std::lock_guard lock(mutex_);
  if (converter_.Destination() == destination) {
    return std::nullopt;
  }
  converter_.SetDestination(std::move(destination));
  ++destination_revision_;
  if (operation_id_ == 0) {
    return std::nullopt;
  }
  return PairUrls{MakeUrl(operation_id_, ComparisonSide::A, destination_revision_),
                  MakeUrl(operation_id_, ComparisonSide::B, destination_revision_)};
}

void ComparisonImageStore::Clear() {
  QImage released_a;
  QImage released_b;
  {
    std::lock_guard lock(mutex_);
    operation_id_ = 0;
    released_a    = std::exchange(a_, QImage{});
    released_b    = std::exchange(b_, QImage{});
  }
}

auto ComparisonImageStore::Get(std::uint64_t operation_id, ComparisonSide side) const -> QImage {
  std::lock_guard lock(mutex_);
  if (operation_id == 0 || operation_id != operation_id_) {
    return {};
  }
  return side == ComparisonSide::A ? a_ : b_;
}

auto ComparisonImageStore::GetForDisplay(std::uint64_t operation_id, ComparisonSide side) const
    -> QImage {
  QImage              image;
  ViewerDisplayConfig encoding{};
  {
    std::lock_guard lock(mutex_);
    if (operation_id == 0 || operation_id != operation_id_) {
      return {};
    }
    image    = side == ComparisonSide::A ? a_ : b_;
    encoding = side == ComparisonSide::A ? a_encoding_ : b_encoding_;
  }
  // The conversion runs outside the lock: the GUI thread can publish meanwhile.
  return converter_.Convert(image, encoding);
}

auto ComparisonImageStore::ImageCount() const -> int {
  std::lock_guard lock(mutex_);
  return (a_.isNull() ? 0 : 1) + (b_.isNull() ? 0 : 1);
}

ComparisonImageProvider::ComparisonImageProvider(std::shared_ptr<ComparisonImageStore> store)
    : QQuickImageProvider(QQuickImageProvider::Image), store_(std::move(store)) {}

QImage ComparisonImageProvider::requestImage(const QString& id, QSize* size,
                                             const QSize& /*requestedSize*/) {
  std::uint64_t  operation_id = 0;
  ComparisonSide side         = ComparisonSide::A;
  QImage         image;
  if (store_ && ComparisonImageStore::ParseProviderId(id, &operation_id, &side)) {
    image = store_->GetForDisplay(operation_id, side);
  }
  if (size != nullptr) {
    *size = image.size();
  }
  return image;
}

auto SharedComparisonImageStore() -> std::shared_ptr<ComparisonImageStore> {
  static auto store = std::make_shared<ComparisonImageStore>();
  return store;
}

auto ComparisonPairPublication::ToVariantMap() const -> QVariantMap {
  return QVariantMap{
      {QStringLiteral("operationId"), QVariant::fromValue<qulonglong>(operation_id)},
      {QStringLiteral("aSource"), a_url},
      {QStringLiteral("bSource"), b_url},
      {QStringLiteral("aPlacement"), a_placement.ToVariantMap()},
      {QStringLiteral("bPlacement"), b_placement.ToVariantMap()},
  };
}

auto PublishComparisonPair(ComparisonImageStore& store, std::uint64_t operation_id,
                           const RenderedPipelineImage& a, const RenderedPipelineImage& b)
    -> ComparisonPairPublication {
  auto a_image = ConvertSide(a, "A");
  auto b_image = ConvertSide(b, "B");
  if (a_image.placement.reference_extent != b_image.placement.reference_extent) {
    throw std::invalid_argument("Comparison images have different reference extents.");
  }
  const auto urls = store.PublishPair(operation_id, std::move(a_image.image), a.display,
                                      std::move(b_image.image), b.display);
  return ComparisonPairPublication{operation_id, urls.a, urls.b, a_image.placement,
                                   b_image.placement};
}

}  // namespace alcedo::ui
