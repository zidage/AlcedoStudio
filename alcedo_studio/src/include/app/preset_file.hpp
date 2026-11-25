//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "app/adjustment_transfer_types.hpp"

namespace alcedo {

/// File extension of a preset file (edit presets plan, D1).
inline constexpr std::string_view kPresetFileExtension         = ".alcpreset";
/// Largest preset file that the reader opens (plan 3.1).
inline constexpr std::uintmax_t   kPresetFileMaxBytes          = std::uintmax_t{4} * 1024 * 1024;
/// Deepest JSON nesting of a preset file; the top-level object is level 1 (plan 3.1).
inline constexpr std::size_t      kPresetJsonMaxDepth          = 16;
/// Count limits of one preset package (plan 3.2).
inline constexpr std::size_t      kPresetMaxColorGrades        = 32;
inline constexpr std::size_t      kPresetMaxAdjustmentsPerNode = 64;
inline constexpr std::size_t      kPresetMaxMasksPerGrade      = 64;
/// Longest string value or object key of a preset file, in UTF-8 bytes (plan 3.2).
inline constexpr std::size_t      kPresetMaxStringBytes        = 256;
/// Longest preset name, in UTF-16 code units (plan 3.5).
inline constexpr std::size_t      kPresetMaxNameLength         = 120;

/// The rule of edit presets plan section 3 that a preset file or name failed.
enum class PresetFileRule : std::uint8_t {
  kNone,
  /// The path is not a regular file: a folder, a device, or a link (a symbolic link, a junction,
  /// or another name-surrogate reparse point).
  kNotRegularFile,
  kFileTooLarge,
  /// The file cannot be opened or read (an absent file included).
  kReadFailed,
  /// The bytes are not UTF-8 without errors.
  kInvalidText,
  kTooDeep,
  kInvalidJson,
  /// A string value or an object key is longer than kPresetMaxStringBytes.
  kStringTooLong,
  /// A number is not finite.
  kNonFiniteNumber,
  /// More Color Grades, adjustments of one node, or Masks of one Grade than the limits allow.
  kCountLimit,
  kMissingFingerprint,
  /// The fingerprint does not match, or the file text is not the canonical dump.
  kNotCanonical,
  /// ImportDocumentTransfer refused the package (schema, version, field, type, or parameter).
  kInvalidPackage,
  /// A LUT or lens database reference to a file outside the libraries (plan 3.3).
  kFileReference,
  /// The preset name cannot be a file name (plan 3.5).
  kInvalidName,
  /// The target file exists; no operation overwrites a file.
  kTargetExists,
  kWriteFailed,
};

/// Short stable text of @p rule for logs and error messages, such as `file too large`.
[[nodiscard]] auto PresetFileRuleText(PresetFileRule rule) -> std::string_view;

/**
 * @brief Result of a preset read: the package, or the failed rule and its reason.
 *
 * @p package is set only when @p rule is kNone. @p canonical_text is the file text that was
 * read; callers that copy an accepted file write these bytes, not the source file again.
 */
struct PresetFileReadResult {
  std::optional<AdjustmentTransferPackage> package;
  PresetFileRule                           rule = PresetFileRule::kNone;
  std::string                              message;
  std::string                              canonical_text;

  [[nodiscard]] auto Ok() const -> bool { return rule == PresetFileRule::kNone; }
};

/// Result of a preset write. @p rule is kNone when the new file exists with the canonical text.
struct PresetFileWriteResult {
  PresetFileRule     rule = PresetFileRule::kNone;
  std::string        message;

  [[nodiscard]] auto Ok() const -> bool { return rule == PresetFileRule::kNone; }
};

/**
 * @brief The only reader of a preset file from disk (edit presets plan, section 3).
 *
 * Refuses a path that is not a regular file (a link included; the target of a link is never
 * opened) and a file larger than kPresetFileMaxBytes, measured on the open handle before the bytes
 * are read. Then applies ParsePresetText. Never throws.
 *
 * @param path Path of the preset file. The file is opened read-only.
 * @return The package, or the first failed rule with a message that names the file.
 */
[[nodiscard]] auto ReadPresetFile(const std::filesystem::path& path) -> PresetFileReadResult;

/**
 * @brief Apply the text and package rules of section 3 to the bytes of one preset file.
 *
 * In order: UTF-8 without errors; a scan of the JSON that stops at the first value deeper than
 * kPresetJsonMaxDepth, the first string or key longer than kPresetMaxStringBytes, and the first
 * number that is not finite, before the JSON is built; the count limits; a required fingerprint;
 * the file reference rules of CheckPresetReferences on the JSON; ImportDocumentTransfer; the
 * fingerprint and the canonical text. Never throws.
 */
[[nodiscard]] auto ParsePresetText(std::string_view bytes) -> PresetFileReadResult;

/**
 * @brief Refuse the file references of plan 3.3.
 *
 * Refuses an LMT `cube_path` (an absolute file path), an LMT library reference that is not a
 * relative path below its root, and a `lens_calib.lens_profile_db_path` other than empty or the
 * built-in database (the DevelopPayload default). Accepts official LUT references.
 *
 * @return kNone, or kFileReference with a message that names the item.
 */
[[nodiscard]] auto CheckPresetReferences(const AdjustmentTransferPackage& package)
    -> PresetFileWriteResult;

/**
 * @brief The reason why @p name cannot be a preset name, or an empty string.
 *
 * A name is not empty, has no leading or trailing space, no path separator, no control
 * character, none of `<>:"|?*`, no trailing dot, is not a reserved Windows device name (also with
 * an extension), and is at most kPresetMaxNameLength UTF-16 code units long (plan 3.5).
 */
[[nodiscard]] auto PresetNameError(std::string_view name) -> std::string;

/**
 * @brief A valid preset name made from @p label, such as a Version name.
 *
 * Replaces each character that PresetNameError refuses (a control character, a path separator,
 * `<>:"|?*`) with `-`, removes leading and trailing spaces and trailing dots, and shortens the text
 * to kPresetMaxNameLength. An empty result is `Preset`; a reserved device name gets ` Preset`.
 * A name that the user types is checked with PresetNameError instead, and never changed.
 */
[[nodiscard]] auto PresetNameFromLabel(std::string_view label) -> std::string;

/// File name of the preset @p name: the name plus kPresetFileExtension. @p name must be valid.
[[nodiscard]] auto PresetFileNameFor(std::string_view name) -> std::filesystem::path;

/// What a preset write does with a file that exists at its path.
enum class PresetFileWriteMode {
  /// Refuse an existing file (kTargetExists): the preset folder never loses a file.
  kCreateNew,
  /// Replace an existing regular file: an export to a path that the user confirmed in the system
  /// save dialog.
  kReplace,
};

/**
 * @brief Write @p package as a preset file at @p path.
 *
 * Makes the canonical dump of ExportDocumentTransfer, applies ParsePresetText to it, so that the
 * reader accepts each written file, then writes a temporary file in the folder of @p path and
 * renames it to @p path. With kCreateNew the rename refuses an existing file and no file is
 * replaced; with kReplace the rename replaces it. A failure leaves no new file and keeps an
 * existing file. Never throws.
 */
[[nodiscard]] auto WritePresetFile(const AdjustmentTransferPackage& package,
                                   const std::filesystem::path&     path,
                                   PresetFileWriteMode mode = PresetFileWriteMode::kCreateNew)
    -> PresetFileWriteResult;

/**
 * @brief Write the accepted preset text @p canonical_text as a file at @p path.
 *
 * The same temporary file, rename, and @p mode as WritePresetFile. The caller has the text from an
 * accepted PresetFileReadResult. Never throws.
 */
[[nodiscard]] auto WritePresetText(std::string_view             canonical_text,
                                   const std::filesystem::path& path,
                                   PresetFileWriteMode mode = PresetFileWriteMode::kCreateNew)
    -> PresetFileWriteResult;

}  // namespace alcedo
