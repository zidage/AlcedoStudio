//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <libraw/libraw.h>

#include <cstddef>
#include <exiv2/exif.hpp>
#include <filesystem>
#include <span>
#include <string>

namespace alcedo {

/// Lens maker and lens model names of one RAW file. An empty string means that no source has the
/// name.
struct LensIdentity {
  std::string make;
  std::string model;
};

/**
 * @brief Reads the lens name of a RAW file for import and for Develop.
 *
 * Import (`MetadataExtractor`) and Develop (`RawInputLoader::LoadEncoded`) both call it, so the
 * library and Lens Correction use the same lens name.
 *
 * The lens model comes from the first source that has a name:
 * 1. LibRaw `lens.Lens`.
 * 2. LibRaw `lens.makernotes.Lens`.
 * 3. Nikon cameras: the lens ID table `config/nikon_lens/id_map.json`.
 * 4. `Exif.Panasonic.LensType`. In a Panasonic RW2 file, only the maker note of the embedded JPEG
 *    has this name; Exiv2 adds the EXIF of that JPEG when it reads the RW2 file.
 * 5. `Exif.Photo.LensModel`.
 *
 * The lens maker is LibRaw `lens.LensMake`, then `Nikon` for a Nikon camera, then
 * `Exif.Photo.LensMake`.
 *
 * Sources 1 to 3 read the opened LibRaw state. Sources 4 and 5 read Exiv2 EXIF. The overloads
 * that take a file or an encoded buffer read the EXIF only when sources 1 to 3 give no lens model,
 * and only when Exiv2 recognizes the file format. Thus the import time of a file that LibRaw
 * names does not change.
 */
class LensIdentityResolver {
 public:
  /// Lens of an opened LibRaw file from the LibRaw sources and the Nikon lens ID table only.
  [[nodiscard]] static auto FromLibRaw(const LibRaw& raw_processor) -> LensIdentity;

  /// Lens of the file at @p image_path, which @p raw_processor opened.
  /// @throws Exiv2::Error when Exiv2 recognizes the file format but cannot read its metadata.
  [[nodiscard]] static auto Resolve(const LibRaw&                raw_processor,
                                    const std::filesystem::path& image_path) -> LensIdentity;

  /// Lens of the encoded file @p encoded, which @p raw_processor opened.
  /// @throws Exiv2::Error when Exiv2 recognizes the file format but cannot read its metadata.
  [[nodiscard]] static auto Resolve(const LibRaw& raw_processor, std::span<const std::byte> encoded)
      -> LensIdentity;

  /// Lens of a file whose EXIF @p exif_data is already read. The EXIF sources fill each name
  /// that the LibRaw sources leave empty.
  [[nodiscard]] static auto Resolve(const LibRaw& raw_processor, const Exiv2::ExifData& exif_data)
      -> LensIdentity;

  /// Lens of the EXIF sources only: `Exif.Panasonic.LensType`, then `Exif.Photo.LensModel` for
  /// the model, and `Exif.Photo.LensMake` for the maker.
  [[nodiscard]] static auto FromExif(const Exiv2::ExifData& exif_data) -> LensIdentity;
};

}  // namespace alcedo
