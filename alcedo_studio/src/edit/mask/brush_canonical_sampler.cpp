//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/brush_canonical_sampler.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace alcedo {
namespace {

[[noreturn]] void FailSampler(std::string_view message) {
  throw std::runtime_error(std::string{message});
}

void RequireFinitePoint(Vector2 point, std::string_view name) {
  if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
    FailSampler(std::string{name} + " must be finite");
  }
}

void RequireMode(BrushStrokeMode mode) {
  if (mode != BrushStrokeMode::Paint && mode != BrushStrokeMode::Erase) {
    FailSampler("brush stroke mode must be paint or erase");
  }
}

auto ValidatedSample(float local_x, float local_y, float radius, float strength, float hardness)
    -> BrushCanonicalSample {
  BrushCanonicalSample sample{local_x, local_y, radius, strength, hardness};
  try {
    ValidateBrushCanonicalSample(sample);
  } catch (const std::invalid_argument& ex) {
    FailSampler(ex.what());
  }
  return sample;
}

}  // namespace

void BrushCanonicalSampler::RequireOpen() const {
  if (!open_) {
    FailSampler("brush stroke is not open");
  }
}

void BrushCanonicalSampler::Emit(const BrushCanonicalSample& sample) {
  samples_.push_back(sample);
}

void BrushCanonicalSampler::WalkTo(Vector2 local) {
  const float dx     = local.x - last_local_.x;
  const float dy     = local.y - last_local_.y;
  const float length = std::hypot(dx, dy);
  if (!(length > 0.0f)) {
    last_local_ = local;
    return;
  }
  const float spacing = radius_ * kBrushDabSpacingRadiusFraction;
  float       next    = spacing - remainder_;
  while (next <= length) {
    const float t = next / length;
    Emit(ValidatedSample(last_local_.x + dx * t, last_local_.y + dy * t, radius_, strength_,
                         hardness_));
    next += spacing;
  }
  remainder_  = length - (next - spacing);
  last_local_ = local;
}

void BrushCanonicalSampler::BeginStroke(BrushStrokeMode mode, Vector2 reference,
                                        Vector2 translation, float radius, float strength,
                                        float hardness) {
  if (open_) {
    FailSampler("brush stroke is already open");
  }
  RequireMode(mode);
  RequireFinitePoint(reference, "reference");
  RequireFinitePoint(translation, "placement_translation");
  const Vector2 local{reference.x - translation.x, reference.y - translation.y};
  const auto    first = ValidatedSample(local.x, local.y, radius, strength, hardness);
  mode_               = mode;
  translation_        = translation;
  last_local_         = local;
  remainder_          = 0.0f;
  radius_             = radius;
  strength_           = strength;
  hardness_           = hardness;
  samples_.clear();
  open_ = true;
  Emit(first);
}

void BrushCanonicalSampler::SetSampleParameters(float radius, float strength, float hardness) {
  RequireOpen();
  const auto sample = ValidatedSample(last_local_.x, last_local_.y, radius, strength, hardness);
  if (radius_ == radius && strength_ == strength && hardness_ == hardness) {
    return;
  }
  radius_    = radius;
  strength_  = strength;
  hardness_  = hardness;
  remainder_ = 0.0f;
  Emit(sample);
}

void BrushCanonicalSampler::AppendReference(Vector2 reference) {
  RequireOpen();
  RequireFinitePoint(reference, "reference");
  WalkTo({reference.x - translation_.x, reference.y - translation_.y});
}

auto BrushCanonicalSampler::FinishStroke() -> std::vector<BrushCanonicalSample> {
  RequireOpen();
  if (samples_.empty()) {
    FailSampler("open brush stroke has no samples");
  }
  const auto& last = samples_.back();
  if (last.local_x != last_local_.x || last.local_y != last_local_.y) {
    Emit(ValidatedSample(last_local_.x, last_local_.y, radius_, strength_, hardness_));
  }
  open_ = false;
  remainder_ = 0.0f;
  return std::move(samples_);
}

void BrushCanonicalSampler::CancelStroke() {
  open_      = false;
  remainder_ = 0.0f;
  samples_.clear();
}

auto BrushCanonicalSampler::DraftSamples() const -> std::span<const BrushCanonicalSample> {
  if (!open_) {
    return {};
  }
  return samples_;
}

}  // namespace alcedo
