//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <span>

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
   * @throws std::runtime_error on LibRaw failure or unsupported CFA.
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
