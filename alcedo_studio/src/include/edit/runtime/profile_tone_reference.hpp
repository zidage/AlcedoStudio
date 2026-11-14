// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/utils/color_utils.hpp"
#include "edit/runtime/drt/aces2_reference_runtime.hpp"
#include "image/dng_tone_curve.hpp"

// Host reference of the profile tone stage (dcp_profile_tone_curve_study.md, section 6.1, phase
// T1). The stage expresses the `ProfileToneCurve` of a DCP profile through the inverse of the
// neutral response of the active DRT, so that the DRT renders a neutral as the profile curve does.
// The GPU backends of phase T2 compute the stage; the host builds the tables and keeps this scalar
// reference for the tests.

namespace alcedo {

/// Exposure anchor `E_a` between Alcedo and ACR: Alcedo +1.5 EV equals ACR 0 EV (study 3.3).
inline constexpr float       kProfileToneExposureAnchorEv     = kDefaultPipelineExposureEv;
/// Peak luminance of the SDR reference display of the neutral responses.
inline constexpr float       kProfileToneReferencePeakNits    = 100.0f;
/// Entries of the inverse table: display light sampled on `d^(1 / 2.4)` at `i / 4096`.
inline constexpr std::size_t kProfileToneInverseTableEntries  = 4097;
/// Exponent of the inverse table coordinate: entry i holds `N^-1((i / 4096)^2.4)`.
inline constexpr float       kProfileToneInverseTableExponent = 2.4f;
/// Knee `k` of the highlight shoulder at the curve input, ACR scale: one stop below raw white.
inline constexpr float       kProfileToneHighlightKnee        = 0.5f;

/**
 * @brief The highlight shoulder `H` at the curve input (study section 6.6), ACR scale.
 *
 * 0 below 0; identity up to @ref kProfileToneHighlightKnee `k`; above it
 * `1 - (1 - k)^2 / (x + 1 - 2k)`, with slope 1 at the knee and a limit of 1. Raw white maps to
 * 0.75, and each further stop halves the distance to the curve's white.
 */
[[nodiscard]] auto           ProfileToneHighlightShoulder(float x) -> float;

/**
 * @brief Neutral response `N_m` of one DRT method and its inverse table `N_m^-1`.
 *
 * `N_m` maps scene light of an AP1 neutral to display light relative to the peak, with the product
 * default parameters of the method at the SDR reference display (100 nits, Rec.709):
 * - ACES 2.0: the OpenColorIO ACES 2.0 output transform of aces2_reference_math.h on the AP0
 *   neutral, Rec.709 luminance of the display RGB.
 * - OpenDRT: the achromatic path of `OpenDRTTransform_fwd` (open_drt_funcs.cuh) with the
 *   `DrtPayload` defaults: the tone scale on the norm, without the chroma terms, which are zero on
 *   a neutral.
 *
 * `Forward` is not clamped. The inverse table holds the scene value at which `N_m` reaches the
 * peak for display light 1 (study section 6.1). The value is immutable; @ref Resolve builds it once
 * per method and process.
 */
class ProfileToneNeutralResponse {
 public:
  /**
   * @brief The response of @p method, built on the first call for each method. Thread-safe.
   * @throws std::invalid_argument for DrtMethod::FilmPrint, which has no profile tone stage.
   * @throws std::runtime_error when the default parameters do not resolve, or a default
   *         parameter falls outside the achromatic port (OpenDRT local or high contrast enabled,
   *         creative white other than D65).
   */
  [[nodiscard]] static auto Resolve(DrtMethod method)
      -> std::shared_ptr<const ProfileToneNeutralResponse>;

  [[nodiscard]] auto Method() const -> DrtMethod { return method_; }
  /// `N_m` at scene value @p scene of an AP1 neutral.
  [[nodiscard]] auto Forward(float scene) const -> float;
  /// `N_m^-1` from the table at display light @p display, clamped to [0, 1].
  [[nodiscard]] auto Inverse(float display) const -> float;
  /// The @ref kProfileToneInverseTableEntries scene values of the inverse table.
  [[nodiscard]] auto InverseTable() const -> std::span<const float> { return inverse_table_; }
  /// Scene value at which `N_m` reaches display light 1.
  [[nodiscard]] auto SceneAtPeak() const -> float { return scene_at_peak_; }
  /// Scene value at which `N_m` reaches display light 0: 0 for ACES 2.0, minus the OpenDRT offset.
  [[nodiscard]] auto SceneAtBlack() const -> float { return scene_at_black_; }

 private:
  ProfileToneNeutralResponse() = default;
  void                                         BuildInverseTable();

  DrtMethod                                    method_ = DrtMethod::Aces20;
  std::shared_ptr<const Aces2ReferenceRuntime> aces_;
  ColorUtils::OpenDRTParams                    open_drt_{};
  std::vector<float>                           inverse_table_;
  float                                        scene_at_peak_  = 0.0f;
  float                                        scene_at_black_ = 0.0f;
};

/**
 * @brief The profile tone stage for one AP1 pixel at the DRT entry (study section 6.1).
 *
 * `p = H(M * x * 2^-E_a)` per channel in linear ProPhoto (D50), `d = R_C(p)` with the DNG SDK
 * baseline RGB tone, `s = N_m^-1(d)` per channel, and `M^-1 * s` back to AP1. `M` is the
 * AP1-to-ProPhoto matrix of the camera pass (ProPhotoToAp1 with XyzD50ToAp1Matrix).
 */
[[nodiscard]] auto ApplyProfileToneStageReference(const DngToneCurve&               curve,
                                                  const ProfileToneNeutralResponse& response,
                                                  const std::array<float, 3>&       ap1)
    -> std::array<float, 3>;

/// Linear AP1 to linear ProPhoto (D50) and back, the matrix pair `M`, `M^-1` of the stage.
/// Row-major, column vectors, float as the GPU applies them.
struct ProfileToneMatrices {
  std::array<float, 9> ap1_to_prophoto{};
  std::array<float, 9> prophoto_to_ap1{};
};
[[nodiscard]] auto ProfileToneStageMatrices() -> const ProfileToneMatrices&;

}  // namespace alcedo
