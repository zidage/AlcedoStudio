//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/brush_mask_input.hpp"

#include <utility>
#include <vector>

namespace alcedo {

void BrushMaskInput::SetStrokeParameters(float radius, float strength, float hardness) {
  if (sampler_.IsOpen()) {
    sampler_.SetSampleParameters(radius, strength, hardness);
  } else {
    BrushCanonicalSample probe{0.0f, 0.0f, radius, strength, hardness};
    ValidateBrushCanonicalSample(probe);
  }
  radius_   = radius;
  strength_ = strength;
  hardness_ = hardness;
}

void BrushMaskInput::SetStrokeMode(BrushStrokeMode mode) {
  if (sampler_.IsOpen()) {
    return;
  }
  mode_ = mode;
}

void BrushMaskInput::Begin(StrokeId id, Vector2 reference, Vector2 translation) {
  sampler_.BeginStroke(mode_, reference, translation, radius_, strength_, hardness_);
  stroke_id_ = std::move(id);
}

void BrushMaskInput::Append(Vector2 reference) { sampler_.AppendReference(reference); }

auto BrushMaskInput::Finish() -> BrushStroke {
  const auto id      = stroke_id_;
  const auto mode    = sampler_.Mode();
  auto       samples = sampler_.FinishStroke();
  stroke_id_         = StrokeId{};
  return MakeBrushStroke(id, mode, std::move(samples));
}

void BrushMaskInput::Cancel() {
  sampler_.CancelStroke();
  stroke_id_ = StrokeId{};
}

auto BrushMaskInput::DraftStroke() const -> std::optional<BrushStroke> {
  const auto samples = sampler_.DraftSamples();
  if (samples.empty() || stroke_id_.Empty()) {
    return std::nullopt;
  }
  return MakeBrushStroke(stroke_id_, sampler_.Mode(),
                         std::vector<BrushCanonicalSample>(samples.begin(), samples.end()));
}

}  // namespace alcedo
