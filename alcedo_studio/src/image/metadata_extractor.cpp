//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "image/metadata_extractor.hpp"

#include <libraw/libraw.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "image/lens_identity_resolver.hpp"
#include "metadata/dng_color_metadata.hpp"
#include "metadata/dng_import_fast_path.hpp"
#include "metadata/exif_display_mapper.hpp"
#include "metadata/hdr_marker_probe.hpp"
#include "metadata/libraw_metadata_mapper.hpp"
#include "metadata/metadata_values.hpp"
#include "metadata/raster_metadata_reader.hpp"
#include "type/supported_file_type.hpp"
#include "utils/logging/log.hpp"

// MetadataExtractor is the entry point of the metadata readers. The readers are in
// src/image/metadata/: raster files (raster_metadata_reader), DNG color tags and geometry
// (dng_color_metadata), the DNG import without unpack (dng_import_fast_path), LibRaw fields
// (libraw_metadata_mapper, with camera_color_matrix_db), the lens name (lens_identity_resolver,
// with nikon_lens_id_table), Exiv2 display values (exif_display_mapper) and HDR markers
// (hdr_marker_probe).

namespace alcedo {
namespace {
using metadata_detail::CollectLibRawCaptureTimeText;
using metadata_detail::DetectHdrMetadata;
using metadata_detail::ExtractDngGeometryMetadataFromFile;
using metadata_detail::ExtractDngMetadataToImageFast;
using metadata_detail::GetDisplayMetadataFromExif;
using metadata_detail::ImportCategoryFor;
using metadata_detail::IsDngExtension;
using metadata_detail::LibRawCaptureTimeText;
using metadata_detail::PopulateDisplayMetadataFromLibRaw;
using metadata_detail::PopulateDngColorMetadataFromExif;
using metadata_detail::PopulateMetadataRuntimeContext;
using metadata_detail::PopulateStandardRating;
}  // namespace

auto MetadataExtractor::ClassifyRasterFile(const image_path_t& image_path)
    -> std::optional<RasterFileKind> {
  return metadata_detail::ClassifyRasterFile(image_path);
}

void MetadataExtractor::ExtractRasterMetadata_ToImage(const image_path_t& image_path,
                                                      RasterFileKind kind, Image& image) {
  metadata_detail::ExtractRasterMetadataToImage(image_path, kind, image);
}

void MetadataExtractor::PopulateRuntimeContextFromOpenLibRaw(LibRaw&                 raw_processor,
                                                             RawRuntimeColorContext& ctx) {
  PopulateMetadataRuntimeContext(raw_processor, LensIdentityResolver::FromLibRaw(raw_processor),
                                 ctx);
}

auto MetadataExtractor::ExtractEXIF(const image_path_t& image_path) -> Exiv2::Image::UniquePtr {
  Exiv2::Image::UniquePtr image = Exiv2::ImageFactory::open(image_path.string());
  image->readMetadata();
  return image;
}

auto MetadataExtractor::ExtractEXIFFromBuffer(const uint8_t* buffer, size_t size)
    -> Exiv2::Image::UniquePtr {
  if (!buffer || size == 0) {
    throw std::runtime_error("MetadataExtractor: empty buffer");
  }
  Exiv2::Image::UniquePtr image =
      Exiv2::ImageFactory::open(reinterpret_cast<const Exiv2::byte*>(buffer), size);
  image->readMetadata();
  return image;
}

auto MetadataExtractor::DetectHdrDisplayMetadata(const image_path_t& image_path,
                                                 const Exiv2::Image* exif_image) -> bool {
  return DetectHdrMetadata(image_path, exif_image);
}

auto MetadataExtractor::EXIFToDisplayMetaData(const Exiv2::Image::UniquePtr& exif_data)
    -> ExifDisplayMetaData {
  ExifDisplayMetaData display_metadata;
  if (!exif_data) {
    return display_metadata;
  }
  if (!exif_data->exifData().empty()) {
    GetDisplayMetadataFromExif(exif_data->exifData(), display_metadata);
  }
  PopulateStandardRating(*exif_data, display_metadata);
  return display_metadata;
}

auto MetadataExtractor::BufferToDisplayMetaData(const uint8_t* buffer, size_t size)
    -> ExifDisplayMetaData {
  ExifDisplayMetaData display_metadata;
  try {
    auto exif_data = ExtractEXIFFromBuffer(buffer, size);
    if (!exif_data) {
      return display_metadata;
    }
    if (!exif_data->exifData().empty()) {
      GetDisplayMetadataFromExif(exif_data->exifData(), display_metadata);
    }
    PopulateStandardRating(*exif_data, display_metadata);
  } catch (...) {
    return ExifDisplayMetaData{};
  }
  return display_metadata;
}

auto MetadataExtractor::EXIFToJSON(const Exiv2::Image::UniquePtr& exif_data) -> nlohmann::json {
  // The full EXIF is too large, we only convert the display-friendly metadata to JSON
  nlohmann::json exif_json;
  auto           display_metadata = EXIFToDisplayMetaData(exif_data);
  exif_json                       = display_metadata.ToJson();
  return exif_json;
}

auto MetadataExtractor::ReadDngColorProfileFromSource(const image_path_t& image_path)
    -> DngColorProfilePtr {
  auto exif = ExtractEXIF(image_path);
  if (!exif || exif->exifData().empty()) {
    throw std::runtime_error("DNG profile: source metadata is unavailable for '" +
                             image_path.string() + "'");
  }
  RawRuntimeColorContext context;
  PopulateDngColorMetadataFromExif(exif->exifData(), context);
  return context.dng_profile_.Profile();
}

void MetadataExtractor::ExtractEXIF_ToImage(const image_path_t& image_path, Image& image,
                                            ImportCategoryMask allowed_categories) {
  const auto exclude = [&image_path](ImportFileCategory category) {
    throw MetadataExtractionError(ImportErrorCode::EXCLUDED_TYPE, image_path,
                                  "the content category " +
                                      std::to_string(static_cast<int>(category)) +
                                      " is not selected for import");
  };
  // The content decides RAW or raster, never the extension (raster_image_input_plan.md,
  // section 8). The decision is the one LoadEncodedImage makes at render time.
  if (const auto raster_kind = ClassifyRasterFile(image_path); raster_kind.has_value()) {
    const auto category = ImportCategoryFor(*raster_kind);
    if (!CategoryAllowed(allowed_categories, category)) {
      exclude(category);
    }
    ExtractRasterMetadata_ToImage(image_path, *raster_kind, image);
    return;
  }
  // ExtractRawMetadata_ToImage reads RAW files with LibRaw. Exiv2 adds metadata to DNG files only.
  if (ExtractRawMetadata_ToImage(image_path, image)) {
    if (!CategoryAllowed(allowed_categories, ImportFileCategory::Raw)) {
      exclude(ImportFileCategory::Raw);
    }
    return;
  }
  throw MetadataExtractionError(ImportErrorCode::UNSUPPORTED_FORMAT, image_path,
                                "not a supported RAW or raster image file");
}

auto MetadataExtractor::ExtractRawMetadata_ToImage(const image_path_t& image_path, Image& image)
    -> bool {
  if (IsDngExtension(image_path) && ExtractDngMetadataToImageFast(image_path, image)) {
    return true;
  }

  // LibRaw is large enough to blow worker-thread stacks under ASan if allocated locally.
  auto                  raw_processor = std::make_unique<LibRaw>();
  LibRawCaptureTimeText capture_time_text;
  raw_processor->set_exifparser_handler(&CollectLibRawCaptureTimeText, &capture_time_text);

#if defined(_WIN32)
  int ret = raw_processor->open_file(image_path.wstring().c_str());
#else
  int ret = raw_processor->open_file(image_path.string().c_str());
#endif
  if (ret != LIBRAW_SUCCESS) {
    ALCEDO_LOG_WARN(Import) << "MetadataExtractor: libraw open_file failed for '"
                            << image_path.string() << "' (error " << ret << ")";
    return false;
  }

  // Metadata only: open_file has parsed every field read below. unpack() would decode the whole
  // sensor image (most of the import time) and contributes no metadata.
  // The lens identity reads the EXIF only when LibRaw has no lens name. A DNG file reads its EXIF
  // for the color tags, so its lens identity uses that EXIF.
  RawRuntimeColorContext ctx{};
  if (IsDngExtension(image_path)) {
    auto exif_data = ExtractEXIF(image_path);
    if (!exif_data || exif_data->exifData().empty()) {
      throw std::runtime_error("DNG color profile metadata could not be read");
    }
    PopulateMetadataRuntimeContext(
        *raw_processor, LensIdentityResolver::Resolve(*raw_processor, exif_data->exifData()), ctx);
    if (const auto dng_metadata = ExtractDngGeometryMetadataFromFile(image_path);
        dng_metadata.has_value()) {
      ctx.dng_warp_rectilinear_present_ = dng_metadata->warp_rectilinear.has_value();
    }
    PopulateDngColorMetadataFromExif(exif_data->exifData(), ctx);
  } else {
    PopulateMetadataRuntimeContext(*raw_processor,
                                   LensIdentityResolver::Resolve(*raw_processor, image_path), ctx);
  }

  ExifDisplayMetaData display{};
  PopulateDisplayMetadataFromLibRaw(*raw_processor, ctx, capture_time_text, display);
  // RAW import reads no HDR marker: is_hdr_ stays false until an HDR edit output sets it
  // (PersistImageHdrFlag).

  raw_processor->recycle();

  image.SetExifDisplayMetaData(std::move(display));
  image.SetRawColorContext(std::move(ctx));
  return true;
}
}  // namespace alcedo
