//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/mask_thumbnail_evaluate.hpp"
#include "app/mask_thumbnail_service.hpp"
#include "app/mask_thumbnail_spec.hpp"

#include <gtest/gtest.h>

#include <QImage>
#include <QObject>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <variant>
#include <vector>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/render_request.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "edit/geometry/types.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {
namespace {

constexpr float kAnalyticEpsilon = 1.0e-6f;

auto SquareGeometry() -> MaskThumbnailGeometry {
  MaskThumbnailGeometry geometry;
  geometry.full_reference = Extent2D{128, 128};
  return geometry;
}

auto LandscapeGeometry() -> MaskThumbnailGeometry {
  MaskThumbnailGeometry geometry;
  geometry.full_reference = Extent2D{200, 100};
  return geometry;
}

auto MakeRadial(MaskId id, float opacity = 1.0f, bool invert = false, bool enabled = true,
                RadialMaskSource source = {}) -> MaskModel {
  MaskModel mask;
  mask.id      = std::move(id);
  mask.opacity = opacity;
  mask.invert  = invert;
  mask.enabled = enabled;
  mask.source  = source;
  return mask;
}

auto MakeLinear(MaskId id, float opacity = 1.0f) -> MaskModel {
  MaskModel mask;
  mask.id      = std::move(id);
  mask.opacity = opacity;
  mask.source  = LinearGradientMaskSource{};
  return mask;
}

void RunInline(MaskThumbnailService* service) {
  service->SetJobRunner([](std::function<void()> job) { job(); });
  service->SetResultDispatcher([](QObject*, std::function<void()> fn) { fn(); });
}

auto IndependentRadialCoverage(const RadialMaskParams& radial, float nx, float ny) -> float {
  const float c     = std::cos(radial.rotation);
  const float s     = std::sin(radial.rotation);
  const float dx    = nx - radial.center_x;
  const float dy    = ny - radial.center_y;
  const float rx    = (c * dx + s * dy) / std::max(radial.major_radius, kAnalyticEpsilon);
  const float ry    = (-s * dx + c * dy) / std::max(radial.minor_radius, kAnalyticEpsilon);
  const float rho   = std::sqrt(rx * rx + ry * ry);
  const float inner = std::max(0.0f, 1.0f - radial.inner_feather);
  const float outer = 1.0f + radial.outer_feather;
  return 1.0f - std::clamp((rho - inner) / std::max(outer - inner, kAnalyticEpsilon), 0.0f, 1.0f);
}

auto QuantizeToR8(float coverage) -> std::uint8_t {
  return static_cast<std::uint8_t>(std::clamp(coverage * 255.0f + 0.5f, 0.0f, 255.0f));
}

auto IndependentPixel(const MaskThumbnailSpec& spec, int x, int y) -> std::uint8_t {
  ImageGeometryParams image;
  image.crop_rect        = spec.geometry.crop_rect;
  image.rotation_degrees = spec.geometry.rotation_degrees;
  image.expand_to_fit    = spec.geometry.expand_to_fit;
  ResolutionRequest resolution;
  resolution.render_scale = 1.0f;
  resolution.max_edge     = kMaskThumbnailSize;
  resolution.quality      = RenderQuality::Preview;
  const auto geometry     = ResolveRenderGeometry(
      MakeSourceGeometry(spec.geometry.full_reference, spec.geometry.full_reference), image,
      ViewRequest{}, resolution, SamplingFootprint{});
  const auto content_w = static_cast<int>(geometry.render_extent.width);
  const auto content_h = static_cast<int>(geometry.render_extent.height);
  const int  origin_x  = (static_cast<int>(kMaskThumbnailSize) - content_w) / 2;
  const int  origin_y  = (static_cast<int>(kMaskThumbnailSize) - content_h) / 2;
  if (x < origin_x || y < origin_y || x >= origin_x + content_w || y >= origin_y + content_h) {
    return 0;
  }
  const Vector2 render_center{static_cast<float>(x - origin_x) + 0.5f,
                              static_cast<float>(y - origin_y) + 0.5f};
  const Vector2 reference = TransformPoint(geometry.render_to_reference, render_center);
  const float   full_w    = static_cast<float>(spec.geometry.full_reference.width);
  const float   full_h    = static_cast<float>(spec.geometry.full_reference.height);
  if (reference.x < 0.0f || reference.y < 0.0f || reference.x >= full_w || reference.y >= full_h) {
    return 0;
  }
  const float nx = reference.x / full_w;
  const float ny = reference.y / full_h;
  std::uint8_t mix = 0;
  for (const auto& layer : spec.layers) {
    if (!layer.enabled) {
      continue;
    }
    float coverage = 0.0f;
    if (layer.kind == MaskSourceKind::Radial) {
      coverage = IndependentRadialCoverage(std::get<RadialMaskParams>(layer.params), nx, ny);
    } else {
      const auto& linear        = std::get<LinearGradientMaskParams>(layer.params);
      const float normal_length = std::hypot(linear.normal_x, linear.normal_y);
      const float normal_x      = linear.normal_x / std::max(normal_length, kAnalyticEpsilon);
      const float normal_y      = linear.normal_y / std::max(normal_length, kAnalyticEpsilon);
      const float distance =
          (nx - linear.origin_x) * normal_x + (ny - linear.origin_y) * normal_y;
      const float t =
          std::clamp(distance / std::max(linear.transition_distance, kAnalyticEpsilon) + 0.5f, 0.0f,
                     1.0f);
      coverage = linear.start_value + (linear.end_value - linear.start_value) * t;
    }
    if (layer.invert) {
      coverage = 1.0f - coverage;
    }
    mix = std::max(mix, QuantizeToR8(std::clamp(coverage * layer.opacity, 0.0f, 1.0f)));
  }
  return mix;
}

}  // namespace

TEST(MaskThumbnailSpec, OmitsIdentityFieldsAndSortsGroupLayers) {
  const auto geometry = SquareGeometry();
  auto       left     = MakeRadial(MaskId{"mask.a"});
  std::get<RadialMaskSource>(left.source).center_x = 0.25f;
  auto right = MakeRadial(MaskId{"mask.b"});
  std::get<RadialMaskSource>(right.source).center_x = 0.75f;
  left.display_name                                 = "Left";
  right.display_name                                = "Right";
  left.deletion_protected                           = true;

  const auto ab = MakeGroupMaskThumbnailSpec(geometry, std::vector<MaskModel>{left, right});
  const auto ba = MakeGroupMaskThumbnailSpec(geometry, std::vector<MaskModel>{right, left});
  EXPECT_EQ(ab, ba);
  EXPECT_EQ(ab.key, ba.key);

  const auto named   = MakeSingleMaskThumbnailSpec(geometry, left);
  auto       renamed = left;
  renamed.id            = MaskId{"mask.other"};
  renamed.display_name  = "Other";
  renamed.deletion_protected = false;
  const auto other = MakeSingleMaskThumbnailSpec(geometry, renamed);
  EXPECT_EQ(named, other);
  EXPECT_EQ(named.key, other.key);
}

TEST(MaskThumbnailSpec, ParameterChangeProducesNewKey) {
  const auto geometry = SquareGeometry();
  auto       mask     = MakeRadial(MaskId{"mask.radial"});
  const auto first    = MakeSingleMaskThumbnailSpec(geometry, mask);
  std::get<RadialMaskSource>(mask.source).center_x = 0.4f;
  const auto moved = MakeSingleMaskThumbnailSpec(geometry, mask);
  EXPECT_NE(first, moved);
  EXPECT_NE(first.key, moved.key);
}

TEST(MaskThumbnailEvaluate, RadialCenterIsWhiteAndCornersStayBlack) {
  const auto spec  = MakeSingleMaskThumbnailSpec(SquareGeometry(), MakeRadial(MaskId{"mask.radial"}));
  const auto image = RenderMaskThumbnail(spec);
  ASSERT_EQ(image.format(), QImage::Format_Grayscale8);
  ASSERT_EQ(image.width(), 128);
  ASSERT_EQ(image.height(), 128);
  EXPECT_EQ(qGray(image.pixel(64, 64)), 255);
  EXPECT_EQ(qGray(image.pixel(0, 0)), 0);
  EXPECT_EQ(qGray(image.pixel(127, 127)), 0);
  EXPECT_EQ(qGray(image.pixel(64, 64)), IndependentPixel(spec, 64, 64));
  EXPECT_EQ(qGray(image.pixel(0, 0)), IndependentPixel(spec, 0, 0));
}

TEST(MaskThumbnailEvaluate, InvertKeepsLetterboxBlack) {
  auto       mask = MakeRadial(MaskId{"mask.radial"}, 1.0f, true);
  const auto spec = MakeSingleMaskThumbnailSpec(LandscapeGeometry(), mask);
  const auto image = RenderMaskThumbnail(spec);
  EXPECT_EQ(qGray(image.pixel(64, 0)), 0);
  EXPECT_EQ(qGray(image.pixel(64, 0)), IndependentPixel(spec, 64, 0));
  EXPECT_EQ(qGray(image.pixel(64, 64)), 0);
  EXPECT_EQ(qGray(image.pixel(0, 32)), IndependentPixel(spec, 0, 32));
  EXPECT_GT(qGray(image.pixel(0, 32)), 0);
}

TEST(MaskThumbnailEvaluate, GroupMaxKeepsHalfCoverageAtIntersection) {
  auto a = MakeRadial(MaskId{"mask.a"}, 0.5f);
  auto b = MakeRadial(MaskId{"mask.b"}, 0.5f);
  const auto spec =
      MakeGroupMaskThumbnailSpec(SquareGeometry(), std::vector<MaskModel>{a, b});
  const auto image = RenderMaskThumbnail(spec);
  EXPECT_EQ(qGray(image.pixel(64, 64)), 128);
  EXPECT_EQ(qGray(image.pixel(64, 64)), IndependentPixel(spec, 64, 64));
}

TEST(MaskThumbnailEvaluate, DisabledSingleRendersBlackAndAllDisabledGroupRendersBlack) {
  auto disabled = MakeRadial(MaskId{"mask.off"}, 1.0f, false, false);
  const auto single = MakeSingleMaskThumbnailSpec(SquareGeometry(), disabled);
  const auto group =
      MakeGroupMaskThumbnailSpec(SquareGeometry(), std::vector<MaskModel>{disabled, disabled});
  EXPECT_EQ(group.kind, MaskThumbnailKind::Group);
  EXPECT_TRUE(group.layers.empty());
  const auto single_image = RenderMaskThumbnail(single);
  const auto group_image  = RenderMaskThumbnail(group);
  EXPECT_EQ(qGray(single_image.pixel(64, 64)), 0);
  EXPECT_EQ(qGray(group_image.pixel(64, 64)), 0);
}

TEST(MaskThumbnailService, CacheHitReusesPixelsForSameSpecDifferentCallers) {
  MaskThumbnailService service(8);
  std::uint32_t        enqueued = 0;
  service.SetJobRunner([&](std::function<void()> job) {
    ++enqueued;
    job();
  });
  service.SetResultDispatcher([](QObject*, std::function<void()> fn) { fn(); });
  QObject    receiver;
  const auto spec = MakeSingleMaskThumbnailSpec(SquareGeometry(), MakeRadial(MaskId{"mask.a"}));
  const auto same = MakeSingleMaskThumbnailSpec(SquareGeometry(), MakeRadial(MaskId{"mask.b"}));
  EXPECT_EQ(spec, same);

  int callbacks = 0;
  service.Request(spec, &receiver, 1, [&](std::uint64_t, MaskThumbnailSpec, QImage image, QString error) {
    EXPECT_TRUE(error.isEmpty());
    EXPECT_FALSE(image.isNull());
    ++callbacks;
  });
  service.Request(same, &receiver, 2, [&](std::uint64_t, MaskThumbnailSpec, QImage image, QString error) {
    EXPECT_TRUE(error.isEmpty());
    EXPECT_FALSE(image.isNull());
    ++callbacks;
  });
  EXPECT_EQ(callbacks, 2);
  EXPECT_EQ(enqueued, 1u);
  EXPECT_EQ(service.generate_count(), 1u);
  EXPECT_EQ(service.size(), 1u);
}

TEST(MaskThumbnailService, HeldJobFillsCacheAndLateCallbackStillRuns) {
  MaskThumbnailService service(8);
  std::vector<std::function<void()>> held;
  service.SetJobRunner([&](std::function<void()> job) { held.push_back(std::move(job)); });
  service.SetResultDispatcher([](QObject*, std::function<void()> fn) { fn(); });
  QObject receiver;
  const auto spec = MakeSingleMaskThumbnailSpec(SquareGeometry(), MakeRadial(MaskId{"mask.a"}));
  std::uint64_t seen = 0;
  service.Request(spec, &receiver, 41, [&](std::uint64_t id, MaskThumbnailSpec, QImage, QString) {
    seen = id;
  });
  ASSERT_EQ(held.size(), 1u);
  EXPECT_FALSE(service.Cached(spec).has_value());
  EXPECT_EQ(service.generate_count(), 0u);
  held.front()();
  EXPECT_TRUE(service.Cached(spec).has_value());
  EXPECT_EQ(service.generate_count(), 1u);
  EXPECT_EQ(seen, 41u);
}

TEST(MaskThumbnailService, LruEvictsLeastRecentlyUsedAndPeekDoesNotPromote) {
  MaskThumbnailService service(3);
  RunInline(&service);
  QObject receiver;
  auto spec_at = [&](float center_x) {
    auto mask = MakeRadial(MaskId{"mask.radial"});
    std::get<RadialMaskSource>(mask.source).center_x = center_x;
    return MakeSingleMaskThumbnailSpec(SquareGeometry(), mask);
  };
  const auto a = spec_at(0.20f);
  const auto b = spec_at(0.40f);
  const auto c = spec_at(0.60f);
  const auto d = spec_at(0.80f);
  auto ignore  = [&](std::uint64_t, MaskThumbnailSpec, QImage, QString) {};
  service.Request(a, &receiver, 1, ignore);
  service.Request(b, &receiver, 2, ignore);
  service.Request(c, &receiver, 3, ignore);
  ASSERT_EQ(service.size(), 3u);
  EXPECT_TRUE(service.Cached(a).has_value());
  service.Request(d, &receiver, 4, ignore);
  EXPECT_EQ(service.size(), 3u);
  EXPECT_FALSE(service.Cached(a).has_value());
  EXPECT_TRUE(service.Cached(b).has_value());
  EXPECT_TRUE(service.Cached(c).has_value());
  EXPECT_TRUE(service.Cached(d).has_value());
}

TEST(MaskThumbnailService, DefaultCapacityIsOneThousand) {
  MaskThumbnailService service;
  EXPECT_EQ(service.capacity(), kMaskThumbnailCacheCapacity);
}

TEST(MaskThumbnailService, HashCollisionStillComparesFullSpec) {
  MaskThumbnailService service(8);
  RunInline(&service);
  QObject receiver;
  auto full = MakeSingleMaskThumbnailSpec(SquareGeometry(), MakeRadial(MaskId{"mask.radial"}));
  auto half = full;
  half.layers.front().opacity = 0.5f;
  ASSERT_EQ(full.key, half.key);
  ASSERT_NE(full, half);

  QImage full_image;
  QImage half_image;
  service.Request(full, &receiver, 1, [&](std::uint64_t, MaskThumbnailSpec, QImage image, QString) {
    full_image = std::move(image);
  });
  service.Request(half, &receiver, 2, [&](std::uint64_t, MaskThumbnailSpec, QImage image, QString) {
    half_image = std::move(image);
  });
  ASSERT_FALSE(full_image.isNull());
  ASSERT_FALSE(half_image.isNull());
  EXPECT_EQ(qGray(full_image.pixel(64, 64)), 255);
  EXPECT_EQ(qGray(half_image.pixel(64, 64)), 128);
  EXPECT_EQ(qGray(service.Cached(full)->pixel(64, 64)), 255);
  EXPECT_EQ(qGray(service.Cached(half)->pixel(64, 64)), 128);
}

TEST(MaskThumbnailService, CropChangeMissesAndEvictedQImageHandleStaysValid) {
  MaskThumbnailService service(1);
  RunInline(&service);
  QObject receiver;
  auto uncropped = MakeSingleMaskThumbnailSpec(SquareGeometry(), MakeRadial(MaskId{"mask.radial"}));
  auto cropped_geometry = SquareGeometry();
  cropped_geometry.crop_rect = NormalizedRect{0.1f, 0.1f, 0.8f, 0.8f};
  const auto cropped =
      MakeSingleMaskThumbnailSpec(cropped_geometry, MakeRadial(MaskId{"mask.radial"}));
  ASSERT_NE(uncropped, cropped);

  QImage retained;
  service.Request(uncropped, &receiver, 1,
                  [&](std::uint64_t, MaskThumbnailSpec, QImage image, QString) {
                    retained = image;
                  });
  ASSERT_FALSE(retained.isNull());
  service.Request(cropped, &receiver, 2, [&](std::uint64_t, MaskThumbnailSpec, QImage, QString) {});
  EXPECT_EQ(service.size(), 1u);
  EXPECT_FALSE(service.Cached(uncropped).has_value());
  EXPECT_EQ(retained.width(), 128);
  EXPECT_EQ(qGray(retained.pixel(64, 64)), 255);
}

}  // namespace alcedo
