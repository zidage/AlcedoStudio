//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lut_reference.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

/// Default LUT strength: the complete LUT result (displayed as 100%).
inline constexpr float            kDefaultLutStrength   = 1.0f;

/// Default LUT input and output encoding: ACEScc AP1, the working space the LUT is sampled in.
/// Equal to color::kDefaultColorEncodingId.
inline constexpr std::string_view kDefaultLutEncodingId = "acescc";

struct LmtPayload {
  LutReference reference;
  /// Last known LUT name, kept so a missing association can still be shown.
  std::string  display_name;
  /// Blend of the LUT result into its input, in [0, 1]. Independent of the Grade mix.
  float        strength = kDefaultLutStrength;
  /// Color encoding id (color_encoding_catalog.hpp) of the LUT's input code values.
  std::string  input_encoding{kDefaultLutEncodingId};
  /// Color encoding id of the LUT's output code values.
  std::string  output_encoding{kDefaultLutEncodingId};
};

enum class LmtDirty : std::uint32_t {
  None      = 0,
  Reference = 1U << 0,
  Strength  = 1U << 1,
  /// Input or output encoding. Renders like a Reference change: the LUT table is packed again.
  Encoding  = 1U << 2,
  All       = Reference | Strength | Encoding,
};

/**
 * @brief One focused change of an LMT adjustment: a new LUT selection, a new strength, new
 * encodings, or any combination.
 *
 * Unset members keep their current value. A selection replaces the reference and its
 * display name together and keeps the strength and the encodings. This is the typed editor
 * write of the `lut` field; it is not a copy of the live Model.
 */
struct LmtUpdate {
  std::optional<LutReference> reference;
  /// Name shown for @ref reference. Ignored without a reference; empty when not given.
  std::string                 display_name;
  std::optional<float>        strength;
  /// Catalog encoding id of the LUT input (FindColorEncoding).
  std::optional<std::string>  input_encoding;
  /// Catalog encoding id of the LUT output.
  std::optional<std::string>  output_encoding;
};

/// Empty when @p strength is a finite value in [0, 1], else the reason.
[[nodiscard]] auto ValidateLutStrength(float strength) -> std::string;

/// Empty when @p encoding_id names a color catalog encoding, else the reason.
[[nodiscard]] auto ValidateLutEncodingId(std::string_view encoding_id) -> std::string;

/**
 * @brief Look Modification Transform: the selected LUT reference, its strength, and the color
 * encodings of the LUT's input and output code values.
 *
 * No reference is identity. Setters do no file I/O; the render runtime resolves the
 * reference through its LutResourceResolver and bakes the encodings into the sampled table
 * (lut_bake.hpp). Serialized form keeps projects that store only `cube_path` byte-identical:
 * a file reference or no reference is written as `cube_path`, `reference` appears only for
 * official and library references, and `strength`, `name`, `input_encoding` and
 * `output_encoding` appear only when they differ from their defaults.
 */
class LmtModel final : public OperatorModelBase<LmtModel, LmtPayload, LmtDirty> {
 public:
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Lmt(); }
  static constexpr std::string_view kInstanceSuffix = "lmt";

  [[nodiscard]] auto IsDefault() const -> bool override;

  /// Select @p reference (or clear it with an empty reference). Keeps the strength and the
  /// encodings.
  /// @throws std::invalid_argument when the reference is invalid; nothing changes.
  void               SetReference(LutReference reference, std::string display_name = {});
  /// @throws std::invalid_argument unless @p strength is finite and in [0, 1]; nothing changes.
  void               SetStrength(float strength);
  /// Set the input and output encodings (catalog ids). Keeps the reference and the strength.
  /// @throws std::invalid_argument when an id is not in the catalog; nothing changes.
  void               SetEncodings(std::string input_encoding, std::string output_encoding);
  /// Apply a selection, strength and/or encoding change after validating every part, as one
  /// update.
  /// @throws std::invalid_argument for an invalid part; nothing changes.
  void               ApplyUpdate(const LmtUpdate& update);
  /// Select a file by absolute path; an empty path clears the reference.
  void               SetCubePath(std::string path);

  [[nodiscard]] auto Reference() const -> LutReference;
  [[nodiscard]] auto DisplayName() const -> std::string;
  [[nodiscard]] auto Strength() const -> float;
  /// Catalog encoding id of the LUT input; kDefaultLutEncodingId when never set.
  [[nodiscard]] auto InputEncoding() const -> std::string;
  /// Catalog encoding id of the LUT output; kDefaultLutEncodingId when never set.
  [[nodiscard]] auto OutputEncoding() const -> std::string;
  /// Path of a file reference, else empty.
  [[nodiscard]] auto CubePath() const -> std::string;

  [[nodiscard]] auto ToJson() const -> nlohmann::json override;
  /// Read the current form or the legacy `{"cube_path": ...}` form (strength 1, ACEScc to
  /// ACEScc).
  /// @throws std::invalid_argument for an invalid reference, strength or encoding id; nothing
  ///         changes.
  void               LoadJson(const nlohmann::json& json) override;
};

/// Parse the LMT Model JSON form (see LmtModel) into a complete update.
/// A missing `strength`, `input_encoding` or `output_encoding` reads as its default.
/// @throws std::invalid_argument when invalid, including an encoding id that is not in the
///         catalog (there is no substitute encoding).
[[nodiscard]] auto LmtUpdateFromModelJson(const nlohmann::json& json) -> LmtUpdate;

}  // namespace alcedo
