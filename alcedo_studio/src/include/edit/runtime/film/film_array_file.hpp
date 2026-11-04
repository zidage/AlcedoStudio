//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <json.hpp>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alcedo {

/// Version of the film print resource files. The manifest and each array file state it.
inline constexpr int kFilmResourceSchemaVersion = 7;

/// Element type of an array in a @ref FilmArrayFile.
enum class FilmArrayType { kF64, kF32, kI32, kI64, kU8 };

/**
 * @brief One array file of the film print resources, held in memory and read-only after parsing.
 *
 * Layout: 8 bytes `ALCDFILM`, a little-endian uint64 header length, a JSON header, and then the
 * arrays, each one little-endian and row major on an 8 byte boundary. The lab script
 * `build_product_resources.py` writes this form. Spans that the accessors return stay valid for
 * the life of the object.
 */
class FilmArrayFile {
 public:
  using ShapeList = std::initializer_list<std::int64_t>;

  /**
   * @brief Parse the bytes of a file.
   * @param bytes The complete file content.
   * @param path The file name for error messages.
   * @throws std::runtime_error that names @p path when the bytes are not a valid array file of
   *         the current schema version.
   */
  [[nodiscard]] static auto Parse(std::vector<std::byte> bytes, const std::filesystem::path& path)
      -> FilmArrayFile;

  /// UTF-8 text of the file name, for error messages.
  [[nodiscard]] auto PathText() const -> const std::string& { return path_text_; }

  /// The `metadata` object of the header.
  [[nodiscard]] auto Metadata() const -> const nlohmann::json& { return metadata_; }

  /**
   * @brief The values of a float64 array with exactly this shape.
   * @throws std::runtime_error that names the file and the array when the array is missing or has
   *         another type or shape.
   */
  [[nodiscard]] auto F64(std::string_view name, ShapeList shape) const -> std::span<const double>;
  /// @copydoc F64
  [[nodiscard]] auto F32(std::string_view name, ShapeList shape) const -> std::span<const float>;
  /// @copydoc F64
  [[nodiscard]] auto I32(std::string_view name, ShapeList shape) const
      -> std::span<const std::int32_t>;
  /// @copydoc F64
  [[nodiscard]] auto I64(std::string_view name, ShapeList shape) const
      -> std::span<const std::int64_t>;
  /// @copydoc F64
  [[nodiscard]] auto U8(std::string_view name, ShapeList shape) const
      -> std::span<const std::uint8_t>;

  /// True when the file holds an array of this name.
  [[nodiscard]] auto Contains(std::string_view name) const -> bool {
    return arrays_.find(name) != arrays_.end();
  }

  /**
   * @brief The shape of an array.
   * @throws std::runtime_error that names the file and the array when the array is missing.
   */
  [[nodiscard]] auto Shape(std::string_view name) const -> std::span<const std::int64_t>;

 private:
  struct ArrayEntry {
    FilmArrayType             type = FilmArrayType::kF64;
    std::vector<std::int64_t> shape;
    std::size_t               first_byte = 0;
    std::size_t               byte_count = 0;
  };

  [[nodiscard]] auto Find(std::string_view name) const -> const ArrayEntry&;
  [[nodiscard]] auto Checked(std::string_view name, FilmArrayType type, ShapeList shape) const
      -> const ArrayEntry&;

  std::string                                    path_text_;
  std::vector<std::byte>                         bytes_;
  nlohmann::json                                 metadata_;
  std::map<std::string, ArrayEntry, std::less<>> arrays_;
};

/// UTF-8 text of @p path, for error messages.
[[nodiscard]] auto FilmPathText(const std::filesystem::path& path) -> std::string;

/**
 * @brief The complete content of a file.
 * @throws std::runtime_error that names @p path when the file cannot be read.
 */
[[nodiscard]] auto ReadFilmResourceBytes(const std::filesystem::path& path)
    -> std::vector<std::byte>;

}  // namespace alcedo
