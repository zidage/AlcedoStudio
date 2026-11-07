//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "dng_import_fast_path.hpp"

#include <OpenImageIO/imageio.h>
#include <libraw/libraw.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

#include "camera_color_matrix_db.hpp"
#include "dng_color_metadata.hpp"
#include "image/lens_identity_resolver.hpp"
#include "image/metadata_extractor.hpp"
#include "libraw_metadata_mapper.hpp"
#include "metadata_values.hpp"

namespace alcedo::metadata_detail {
namespace {
OIIO_NAMESPACE_USING

auto PopulateDisplayDimensionsFromOiio(const image_path_t& image_path, ExifDisplayMetaData& display)
    -> bool {
  try {
    auto input = ImageInput::open(PathToUtf8(image_path));
    if (!input) {
      return false;
    }

    const ImageSpec& spec   = input->spec();
    const int        width  = spec.full_width > 0 ? spec.full_width : spec.width;
    const int        height = spec.full_height > 0 ? spec.full_height : spec.height;
    if (width <= 0 || height <= 0) {
      return false;
    }

    display.width_  = static_cast<uint32_t>(width);
    display.height_ = static_cast<uint32_t>(height);
    return true;
  } catch (...) {
    return false;
  }
}
}  // namespace

auto ExtractDngMetadataToImageFast(const image_path_t& image_path, Image& image) -> bool {
  Exiv2::Image::UniquePtr exif_image;
  try {
    exif_image = MetadataExtractor::ExtractEXIF(image_path);
  } catch (...) {
    return false;
  }

  if (!exif_image || exif_image->exifData().empty()) {
    return false;
  }

  ExifDisplayMetaData    display = MetadataExtractor::EXIFToDisplayMetaData(exif_image);
  RawRuntimeColorContext ctx{};

  // LibRaw open_file is cheap and preserves the same camera/model strings
  // the old import path used, without paying the DNG unpack cost.
  // The open is also the RAW admission check: a `.dng` name with EXIF but content LibRaw
  // cannot open (for example a renamed JPEG) is not a DNG.
  auto                   raw_processor = std::make_unique<LibRaw>();
#if defined(_WIN32)
  const int open_ret = raw_processor->open_file(image_path.wstring().c_str());
#else
  const int open_ret = raw_processor->open_file(image_path.string().c_str());
#endif
  if (open_ret != LIBRAW_SUCCESS) {
    raw_processor->recycle();
    return false;
  }
  PopulateDngMetadataHintFromOpenLibRaw(
      *raw_processor, LensIdentityResolver::Resolve(*raw_processor, exif_image->exifData()),
      display, ctx);
  raw_processor->recycle();

  MergeMetadataHint(&display, ctx);

  PopulateDngColorMetadataFromExif(exif_image->exifData(), ctx);
  if (!ctx.color_matrices_valid_) {
    ctx.color_matrices_valid_ = LookupCameraColorMatrices(ctx.camera_make_, ctx.camera_model_,
                                                          ctx.color_matrix_1_, ctx.color_matrix_2_);
    if (ctx.color_matrices_valid_) {
      MarkAdobeCameraMatrixDatabaseIlluminants(ctx);
    }
  }

  ctx.crop_factor_hint_    = ResolveCropFactorHint(ctx.focal_length_mm_, ctx.focal_35mm_mm_);
  ctx.lens_metadata_valid_ = !ctx.lens_model_.empty() && std::isfinite(ctx.focal_length_mm_) &&
                             ctx.focal_length_mm_ > 0.0f;
  if (const auto dng_metadata = ExtractDngGeometryMetadataFromFile(image_path);
      dng_metadata.has_value()) {
    ctx.dng_warp_rectilinear_present_ = dng_metadata->warp_rectilinear.has_value();
  }

  ctx.valid_                  = true;
  ctx.output_in_camera_space_ = true;

  display.make_               = ctx.camera_make_;
  display.model_              = ctx.camera_model_;
  display.lens_make_          = ctx.lens_make_;
  display.lens_               = ctx.lens_model_;
  display.focal_              = ctx.focal_length_mm_;
  display.aperture_           = ctx.aperture_f_number_;
  display.focus_distance_m_   = ctx.focus_distance_m_;
  display.focal_35mm_         = ctx.focal_35mm_mm_;

  if (display.width_ == 0 || display.height_ == 0) {
    // Exiv2 often reports the embedded preview dimensions for DNG. Use OIIO
    // only as a last-resort size probe if LibRaw open_file did not expose it.
    PopulateDisplayDimensionsFromOiio(image_path, display);
  }
  // RAW import reads no HDR marker: is_hdr_ stays false until an HDR edit output sets it
  // (PersistImageHdrFlag). The OIIO and 4 MiB signature probes read 4 MiB of every RAW file.

  image.SetExifDisplayMetaData(std::move(display));
  image.SetRawColorContext(std::move(ctx));
  return true;
}

}  // namespace alcedo::metadata_detail
