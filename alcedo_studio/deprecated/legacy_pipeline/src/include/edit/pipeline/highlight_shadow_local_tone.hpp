//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/runtime/local_tone_mapping.hpp"

namespace alcedo::highlight_shadow_local_tone {

using local_tone_mapping::ApplyReferenceCurve;
using local_tone_mapping::BuildAdjustedResultCacheKey;
using local_tone_mapping::BuildRoiAdjustedResultCacheKey;
using local_tone_mapping::BuildSamples;
using local_tone_mapping::CanReuseReferenceForRoi;
using local_tone_mapping::ComputeLevelCount;
using local_tone_mapping::ComputeMaskDimensions;
using local_tone_mapping::DetailAlpha;
using local_tone_mapping::FloatBits;
using local_tone_mapping::HashCombine;
using local_tone_mapping::HighlightProfileEv;
using local_tone_mapping::Lerp;
using local_tone_mapping::LlfSample;
using local_tone_mapping::MaskDimensions;
using local_tone_mapping::RelativeEv;
using local_tone_mapping::Segment;
using local_tone_mapping::ShadowProfileEv;
using local_tone_mapping::ShouldRun;
using local_tone_mapping::SigmaR;
using local_tone_mapping::Smoothstep;
using local_tone_mapping::ToneBeta;

using local_tone_mapping::kAcesccCodePerEv;
using local_tone_mapping::kAcesccMiddleGray;
using local_tone_mapping::kBackendAmountLimit;
using local_tone_mapping::kBaseSigmaR;
using local_tone_mapping::kGammaMaxL;
using local_tone_mapping::kGammaMinL;
using local_tone_mapping::kGammaStepScale;
using local_tone_mapping::kHighlightStrengthScale;
using local_tone_mapping::kMaxLevels;
using local_tone_mapping::kMaxSamples;
using local_tone_mapping::kMinSampleStep;
using local_tone_mapping::kReferenceMaskMaxLongEdge;
using local_tone_mapping::kToneBetaEps;
using local_tone_mapping::kToneBetaMax;
using local_tone_mapping::kToneBetaMin;

}  // namespace alcedo::highlight_shadow_local_tone
