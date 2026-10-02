//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_comparison_image_provider_test.cpp
/// @brief SDR conversion of rendered comparison images, pair publication, and provider reads.

#include <gtest/gtest.h>

#include <QColor>
#include <QImage>
#include <QSize>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <string>
#include <thread>

#include "editor_comparison_test_support.hpp"
#include "ui/alcedo_main/album_backend/comparison_image_provider.hpp"
#include "ui/alcedo_main/album_backend/comparison_presentation_image.hpp"

namespace alcedo::ui::test {
namespace {

constexpr Extent2D kSource{40, 30};

auto               PixelRgba(const QImage& image, int x, int y) -> std::array<int, 4> {
  const QRgb value = image.pixel(x, y);
  return {qRed(value), qGreen(value), qBlue(value), qAlpha(value)};
}

auto ExpectInvalid(const RenderedPipelineImage& rendered, const std::string& expected_reason) {
  try {
    (void)ConvertRenderedImageForComparison(rendered);
    ADD_FAILURE() << "conversion accepted an image that should fail: " << expected_reason;
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find(expected_reason), std::string::npos)
        << "actual reason: " << error.what();
  }
}

// Tags both sides of one operation so a read shows which publication it came from.
auto TaggedPair(int tag) -> std::pair<QImage, QImage> {
  QImage a(4, 3, QImage::Format_RGBA8888);
  QImage b(4, 3, QImage::Format_RGBA8888);
  a.fill(QColor(tag, 10, 0, 255));
  b.fill(QColor(tag, 20, 0, 255));
  return {a, b};
}

}  // namespace

TEST(EditorComparisonImageProviderTest, SdrPairConversionMatchesRoundedClampedRgbaValues) {
  const auto geometry = ResolveComparisonGeometry(Extent2D{3, 2});
  ASSERT_EQ(geometry.render_extent, (Extent2D{3, 2}));

  cv::Mat pixels(2, 3, CV_32FC4);
  // Distinct values per channel show the channel order. Values outside [0, 1] clamp; fractions
  // round to the nearest 8-bit value: 0.61 * 255 = 155.55 -> 156, 0.0039 * 255 = 0.99 -> 1.
  pixels.at<cv::Vec4f>(0, 0) = {1.0f, 0.0f, 0.2f, 1.0f};
  pixels.at<cv::Vec4f>(0, 1) = {0.4f, 0.61f, 0.0039f, 0.8f};
  pixels.at<cv::Vec4f>(0, 2) = {1.5f, -0.2f, 2.0f, 0.0f};
  pixels.at<cv::Vec4f>(1, 0) = {0.0f, 1.0f, 0.0f, 1.0f};
  pixels.at<cv::Vec4f>(1, 1) = {0.0f, 0.0f, 1.0f, 1.0f};
  pixels.at<cv::Vec4f>(1, 2) = {0.996f, 0.502f, 0.251f, 0.749f};

  auto        rendered       = MakeRenderedImage(pixels.clone(), geometry);
  const auto* source_bytes   = rendered.pixels->GetCPUData().data;
  auto        converted      = ConvertRenderedImageForComparison(rendered);

  // The float output can be released; the 8-bit image owns its own pixels.
  rendered.pixels.reset();

  ASSERT_EQ(converted.image.format(), QImage::Format_RGBA8888);
  ASSERT_EQ(converted.image.size(), QSize(3, 2));
  EXPECT_NE(static_cast<const void*>(converted.image.constBits()),
            static_cast<const void*>(source_bytes));
  EXPECT_EQ(PixelRgba(converted.image, 0, 0), (std::array<int, 4>{255, 0, 51, 255}));
  EXPECT_EQ(PixelRgba(converted.image, 1, 0), (std::array<int, 4>{102, 156, 1, 204}));
  EXPECT_EQ(PixelRgba(converted.image, 2, 0), (std::array<int, 4>{255, 0, 255, 0}));
  EXPECT_EQ(PixelRgba(converted.image, 0, 1), (std::array<int, 4>{0, 255, 0, 255}));
  EXPECT_EQ(PixelRgba(converted.image, 1, 1), (std::array<int, 4>{0, 0, 255, 255}));
  EXPECT_EQ(PixelRgba(converted.image, 2, 1), (std::array<int, 4>{254, 128, 64, 191}));

  // The raw RGBA8888 bytes keep R, G, B, A order (QImage::pixel converts for reading).
  const uchar* row0 = converted.image.constScanLine(0);
  EXPECT_EQ(row0[4], 102);
  EXPECT_EQ(row0[5], 156);
  EXPECT_EQ(row0[6], 1);
  EXPECT_EQ(row0[7], 204);

  EXPECT_EQ(converted.placement.reference_extent, geometry.full_reference_extent);
  EXPECT_EQ(converted.placement.render_extent, geometry.render_extent);
}

TEST(EditorComparisonImageProviderTest, ConversionRejectsInvalidPixelsGeometryAndHdrOutput) {
  const auto geometry = ResolveComparisonGeometry(kSource);

  ExpectInvalid(RenderedPipelineImage{nullptr, geometry, {}}, "no host pixels");

  auto no_cpu = MakeFilledRenderedImage(geometry, cv::Scalar(0.5, 0.5, 0.5, 1.0));
  no_cpu.pixels->cpu_data_valid_ = false;
  ExpectInvalid(no_cpu, "no host pixels");

  ExpectInvalid(MakeRenderedImage(cv::Mat(30, 40, CV_32FC3, cv::Scalar(0.5, 0.5, 0.5)), geometry),
                "not RGBA32F");

  ExpectInvalid(MakeRenderedImage(cv::Mat(30, 39, CV_32FC4, cv::Scalar::all(0.5)), geometry),
                "39x30 pixels, but its render geometry is 40x30");

  auto nan_pixels                    = FilledPixelsFor(geometry, cv::Scalar::all(0.5));
  nan_pixels.at<cv::Vec4f>(7, 11)[1] = std::numeric_limits<float>::quiet_NaN();
  ExpectInvalid(MakeRenderedImage(nan_pixels, geometry), "non-finite");

  auto inf_pixels                   = FilledPixelsFor(geometry, cv::Scalar::all(0.5));
  inf_pixels.at<cv::Vec4f>(0, 0)[3] = std::numeric_limits<float>::infinity();
  ExpectInvalid(MakeRenderedImage(inf_pixels, geometry), "non-finite");

  for (const auto eotf : {ColorUtils::EOTF::ST2084, ColorUtils::EOTF::HLG}) {
    ViewerDisplayConfig hdr;
    hdr.encoding_eotf = eotf;
    ExpectInvalid(MakeRenderedImage(FilledPixelsFor(geometry, cv::Scalar::all(0.5)), geometry, hdr),
                  eotf == ColorUtils::EOTF::ST2084 ? "ST 2084" : "HLG");
  }

  auto singular                = geometry;
  singular.render_to_reference = Matrix3x3::Scale(0.0f, 1.0f);
  ExpectInvalid(MakeFilledRenderedImage(singular, cv::Scalar::all(0.5)), "singular");

  auto empty_reference                  = geometry;
  empty_reference.full_reference_extent = Extent2D{};
  ExpectInvalid(MakeFilledRenderedImage(empty_reference, cv::Scalar::all(0.5)),
                "empty reference extent");
}

TEST(EditorComparisonImageProviderTest, PairProviderNeverPublishesOneNewSideWithOneOldSide) {
  ComparisonImageStore store;
  const auto           full    = ResolveComparisonGeometry(kSource);
  const auto           cropped = ResolveComparisonGeometry(kSource, NormalizedRect{0, 0, 0.5f, 1});

  const auto           first =
      PublishComparisonPair(store, 11, MakeFilledRenderedImage(full, cv::Scalar(1, 0, 0, 1)),
                            MakeFilledRenderedImage(cropped, cv::Scalar(0, 1, 0, 1)));
  EXPECT_EQ(first.a_url, QStringLiteral("image://alcedo-comparison/11/a"));
  EXPECT_EQ(first.b_url, QStringLiteral("image://alcedo-comparison/11/b"));
  EXPECT_EQ(store.ImageCount(), 2);

  // A new pair whose B side is invalid publishes nothing: the first pair stays whole.
  auto invalid_b = MakeFilledRenderedImage(full, cv::Scalar::all(0.5));
  invalid_b.pixels->GetCPUData().at<cv::Vec4f>(0, 0)[0] = std::numeric_limits<float>::quiet_NaN();
  try {
    (void)PublishComparisonPair(store, 12, MakeFilledRenderedImage(full, cv::Scalar(0, 0, 1, 1)),
                                invalid_b);
    ADD_FAILURE() << "a pair with an invalid B side was published";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("Comparison image B"), std::string::npos);
  }
  EXPECT_TRUE(store.Get(12, ComparisonSide::A).isNull());
  EXPECT_TRUE(store.Get(12, ComparisonSide::B).isNull());
  ASSERT_FALSE(store.Get(11, ComparisonSide::A).isNull());
  ASSERT_FALSE(store.Get(11, ComparisonSide::B).isNull());
  EXPECT_EQ(qRed(store.Get(11, ComparisonSide::A).pixel(0, 0)), 255);
  EXPECT_EQ(qGreen(store.Get(11, ComparisonSide::B).pixel(0, 0)), 255);
  EXPECT_EQ(store.Get(11, ComparisonSide::B).size(),
            QSize(static_cast<int>(cropped.render_extent.width),
                  static_cast<int>(cropped.render_extent.height)));

  // Images of different reference extents are not one image's pair.
  const auto other_source = ResolveComparisonGeometry(Extent2D{41, 30});
  EXPECT_THROW(
      (void)PublishComparisonPair(store, 13, MakeFilledRenderedImage(full, cv::Scalar::all(0.5)),
                                  MakeFilledRenderedImage(other_source, cv::Scalar::all(0.5))),
      std::invalid_argument);
  EXPECT_FALSE(store.Get(11, ComparisonSide::A).isNull());

  // A valid replacement replaces both sides at once.
  const auto second =
      PublishComparisonPair(store, 14, MakeFilledRenderedImage(full, cv::Scalar(0, 0, 1, 1)),
                            MakeFilledRenderedImage(full, cv::Scalar(1, 1, 0, 1)));
  EXPECT_TRUE(store.Get(11, ComparisonSide::A).isNull());
  EXPECT_TRUE(store.Get(11, ComparisonSide::B).isNull());
  EXPECT_EQ(qBlue(store.Get(14, ComparisonSide::A).pixel(0, 0)), 255);
  EXPECT_EQ(qRed(store.Get(14, ComparisonSide::B).pixel(0, 0)), 255);
  EXPECT_EQ(store.ImageCount(), 2);

  const QVariantMap published = second.ToVariantMap();
  EXPECT_EQ(published.value(QStringLiteral("operationId")).toULongLong(), 14u);
  EXPECT_EQ(published.value(QStringLiteral("aSource")).toString(), second.a_url);
  EXPECT_EQ(published.value(QStringLiteral("bSource")).toString(), second.b_url);
  EXPECT_EQ(
      published.value(QStringLiteral("aPlacement")).toMap().value("referenceWidth").toDouble(),
      40.0);

  // Concurrent reads during replacements only ever see images of the operation they asked for.
  std::atomic<bool>          stop{false};
  std::atomic<std::uint64_t> latest{14};
  std::atomic<int>           mismatches{0};
  std::atomic<int>           complete_reads{0};
  std::thread                reader([&] {
    while (!stop.load()) {
      const auto op = latest.load();
      const auto a  = store.Get(op, ComparisonSide::A);
      const auto b  = store.Get(op, ComparisonSide::B);
      for (const auto& [image, green] : {std::pair{a, 10}, std::pair{b, 20}}) {
        if (!image.isNull() && (qRed(image.pixel(0, 0)) != static_cast<int>(op % 200) ||
                                qGreen(image.pixel(0, 0)) != green)) {
          ++mismatches;
        }
      }
      if (!a.isNull() && !b.isNull()) {
        ++complete_reads;
      }
    }
  });
  for (std::uint64_t op = 15; op < 615; ++op) {
    auto [a, b] = TaggedPair(static_cast<int>(op % 200));
    (void)store.PublishPair(op, a, b);
    latest.store(op);
  }
  stop.store(true);
  reader.join();
  EXPECT_EQ(mismatches.load(), 0);
  EXPECT_GT(complete_reads.load(), 0);
}

TEST(EditorComparisonImageProviderTest, ProviderServesOnlyTheStoredOperationAtItsRenderExtent) {
  auto       store    = std::make_shared<ComparisonImageStore>();
  const auto geometry = ResolveComparisonGeometry(kSource, NormalizedRect{0.25f, 0, 0.5f, 1});
  (void)PublishComparisonPair(*store, 21, MakeFilledRenderedImage(geometry, cv::Scalar::all(0.25)),
                              MakeFilledRenderedImage(geometry, cv::Scalar::all(0.75)));
  ComparisonImageProvider provider(store);

  QSize                   size;
  const QImage            a = provider.requestImage(QStringLiteral("21/a"), &size, QSize(8, 8));
  ASSERT_FALSE(a.isNull());
  // The requested size is ignored: the image keeps its exact render extent.
  EXPECT_EQ(size, QSize(20, 30));
  EXPECT_EQ(a.size(), QSize(20, 30));
  EXPECT_EQ(qRed(provider.requestImage(QStringLiteral("21/b"), nullptr, {}).pixel(0, 0)), 191);

  for (const auto* id : {"20/a", "21/c", "21", "0/a", "x/a", "21/a/b"}) {
    QSize missing_size(1, 1);
    EXPECT_TRUE(provider.requestImage(QString::fromLatin1(id), &missing_size, {}).isNull()) << id;
    EXPECT_TRUE(missing_size.isEmpty()) << id;
  }

  EXPECT_THROW((void)store->PublishPair(0, a, a), std::invalid_argument);
  EXPECT_THROW((void)store->PublishPair(22, a, QImage{}), std::invalid_argument);
  EXPECT_FALSE(store->Get(21, ComparisonSide::A).isNull());
}

TEST(EditorComparisonImageProviderTest, ClearedStoreKeepsImagesAlreadyReadByTheProvider) {
  auto       store    = std::make_shared<ComparisonImageStore>();
  const auto geometry = ResolveComparisonGeometry(kSource);
  (void)PublishComparisonPair(*store, 31, MakeFilledRenderedImage(geometry, cv::Scalar(1, 0, 0, 1)),
                              MakeFilledRenderedImage(geometry, cv::Scalar(0, 0, 1, 1)));
  ComparisonImageProvider provider(store);

  QImage                  read_a = provider.requestImage(QStringLiteral("31/a"), nullptr, {});
  ASSERT_FALSE(read_a.isNull());
  // The read shares the stored pixels.
  EXPECT_FALSE(read_a.isDetached());

  store->Clear();
  EXPECT_EQ(store->ImageCount(), 0);
  EXPECT_TRUE(provider.requestImage(QStringLiteral("31/a"), nullptr, {}).isNull());

  // The store released its reference; the read image is now the only owner and is intact.
  EXPECT_TRUE(read_a.isDetached());
  EXPECT_EQ(qRed(read_a.pixel(5, 5)), 255);
  EXPECT_EQ(qBlue(read_a.pixel(5, 5)), 0);
}

}  // namespace alcedo::ui::test
