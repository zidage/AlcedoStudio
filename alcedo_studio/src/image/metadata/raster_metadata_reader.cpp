//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "raster_metadata_reader.hpp"

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "hdr_marker_probe.hpp"
#include "image/image_content_class.hpp"
#include "image/metadata_extractor.hpp"
#include "read_only_file_mapping.hpp"

namespace alcedo::metadata_detail {
namespace {
OIIO_NAMESPACE_USING

auto ReadFileHead(const image_path_t& image_path, std::size_t byte_count)
    -> std::vector<std::byte> {
  std::ifstream ifs(image_path, std::ios::binary);
  if (!ifs.is_open()) {
    return {};
  }
  std::vector<std::byte> bytes(byte_count);
  ifs.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, ifs.gcount())));
  return bytes;
}

auto ImageTypeFor(RasterFileKind kind) -> ImageType {
  switch (kind) {
    case RasterFileKind::Jpeg:
      return ImageType::JPEG;
    case RasterFileKind::Png:
      return ImageType::PNG;
    case RasterFileKind::Tiff:
      return ImageType::TIFF;
    case RasterFileKind::OpenExr:
      return ImageType::EXR;
  }
  throw std::invalid_argument("unknown raster file kind");
}
}  // namespace

auto ImportCategoryFor(RasterFileKind kind) -> ImportFileCategory {
  switch (kind) {
    case RasterFileKind::Jpeg:
      return ImportFileCategory::Jpeg;
    case RasterFileKind::Png:
      return ImportFileCategory::Png;
    case RasterFileKind::Tiff:
      return ImportFileCategory::Tiff;
    case RasterFileKind::OpenExr:
      return ImportFileCategory::OpenExr;
  }
  throw std::invalid_argument("unknown raster file kind");
}

namespace {
OIIO_NAMESPACE_USING

auto TransferIsHdr(const RasterColorDescription& description) -> bool {
  for (const auto& transfer : description.transfer_) {
    if (transfer.kind_ == RasterTransferKind::St2084 || transfer.kind_ == RasterTransferKind::Hlg) {
      return true;
    }
  }
  return false;
}

/// Width, height and orientation from the decoder header. The dimensions are those of the
/// oriented image, as for RAW files (SetDisplayDimensionsFromLibRaw).
void PopulateRasterDimensionsFromHeader(const image_path_t&        image_path,
                                        std::span<const std::byte> bytes, RasterFileKind kind,
                                        ExifDisplayMetaData& display) {
  // The format name follows the content, so a renamed file opens with the right reader.
  const char* name = "raster.jpg";
  switch (kind) {
    case RasterFileKind::Jpeg:
      name = "raster.jpg";
      break;
    case RasterFileKind::Png:
      name = "raster.png";
      break;
    case RasterFileKind::Tiff:
      name = "raster.tif";
      break;
    case RasterFileKind::OpenExr:
      name = "raster.exr";
      break;
  }
  OIIO::Filesystem::IOMemReader reader(bytes.data(), bytes.size());
  // With a config spec, OpenImageIO takes the proxy from its "oiio:ioproxy" attribute.
  OIIO::ImageSpec               config;
  OIIO::Filesystem::IOProxy*    proxy = &reader;
  config.attribute("oiio:ioproxy", OIIO::TypeDesc::PTR, &proxy);
  auto input = ImageInput::open(name, &config, &reader);
  if (!input) {
    throw MetadataExtractionError(ImportErrorCode::METADATA_EXTRACTION_FAILED, image_path,
                                  "the image header cannot be read: " + OIIO::geterror());
  }
  const ImageSpec& spec = input->spec();
  auto width            = static_cast<uint32_t>(spec.full_width > 0 ? spec.full_width : spec.width);
  auto height = static_cast<uint32_t>(spec.full_height > 0 ? spec.full_height : spec.height);
  if (width == 0 || height == 0) {
    throw MetadataExtractionError(ImportErrorCode::METADATA_EXTRACTION_FAILED, image_path,
                                  "the image header has no dimensions");
  }
  if (spec.get_int_attribute("Orientation", 1) >= 5) {
    std::swap(width, height);
  }
  display.width_  = width;
  display.height_ = height;
  if (display.make_.empty()) display.make_ = spec.get_string_attribute("Make");
  if (display.model_.empty()) display.model_ = spec.get_string_attribute("Model");
  input->close();
}
}  // namespace

auto ClassifyRasterFile(const image_path_t& image_path) -> std::optional<RasterFileKind> {
  // The magic numbers need 8 bytes. A TIFF container is classified from the whole file, because
  // its DNG version tag and the LibRaw check decide RAW or raster. Those read the first directory
  // and the RAW headers only, so the file is mapped: the OS reads just those pages. Every RAW
  // format with TIFF magic (ARW, NEF, CR2, RW2, DNG) takes this branch, and a copy of the file read
  // the whole 25-100 MB of each one and held it per import worker.
  const auto head = ReadFileHead(image_path, 8);
  const bool tiff_magic =
      head.size() >= 2 && ((head[0] == std::byte{'I'} && head[1] == std::byte{'I'}) ||
                           (head[0] == std::byte{'M'} && head[1] == std::byte{'M'}));
  if (tiff_magic) {
    const ReadOnlyFileMapping mapping(image_path);
    if (mapping.Bytes().empty()) {
      throw MetadataExtractionError(ImportErrorCode::READ_FAILED, image_path,
                                    "the file cannot be mapped for reading");
    }
    return RasterFileKindFor(ClassifyImageContent(std::as_bytes(mapping.Bytes())));
  }
  return RasterFileKindFor(ClassifyImageContent(head));
}

void ExtractRasterMetadataToImage(const image_path_t& image_path, RasterFileKind kind,
                                  Image& image) {
  // The color description, EXIF and header sit in segments and directories that the parsers
  // locate; mapping the file lets the OS read only those pages instead of every byte of a large
  // JPEG or TIFF.
  const ReadOnlyFileMapping mapping(image_path);
  const auto                bytes = std::as_bytes(mapping.Bytes());
  if (bytes.empty()) {
    throw MetadataExtractionError(ImportErrorCode::READ_FAILED, image_path,
                                  "the file cannot be mapped for reading");
  }
  RasterColorDescription description;
  try {
    description = ResolveRasterColorDescription(bytes, kind);
  } catch (const RasterColorDescriptionError& e) {
    const auto code = e.reason() == RasterColorDescriptionError::Reason::UnsupportedCmyk
                          ? ImportErrorCode::UNSUPPORTED_FORMAT
                          : ImportErrorCode::METADATA_EXTRACTION_FAILED;
    throw MetadataExtractionError(code, image_path, e.what());
  }

  ExifDisplayMetaData     display{};
  Exiv2::Image::UniquePtr exiv;
  if (kind != RasterFileKind::OpenExr) {
    try {
      exiv = MetadataExtractor::ExtractEXIFFromBuffer(
          reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
      display = MetadataExtractor::EXIFToDisplayMetaData(exiv);
    } catch (const Exiv2::Error&) {
      // A file without readable EXIF or XMP has display metadata from its header only.
      exiv.reset();
    }
  }
  PopulateRasterDimensionsFromHeader(image_path, bytes, kind, display);
  display.is_hdr_ = kind == RasterFileKind::OpenExr || TransferIsHdr(description) ||
                    DetectHdrMetadata(image_path, exiv.get());

  image.image_type_ = ImageTypeFor(kind);
  image.SetExifDisplayMetaData(std::move(display));
  image.SetRasterColorDescription(std::move(description));
}

}  // namespace alcedo::metadata_detail
