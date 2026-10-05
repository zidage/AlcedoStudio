//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "image/image_content_class.hpp"

#include <libraw/libraw.h>

#include <cstring>
#include <memory>

#include "image/raster_container_reader.hpp"

namespace alcedo {
namespace {

auto StartsWith(std::span<const std::byte> bytes, std::initializer_list<unsigned char> magic)
    -> bool {
  if (bytes.size() < magic.size()) {
    return false;
  }
  std::size_t i = 0;
  for (const auto value : magic) {
    if (std::to_integer<unsigned char>(bytes[i++]) != value) {
      return false;
    }
  }
  return true;
}

/// LibRaw opens the TIFF container and reports a camera make and a color matrix.
auto LibRawReportsCamera(std::span<const std::byte> bytes) -> bool {
  // Heap allocation: LibRaw is large (see AGENTS.md on WebGPU RAW tests).
  auto raw = std::make_unique<LibRaw>();
  if (raw->open_buffer(bytes.data(), bytes.size()) != LIBRAW_SUCCESS) {
    return false;
  }
  const bool has_make   = raw->imgdata.idata.make[0] != '\0';
  bool       has_matrix = false;
  for (const auto& row : raw->imgdata.color.cam_xyz) {
    for (const float value : row) {
      has_matrix = has_matrix || value != 0.0f;
    }
  }
  raw->recycle();
  return has_make && has_matrix;
}

}  // namespace

auto ClassifyImageContent(std::span<const std::byte> bytes) -> ImageContentClass {
  if (StartsWith(bytes, {0xFF, 0xD8, 0xFF})) {
    return ImageContentClass::Jpeg;
  }
  if (StartsWith(bytes, {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A})) {
    return ImageContentClass::Png;
  }
  if (StartsWith(bytes, {0x76, 0x2F, 0x31, 0x01})) {
    return ImageContentClass::OpenExr;
  }
  if (StartsWith(bytes, {'I', 'I', 0x2A, 0x00}) || StartsWith(bytes, {'M', 'M', 0x00, 0x2A})) {
    try {
      if (ReadTiffContainer(bytes).has_dng_version_) {
        return ImageContentClass::Raw;
      }
    } catch (const RasterContainerError&) {
      // A TIFF-magic file whose first directory cannot be read is left to LibRaw.
      return ImageContentClass::Unknown;
    }
    return LibRawReportsCamera(bytes) ? ImageContentClass::Raw : ImageContentClass::Tiff;
  }
  return ImageContentClass::Unknown;
}

auto RasterFileKindFor(ImageContentClass content_class) -> std::optional<RasterFileKind> {
  switch (content_class) {
    case ImageContentClass::Jpeg:
      return RasterFileKind::Jpeg;
    case ImageContentClass::Png:
      return RasterFileKind::Png;
    case ImageContentClass::Tiff:
      return RasterFileKind::Tiff;
    case ImageContentClass::OpenExr:
      return RasterFileKind::OpenExr;
    case ImageContentClass::Raw:
    case ImageContentClass::Unknown:
      break;
  }
  return std::nullopt;
}

}  // namespace alcedo
