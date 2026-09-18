//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/mask_thumbnail_spec.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace alcedo {
namespace {

[[nodiscard]] auto CanonicalFloat(float value, std::string_view name) -> float {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string{name} + " must be finite");
  }
  return value == 0.0f ? 0.0f : value;
}

void MixHash(std::uint64_t& hash, std::uint64_t value) {
  hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
}

void MixU32(std::uint64_t& hash, std::uint32_t value) { MixHash(hash, value); }

void MixBool(std::uint64_t& hash, bool value) { MixHash(hash, value ? 1 : 0); }

void MixFloat(std::uint64_t& hash, float value) {
  std::uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  MixU32(hash, bits);
}

[[nodiscard]] auto RadialParamsEqual(const RadialMaskParams& a, const RadialMaskParams& b) -> bool {
  return a.center_x == b.center_x && a.center_y == b.center_y && a.major_radius == b.major_radius &&
         a.minor_radius == b.minor_radius && a.rotation == b.rotation &&
         a.inner_feather == b.inner_feather && a.outer_feather == b.outer_feather &&
         a.invert == b.invert && a.opacity == b.opacity;
}

[[nodiscard]] auto LinearParamsEqual(const LinearGradientMaskParams& a,
                                     const LinearGradientMaskParams& b) -> bool {
  return a.origin_x == b.origin_x && a.origin_y == b.origin_y && a.normal_x == b.normal_x &&
         a.normal_y == b.normal_y && a.transition_distance == b.transition_distance &&
         a.start_value == b.start_value && a.end_value == b.end_value && a.invert == b.invert &&
         a.opacity == b.opacity;
}

void CanonicalizeRadial(RadialMaskParams& params) {
  params.center_x      = CanonicalFloat(params.center_x, "center_x");
  params.center_y      = CanonicalFloat(params.center_y, "center_y");
  params.major_radius  = CanonicalFloat(params.major_radius, "major_radius");
  params.minor_radius  = CanonicalFloat(params.minor_radius, "minor_radius");
  params.rotation      = CanonicalFloat(params.rotation, "rotation");
  params.inner_feather = CanonicalFloat(params.inner_feather, "inner_feather");
  params.outer_feather = CanonicalFloat(params.outer_feather, "outer_feather");
  params.opacity       = CanonicalFloat(params.opacity, "opacity");
}

void CanonicalizeLinear(LinearGradientMaskParams& params) {
  params.origin_x            = CanonicalFloat(params.origin_x, "origin_x");
  params.origin_y            = CanonicalFloat(params.origin_y, "origin_y");
  params.normal_x            = CanonicalFloat(params.normal_x, "normal_x");
  params.normal_y            = CanonicalFloat(params.normal_y, "normal_y");
  params.transition_distance = CanonicalFloat(params.transition_distance, "transition_distance");
  params.start_value         = CanonicalFloat(params.start_value, "start_value");
  params.end_value           = CanonicalFloat(params.end_value, "end_value");
  params.opacity             = CanonicalFloat(params.opacity, "opacity");
}

void MixRadial(std::uint64_t& hash, const RadialMaskParams& params) {
  MixFloat(hash, params.center_x);
  MixFloat(hash, params.center_y);
  MixFloat(hash, params.major_radius);
  MixFloat(hash, params.minor_radius);
  MixFloat(hash, params.rotation);
  MixFloat(hash, params.inner_feather);
  MixFloat(hash, params.outer_feather);
  MixBool(hash, params.invert);
  MixFloat(hash, params.opacity);
}

void MixLinear(std::uint64_t& hash, const LinearGradientMaskParams& params) {
  MixFloat(hash, params.origin_x);
  MixFloat(hash, params.origin_y);
  MixFloat(hash, params.normal_x);
  MixFloat(hash, params.normal_y);
  MixFloat(hash, params.transition_distance);
  MixFloat(hash, params.start_value);
  MixFloat(hash, params.end_value);
  MixBool(hash, params.invert);
  MixFloat(hash, params.opacity);
}

[[nodiscard]] auto LayerLess(const MaskThumbnailLayer& a, const MaskThumbnailLayer& b) -> bool {
  if (a.kind != b.kind) {
    return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
  }
  if (a.enabled != b.enabled) {
    return a.enabled < b.enabled;
  }
  if (a.invert != b.invert) {
    return a.invert < b.invert;
  }
  if (a.opacity != b.opacity) {
    return a.opacity < b.opacity;
  }
  if (a.kind == MaskSourceKind::Radial) {
    const auto& lhs = std::get<RadialMaskParams>(a.params);
    const auto& rhs = std::get<RadialMaskParams>(b.params);
    if (lhs.center_x != rhs.center_x) {
      return lhs.center_x < rhs.center_x;
    }
    if (lhs.center_y != rhs.center_y) {
      return lhs.center_y < rhs.center_y;
    }
    if (lhs.major_radius != rhs.major_radius) {
      return lhs.major_radius < rhs.major_radius;
    }
    if (lhs.minor_radius != rhs.minor_radius) {
      return lhs.minor_radius < rhs.minor_radius;
    }
    if (lhs.rotation != rhs.rotation) {
      return lhs.rotation < rhs.rotation;
    }
    if (lhs.inner_feather != rhs.inner_feather) {
      return lhs.inner_feather < rhs.inner_feather;
    }
    return lhs.outer_feather < rhs.outer_feather;
  }
  const auto& lhs = std::get<LinearGradientMaskParams>(a.params);
  const auto& rhs = std::get<LinearGradientMaskParams>(b.params);
  if (lhs.origin_x != rhs.origin_x) {
    return lhs.origin_x < rhs.origin_x;
  }
  if (lhs.origin_y != rhs.origin_y) {
    return lhs.origin_y < rhs.origin_y;
  }
  if (lhs.normal_x != rhs.normal_x) {
    return lhs.normal_x < rhs.normal_x;
  }
  if (lhs.normal_y != rhs.normal_y) {
    return lhs.normal_y < rhs.normal_y;
  }
  if (lhs.transition_distance != rhs.transition_distance) {
    return lhs.transition_distance < rhs.transition_distance;
  }
  if (lhs.start_value != rhs.start_value) {
    return lhs.start_value < rhs.start_value;
  }
  return lhs.end_value < rhs.end_value;
}

[[nodiscard]] auto LayerFromMask(const MaskModel& mask) -> MaskThumbnailLayer {
  MaskThumbnailLayer layer;
  layer.kind     = GetMaskSourceKind(mask.source);
  layer.enabled  = mask.enabled;
  layer.invert   = mask.invert;
  layer.opacity  = mask.opacity;
  if (layer.kind == MaskSourceKind::Radial) {
    layer.params = RadialParamsFromMask(mask);
  } else if (layer.kind == MaskSourceKind::LinearGradient) {
    layer.params = LinearGradientParamsFromMask(mask);
  } else {
    throw std::invalid_argument("Mask thumbnail specs support Radial and Linear Gradient only");
  }
  return layer;
}

}  // namespace

auto MaskThumbnailGeometry::operator==(const MaskThumbnailGeometry& other) const -> bool {
  return full_reference == other.full_reference && crop_rect.x == other.crop_rect.x &&
         crop_rect.y == other.crop_rect.y && crop_rect.w == other.crop_rect.w &&
         crop_rect.h == other.crop_rect.h && rotation_degrees == other.rotation_degrees &&
         expand_to_fit == other.expand_to_fit;
}

auto MaskThumbnailLayer::operator==(const MaskThumbnailLayer& other) const -> bool {
  if (kind != other.kind || enabled != other.enabled || invert != other.invert ||
      opacity != other.opacity) {
    return false;
  }
  if (kind == MaskSourceKind::Radial) {
    return RadialParamsEqual(std::get<RadialMaskParams>(params),
                             std::get<RadialMaskParams>(other.params));
  }
  return LinearParamsEqual(std::get<LinearGradientMaskParams>(params),
                           std::get<LinearGradientMaskParams>(other.params));
}

auto MaskThumbnailSpec::operator==(const MaskThumbnailSpec& other) const -> bool {
  return kind == other.kind && sampling_version == other.sampling_version &&
         geometry == other.geometry && layers == other.layers;
}

auto CanonicalizeMaskThumbnailSpec(MaskThumbnailSpec spec) -> MaskThumbnailSpec {
  if (spec.sampling_version != kMaskThumbnailSamplingVersion) {
    throw std::invalid_argument("unsupported Mask thumbnail sampling version");
  }
  if (spec.geometry.full_reference.Empty()) {
    throw std::invalid_argument("Mask thumbnail geometry requires a nonempty full reference");
  }
  spec.geometry.crop_rect.x = CanonicalFloat(spec.geometry.crop_rect.x, "crop_rect.x");
  spec.geometry.crop_rect.y = CanonicalFloat(spec.geometry.crop_rect.y, "crop_rect.y");
  spec.geometry.crop_rect.w = CanonicalFloat(spec.geometry.crop_rect.w, "crop_rect.w");
  spec.geometry.crop_rect.h = CanonicalFloat(spec.geometry.crop_rect.h, "crop_rect.h");
  spec.geometry.rotation_degrees =
      CanonicalFloat(spec.geometry.rotation_degrees, "rotation_degrees");
  for (auto& layer : spec.layers) {
    layer.opacity = CanonicalFloat(layer.opacity, "opacity");
    if (layer.kind == MaskSourceKind::Radial) {
      auto params = std::get<RadialMaskParams>(layer.params);
      CanonicalizeRadial(params);
      params.invert  = layer.invert;
      params.opacity = layer.opacity;
      layer.params   = params;
    } else if (layer.kind == MaskSourceKind::LinearGradient) {
      auto params = std::get<LinearGradientMaskParams>(layer.params);
      CanonicalizeLinear(params);
      params.invert  = layer.invert;
      params.opacity = layer.opacity;
      layer.params   = params;
    } else {
      throw std::invalid_argument("Mask thumbnail specs support Radial and Linear Gradient only");
    }
  }
  if (spec.kind == MaskThumbnailKind::Group) {
    std::sort(spec.layers.begin(), spec.layers.end(), LayerLess);
  }
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  MixU32(hash, spec.sampling_version);
  MixU32(hash, static_cast<std::uint32_t>(spec.kind));
  MixU32(hash, spec.geometry.full_reference.width);
  MixU32(hash, spec.geometry.full_reference.height);
  MixFloat(hash, spec.geometry.crop_rect.x);
  MixFloat(hash, spec.geometry.crop_rect.y);
  MixFloat(hash, spec.geometry.crop_rect.w);
  MixFloat(hash, spec.geometry.crop_rect.h);
  MixFloat(hash, spec.geometry.rotation_degrees);
  MixBool(hash, spec.geometry.expand_to_fit);
  MixHash(hash, spec.layers.size());
  for (const auto& layer : spec.layers) {
    MixU32(hash, static_cast<std::uint32_t>(layer.kind));
    MixBool(hash, layer.enabled);
    MixBool(hash, layer.invert);
    MixFloat(hash, layer.opacity);
    if (layer.kind == MaskSourceKind::Radial) {
      MixRadial(hash, std::get<RadialMaskParams>(layer.params));
    } else {
      MixLinear(hash, std::get<LinearGradientMaskParams>(layer.params));
    }
  }
  spec.key = hash;
  return spec;
}

auto MakeSingleMaskThumbnailSpec(const MaskThumbnailGeometry& geometry, const MaskModel& mask)
    -> MaskThumbnailSpec {
  MaskThumbnailSpec spec;
  spec.kind     = MaskThumbnailKind::Single;
  spec.geometry = geometry;
  spec.layers.push_back(LayerFromMask(mask));
  return CanonicalizeMaskThumbnailSpec(std::move(spec));
}

auto MakeGroupMaskThumbnailSpec(const MaskThumbnailGeometry& geometry,
                                std::span<const MaskModel>   masks) -> MaskThumbnailSpec {
  MaskThumbnailSpec spec;
  spec.kind     = MaskThumbnailKind::Group;
  spec.geometry = geometry;
  spec.layers.reserve(masks.size());
  for (const auto& mask : masks) {
    if (!mask.enabled) {
      continue;
    }
    spec.layers.push_back(LayerFromMask(mask));
  }
  return CanonicalizeMaskThumbnailSpec(std::move(spec));
}

}  // namespace alcedo
