//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alcedo {

/// Author declaration of a LUT file. It classifies the file; it is not a signature.
enum class LutOrigin { kUnannotated, kAlcedo, kUser };

/// Classification category declared by the metadata comment.
enum class LutCategory { kGeneral, kFilmSimulation };

/// Descriptive kind of a print stock. Both kinds count as "with print".
enum class LutPrintKind { kFilm, kPaper };

struct LutSourceInfo {
  std::string id;
  std::string name;
};

struct LutFilmInfo {
  std::string id;
  std::string name;
  std::string brand;
};

struct LutPrintInfo {
  std::string  id;
  std::string  name;
  std::string  brand;
  LutPrintKind kind = LutPrintKind::kFilm;
};

/// Parsed schema-1 `ALCEDO_LUT` comment. See docs/lut-package-system.md.
struct LutMetadata {
  std::string                  id;
  LutOrigin                    origin   = LutOrigin::kUser;
  LutCategory                  category = LutCategory::kGeneral;
  std::optional<LutSourceInfo> source;
  std::optional<LutFilmInfo>   film;
  std::optional<LutPrintInfo>  print;
  std::string                  variant;
  std::string                  input_space;
  std::string                  output_space;
  std::string                  description;
  std::string                  display_requirement;
  std::vector<std::string>     aliases;
};

/// Reason a CUBE header could not be classified. `kNone` means success.
enum class LutHeaderError {
  kNone,
  kUnreadable,
  kHeaderTooLarge,
  kInvalidDirective,
  kNoNumericTable,
  kDuplicateMetadata,
  kMetadataLineTooLarge,
  kInvalidMetadataJson,
  kUnsupportedSchema,
  kInvalidMetadataField,
};

/// Header facts read before the first numeric row. The numeric table is never loaded.
struct LutHeader {
  std::optional<LutMetadata> metadata;
  std::string                title;
  int                        lut_3d_size = 0;
  int                        lut_1d_size = 0;

  /// The grade runtime applies 3D cubes without a 1D shaper only; 1D-only and shaper files stay
  /// visible but unusable (lut_color_encoding_plan.md, section 6.4).
  [[nodiscard]] auto         SupportsGradeApplication() const -> bool {
    return lut_3d_size > 0 && lut_1d_size == 0;
  }
  [[nodiscard]] auto         Origin() const -> LutOrigin {
    return metadata ? metadata->origin : LutOrigin::kUnannotated;
  }
};

struct LutHeaderReadResult {
  LutHeaderError     error = LutHeaderError::kNone;
  /// Human-readable diagnostic when `error != kNone`.
  std::string        message;
  LutHeader          header;

  [[nodiscard]] auto Ok() const -> bool { return error == LutHeaderError::kNone; }
};

inline constexpr std::size_t kLutHeaderLimitBytes       = 64 * 1024;
inline constexpr std::size_t kLutMetadataLineLimitBytes = 16 * 1024;

/// Parse a CUBE header from @p bytes, stopping at the first numeric row.
///
/// Accepts an optional UTF-8 BOM and LF or CRLF lines. Records TITLE,
/// LUT_3D_SIZE, LUT_1D_SIZE, and at most one `ALCEDO_LUT` comment. Invalid
/// metadata is an error result, never an unannotated LUT. Pure function.
[[nodiscard]] auto           ReadLutHeader(std::string_view bytes) -> LutHeaderReadResult;

/// Read at most the header limit plus one metadata line of @p path and parse it.
/// I/O failure returns `kUnreadable`. Safe to call concurrently for different files.
[[nodiscard]] auto ReadLutHeaderFile(const std::filesystem::path& path) -> LutHeaderReadResult;

/// Parse and validate the JSON payload of an `ALCEDO_LUT` comment.
/// On failure returns std::nullopt and writes the error kind and message.
[[nodiscard]] auto ParseLutMetadataJson(std::string_view json, LutHeaderError* error,
                                        std::string* message) -> std::optional<LutMetadata>;

/// Serialize @p metadata as the compact schema-1 JSON object used in the comment.
[[nodiscard]] auto SerializeLutMetadataJson(const LutMetadata& metadata) -> std::string;

/// Return the browser name of a LUT.
///
/// An `origin: alcedo` film simulation is titled by film brand and stock
/// (`Kodak Vision3 250D`; the brand is not repeated when the film name already
/// starts with it). Its print is not part of the title; it is a separate option
/// (`LutPrintOptionName`). Every other file shows @p file_stem.
[[nodiscard]] auto LutDisplayName(const LutHeader& header, std::string_view file_stem)
    -> std::string;

/// Return the print option of an official film simulation, or an empty string
/// when the LUT has no print or is not an official film simulation.
[[nodiscard]] auto LutPrintOptionName(const LutHeader& header) -> std::string;

/// Return the canonical `<film>[__<print>][__<variant>]` stem of a film simulation,
/// or an empty string when @p metadata has no film. Official file names use it,
/// so a `<film.id>` prefix finds every print option of one film.
[[nodiscard]] auto CanonicalLutFileStem(const LutMetadata& metadata) -> std::string;

}  // namespace alcedo
