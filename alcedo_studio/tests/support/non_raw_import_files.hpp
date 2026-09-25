//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Writers for small non-RAW files that import must reject by content
// (library_search_and_project_size_plan.md, Phase S1, Decision D2a).
//
// Each writer produces a file that a real reader recognizes as its format: OpenImageIO
// encodes the JPEG and TIFF rasters, the XMP packet is one Exiv2 opens as an XMP sidecar,
// and the QuickTime file starts with a valid `ftyp` box. Consumers must link
// OpenImageIO::OpenImageIO.

#pragma once

#include <OpenImageIO/imageio.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace alcedo::test_support {

/// Encode a 32x24 RGB8 gradient to @p path. The OpenImageIO writer is chosen from the
/// extension in @p format_extension (for example ".jpg" or ".tif"), so the file content can
/// differ from the extension of @p path. Throws std::runtime_error when encoding fails.
inline void WriteRgbRaster(const std::filesystem::path& path, std::string_view format_extension) {
  constexpr int             kWidth = 32, kHeight = 24, kChannels = 3;
  std::vector<std::uint8_t> pixels(kWidth * kHeight * kChannels);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      auto* px = &pixels[(y * kWidth + x) * kChannels];
      px[0]    = static_cast<std::uint8_t>(x * 8);
      px[1]    = static_cast<std::uint8_t>(y * 10);
      px[2]    = static_cast<std::uint8_t>(128);
    }
  }
  const auto encode_path =
      path.parent_path() / (path.stem().string() + "_encode" + std::string(format_extension));
  auto output = OIIO::ImageOutput::create(encode_path.string());
  if (!output) {
    throw std::runtime_error("No OpenImageIO writer for " + std::string(format_extension));
  }
  const OIIO::ImageSpec spec(kWidth, kHeight, kChannels, OIIO::TypeDesc::UINT8);
  if (!output->open(encode_path.string(), spec) ||
      !output->write_image(OIIO::TypeDesc::UINT8, pixels.data()) || !output->close()) {
    throw std::runtime_error("OpenImageIO failed to write " + encode_path.string());
  }
  output.reset();
  std::filesystem::rename(encode_path, path);
}

/// Write an XMP sidecar packet that Exiv2 opens as ImageType::xmp.
inline void WriteXmpSidecar(const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
         "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
         " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
         "  <rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\""
         " xmp:Rating=\"3\"/>\n"
         " </rdf:RDF>\n"
         "</x:xmpmeta>\n"
         "<?xpacket end=\"w\"?>\n";
  if (!out) throw std::runtime_error("Failed to write " + path.string());
}

/// Write a QuickTime container header: an `ftyp` box with brand `qt  ` and an empty `mdat`.
inline void WriteQuickTimeHeader(const std::filesystem::path& path) {
  constexpr std::array<std::uint8_t, 28> kBytes = {
      0x00, 0x00, 0x00, 0x14, 'f', 't', 'y',  'p',  'q',  't',  ' ', ' ', 0x00, 0x00,
      0x00, 0x00, 'q',  't',  ' ', ' ', 0x00, 0x00, 0x00, 0x08, 'm', 'd', 'a',  't'};
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(kBytes.data()), kBytes.size());
  if (!out) throw std::runtime_error("Failed to write " + path.string());
}

/// Write 4 KiB of deterministic bytes that no image reader recognizes.
inline void WriteUnknownBinary(const std::filesystem::path& path) {
  std::vector<char> bytes(4096);
  std::uint32_t     state = 0x12345678u;
  for (auto& byte : bytes) {
    state = state * 1664525u + 1013904223u;
    byte  = static_cast<char>(state >> 24);
  }
  bytes[0] = 0x7f;  // Keep the first byte off every known magic number.
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!out) throw std::runtime_error("Failed to write " + path.string());
}

}  // namespace alcedo::test_support
