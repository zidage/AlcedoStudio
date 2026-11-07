//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "image/lens_identity_resolver.hpp"

#include <exiv2/image.hpp>
#include <exiv2/tags.hpp>
#include <string>

#include "metadata_values.hpp"
#include "nikon_lens_id_table.hpp"

namespace alcedo {
namespace {
using metadata_detail::IsNikonCamera;
using metadata_detail::ResolveNikonLensModel;
using metadata_detail::TrimAscii;
using metadata_detail::TrimTrailingZeroPadded;

/// Text of the EXIF tag @p key without leading and trailing spaces, or empty when the tag is
/// absent.
auto ExifText(const Exiv2::ExifData& exif_data, const char* key) -> std::string {
  const auto it = exif_data.findKey(Exiv2::ExifKey(key));
  if (it == exif_data.end()) {
    return {};
  }
  return TrimAscii(it->toString());
}

/// Fill each name of @p lens that is still empty from the EXIF sources.
void FillFromExif(const Exiv2::ExifData& exif_data, LensIdentity& lens) {
  const LensIdentity exif_lens = LensIdentityResolver::FromExif(exif_data);
  if (lens.make.empty()) {
    lens.make = exif_lens.make;
  }
  if (lens.model.empty()) {
    lens.model = exif_lens.model;
  }
}

/// Fill the empty names of @p lens from the EXIF that Exiv2 reads from @p exif_image.
void FillFromExifImage(Exiv2::Image& exif_image, LensIdentity& lens) {
  exif_image.readMetadata();
  FillFromExif(exif_image.exifData(), lens);
}
}  // namespace

auto LensIdentityResolver::FromLibRaw(const LibRaw& raw_processor) -> LensIdentity {
  const auto&  lens_info = raw_processor.imgdata.lens;
  LensIdentity lens;
  lens.make  = TrimTrailingZeroPadded(lens_info.LensMake);
  lens.model = TrimTrailingZeroPadded(lens_info.Lens);
  if (lens.model.empty()) {
    lens.model = TrimTrailingZeroPadded(lens_info.makernotes.Lens);
  }

  const std::string camera_make  = TrimAscii(raw_processor.imgdata.idata.make);
  const std::string camera_model = TrimAscii(raw_processor.imgdata.idata.model);
  if (IsNikonCamera(camera_make, camera_model)) {
    if (lens.make.empty()) {
      lens.make = "Nikon";
    }
    if (lens.model.empty()) {
      lens.model = ResolveNikonLensModel(lens_info);
    }
  }
  return lens;
}

auto LensIdentityResolver::Resolve(const LibRaw&                raw_processor,
                                   const std::filesystem::path& image_path) -> LensIdentity {
  LensIdentity lens = FromLibRaw(raw_processor);
  if (!lens.model.empty()) {
    return lens;
  }
  const std::string path_text = image_path.string();
  if (Exiv2::ImageFactory::getType(path_text) == Exiv2::ImageType::none) {
    return lens;
  }
  const auto exif_image = Exiv2::ImageFactory::open(path_text);
  FillFromExifImage(*exif_image, lens);
  return lens;
}

auto LensIdentityResolver::Resolve(const LibRaw& raw_processor, std::span<const std::byte> encoded)
    -> LensIdentity {
  LensIdentity lens = FromLibRaw(raw_processor);
  if (!lens.model.empty() || encoded.empty()) {
    return lens;
  }
  const auto* data = reinterpret_cast<const Exiv2::byte*>(encoded.data());
  if (Exiv2::ImageFactory::getType(data, encoded.size()) == Exiv2::ImageType::none) {
    return lens;
  }
  const auto exif_image = Exiv2::ImageFactory::open(data, encoded.size());
  FillFromExifImage(*exif_image, lens);
  return lens;
}

auto LensIdentityResolver::Resolve(const LibRaw& raw_processor, const Exiv2::ExifData& exif_data)
    -> LensIdentity {
  LensIdentity lens = FromLibRaw(raw_processor);
  FillFromExif(exif_data, lens);
  return lens;
}

auto LensIdentityResolver::FromExif(const Exiv2::ExifData& exif_data) -> LensIdentity {
  LensIdentity lens;
  lens.make  = ExifText(exif_data, "Exif.Photo.LensMake");
  lens.model = ExifText(exif_data, "Exif.Panasonic.LensType");
  if (lens.model.empty()) {
    lens.model = ExifText(exif_data, "Exif.Photo.LensModel");
  }
  return lens;
}

}  // namespace alcedo
