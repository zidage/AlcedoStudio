//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Host bake of the LMT composite table (docs/roadmap/alcedo_studio/edit/lut_color_encoding_plan.md,
// sections 4 and 6.2): the conversion from the ACEScc AP1 working space to the LUT's input
// encoding, the user LUT with its DOMAIN_MIN/MAX, and the conversion from the LUT's output
// encoding back to ACEScc AP1, composed into one ACEScc to ACEScc 3D table. The primary grade
// pass samples the composite table exactly like a raw LUT, so no GPU kernel changes.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "color/color_encoding_catalog.hpp"
#include "edit/runtime/drt/aces2_reference_runtime.hpp"
#include "edit/runtime/grade_lut.hpp"

namespace alcedo {

/// Edge of every composite table, whatever the source LUT size (plan decision E3).
inline constexpr std::uint32_t kLmtCompositeEdge = 65;

/// One RGB triple: ACEScc AP1 values or LUT code values.
using LutRgb                                     = std::array<float, 3>;

/// True when @p table's domain is [0, 1] on every channel.
[[nodiscard]] auto HasUnitDomain(const PackedGradeLut& table) -> bool;

/**
 * @brief Sample @p table at code value @p code like the primary grade pass does.
 *
 * Each channel is mapped through the domain, u = (code - domain_min) / (domain_max -
 * domain_min), clamped to [0, 1], and the table is interpolated trilinearly at u * (edge - 1).
 * The arithmetic order matches the CUDA, OpenCL and Metal SampleLut3d.
 */
[[nodiscard]] auto SampleLutTable(const PackedGradeLut& table, const LutRgb& code) -> LutRgb;

/**
 * @brief Conversions between the ACEScc AP1 working space and the encodings of one LUT.
 *
 * Scene-referred encodings convert with a gamut matrix and the encoding's transfer function.
 * A display-referred input is reached with the OCIO ACES 2.0 reference forward and a
 * display-referred output is brought back with the R2 inverse (D2aSourceToAcesccAp1), both
 * built for the encoding's own primaries and peak luminance (plan sections 4.2 and 4.3).
 * Display light is decoded to 1.0 = 100 nits: SDR and HLG code 1.0 is the encoding's peak, PQ
 * is absolute. ACEScc itself converts without arithmetic.
 *
 * Immutable after construction and safe to share between threads. The referenced encodings are
 * catalog entries with static storage.
 */
class LmtEncodingConversion {
 public:
  /// @throws std::invalid_argument when a display encoding has an invalid peak luminance.
  LmtEncodingConversion(const color::ColorEncoding& input, const color::ColorEncoding& output);

  /// ACEScc AP1 to the LUT input code values. Values are not clamped to the LUT domain.
  [[nodiscard]] auto AcesccToLutInput(const LutRgb& acescc) const -> LutRgb;
  /// LUT output code values to ACEScc AP1.
  [[nodiscard]] auto LutOutputToAcescc(const LutRgb& code) const -> LutRgb;
  /// AcesccToLutInput, SampleLutTable and LutOutputToAcescc of one point.
  [[nodiscard]] auto Compose(const PackedGradeLut& source, const LutRgb& acescc) const -> LutRgb;

  [[nodiscard]] auto Input() const -> const color::ColorEncoding& { return *input_; }
  [[nodiscard]] auto Output() const -> const color::ColorEncoding& { return *output_; }

 private:
  const color::ColorEncoding*                  input_;
  const color::ColorEncoding*                  output_;
  /// AP1 to the input gamut (scene input) or AP1 to AP0 (display input), row-major.
  color::Matrix33d                             input_matrix_{};
  /// Output gamut to AP1 (scene output), row-major. Unused for a display output.
  color::Matrix33d                             output_matrix_{};
  std::shared_ptr<const Aces2ReferenceRuntime> input_runtime_;
  std::shared_ptr<const Aces2ReferenceRuntime> output_runtime_;
};

/**
 * @brief Bake the 65^3 ACEScc to ACEScc composite table of @p source with @p input and
 * @p output encodings.
 *
 * Grid node (r, g, b) holds Compose(source, (r, g, b) / 64). The result is RGBA32F with X
 * varying fastest, the layout of PackCubeLutRgba. The blue slices run on a process-wide
 * worker pool; the call blocks until the table is complete.
 * @throws std::invalid_argument when @p source has no table or a display encoding has an
 *         invalid peak luminance.
 */
[[nodiscard]] auto BakeLmtCompositeTable(const PackedGradeLut&       source,
                                         const color::ColorEncoding& input,
                                         const color::ColorEncoding& output)
    -> std::vector<std::byte>;

/**
 * @brief The table the primary grade pass samples for @p source and the two encodings.
 *
 * ACEScc to ACEScc with a unit domain returns @p source itself, so its bytes and key are the
 * ones uploaded before encodings existed. Otherwise the composite table is baked once per
 * (source key, domain, input id, output id) and kept in a process-wide LRU of 16 entries; its
 * key mixes the composite bytes. Thread-safe; concurrent misses for one key may bake twice and
 * keep the first result.
 * @throws std::invalid_argument when an encoding id is not in the catalog.
 */
[[nodiscard]] auto ResolveLmtSampledTable(std::shared_ptr<const PackedGradeLut> source,
                                          std::string_view                      input_encoding_id,
                                          std::string_view                      output_encoding_id)
    -> std::shared_ptr<const PackedGradeLut>;

/// Number of composite tables that ResolveLmtSampledTable has baked in this process.
[[nodiscard]] auto LmtCompositeBakeCount() -> std::uint64_t;

}  // namespace alcedo
