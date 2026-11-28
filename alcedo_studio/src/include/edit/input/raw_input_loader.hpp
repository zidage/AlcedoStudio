//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "edit/input/prepared_raw_input.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief CPU LibRaw unpack, active-area metadata, and DecodeRes downsample.
 *
 * These steps are not GPU passes and do not belong in ExecutionPlan.
 */
class RawInputLoader {
 public:
  /**
   * @brief Open encoded bytes, unpack CFA or already-demosaiced RGB, downsample, then recycle
   *        LibRaw.
   *
   * Copies the camera and the shooting values into @ref PreparedRawInput::color_context so
   * Develop lens calibration can resolve a Lensfun profile. The lens name comes from
   * @ref LensIdentityResolver, the same reader that import uses. White-balance multipliers stay on
   * the unpack color fields, not the lens EXIF fields.
   * @throws std::runtime_error on LibRaw failure or unsupported CFA.
   * @throws Exiv2::Error when LibRaw has no lens name and Exiv2 recognizes the file format but
   *         cannot read its metadata.
   */
  [[nodiscard]] static auto LoadEncoded(std::span<const std::byte> encoded, DecodeRes decode_res)
      -> PreparedRawInput;

  /**
   * @brief Build a prepared CFA input without LibRaw. Used by tests and synthetic paths.
   */
  [[nodiscard]] static auto FromUnpackedCfa(HostImagePlane plane, RawCfaPattern pattern,
                                            RawLinearizationParams linearization,
                                            RawSensorGeometry sensor, DecodeRes decode_res)
      -> PreparedRawInput;

  /**
   * @brief Direct RGB (already demosaiced) host plane. No LibRaw.
   */
  [[nodiscard]] static auto FromDirectRgb(HostImagePlane plane, RawSensorGeometry sensor)
      -> PreparedRawInput;

  /**
   * @brief AI Denoise input of a full-size Bayer RAW file (AI Denoise integration plan, DN5).
   *
   * @p denoised is the camera RGB output of the model for the decode area of @p cfa (the active
   * area or the DNG default crop, @p cfa at full size), F32 RGBA and tightly packed. Channel c is
   * in the value domain of the CFA Linearize step before its clamp and its white balance: 0 is
   * `black_level[c]`, 1 is `white_level[c]` of @p cfa's linearization; values outside [0, 1] are
   * kept. The result copies the linearization, color context, DNG warp, orientation, and encoded
   * identity of @p cfa; its sensor geometry is the plane, and its source key holds the hash of
   * @p model_version and @p xyz_to_camera, the camera matrix of the gain match (HashAiDenoiseRun).
   * @throws std::runtime_error when @p cfa is not a full-size Bayer CFA input, when @p denoised is
   *         not tightly packed F32 RGBA of the decode area, or when @p model_version is not 64
   *         lowercase hexadecimal digits.
   */
  [[nodiscard]] static auto FromDenoisedBayer(const PreparedRawInput& cfa, HostImagePlane denoised,
                                              std::string_view             model_version,
                                              const std::array<double, 9>& xyz_to_camera)
      -> PreparedRawInput;

  [[nodiscard]] static auto TryLoadEncoded(std::span<const std::byte> encoded, DecodeRes decode_res)
      -> std::optional<PreparedRawInput>;
};

[[nodiscard]] inline auto DecodeResToDownsamplePasses(DecodeRes decode_res) -> std::uint8_t {
  switch (decode_res) {
    case DecodeRes::FULL:
      return 0;
    case DecodeRes::HALF:
      return 1;
    case DecodeRes::QUARTER:
      return 2;
    case DecodeRes::EIGHTH:
      return 3;
  }
  return 0;
}

}  // namespace alcedo
