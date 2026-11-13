//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "hdr_marker_probe.hpp"

#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <string>
#include <string_view>

#include "metadata_values.hpp"

namespace alcedo::metadata_detail {
namespace {
OIIO_NAMESPACE_USING

auto OiioMetadataSuggestsHdr(const ImageSpec& spec) -> bool {
  const auto inspect = [](std::string text) {
    text = ToLowerAscii(std::move(text));
    return ContainsAny(
        text, {"ultrahdr", "ultra hdr", "hdrgm", "gainmap", "gain map", "iso 21496", "iso:ts:21496",
               "rec2100", "rec.2100", "bt2100", "bt.2100", "itur_2100", "itur-2100", "st2084",
               "st 2084", "smpte2084", "smpte 2084", "hlg"});
  };

  if (inspect(spec.get_string_attribute("oiio:ColorSpace")) ||
      inspect(spec.get_string_attribute("colorspace")) ||
      inspect(spec.get_string_attribute("jpeg:subimages"))) {
    return true;
  }

  for (const auto& param : spec.extra_attribs) {
    const std::string key = param.name().string();
    if (inspect(key) || inspect(param.get_string(0))) {
      return true;
    }
  }
  return false;
}

/// HDR gain-map signatures searched in the first bytes of a file, lower case.
constexpr std::array<std::string_view, 10> kHdrFileSignatures = {"http://ns.adobe.com/hdr-gain-map",
                                                                 "hdrgm:version",
                                                                 "hdrgm:gainmapmin",
                                                                 "gainmap:version",
                                                                 "ultrahdr",
                                                                 "ultra hdr",
                                                                 "iso 21496",
                                                                 "iso:ts:21496",
                                                                 "urn:iso:std:iso:ts:21496",
                                                                 "gain map"};

/// Every signature contains one of these anchors, so a probe without an anchor has no signature
/// and the full list is searched only after an anchor hit.
constexpr std::array<std::string_view, 4>  kHdrSignatureAnchors = {"gain", "hdrgm", "ultra",
                                                                   "21496"};

static_assert(
    [] {
      for (const auto signature : kHdrFileSignatures) {
        bool anchored = false;
        for (const auto anchor : kHdrSignatureAnchors) {
          anchored = anchored || signature.find(anchor) != std::string_view::npos;
        }
        if (!anchored) {
          return false;
        }
      }
      return true;
    }(),
    "every HDR file signature must contain an anchor");

auto FileSignaturesSuggestHdr(const image_path_t& image_path) -> bool {
  std::ifstream ifs(image_path, std::ios::binary);
  if (!ifs.is_open()) {
    return false;
  }

  constexpr size_t kMaxProbeBytes = 4 * 1024 * 1024;
  std::string      text(kMaxProbeBytes, '\0');
  ifs.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<size_t>(std::max<std::streamsize>(0, ifs.gcount())));
  if (text.empty()) {
    return false;
  }

  // ASCII fold without a branch so the loop vectorizes. The signatures are ASCII and no byte
  // above 0x7F folds to ASCII, so this matches per-byte std::tolower for the search below.
  // Per-byte std::tolower plus ten std::string::find calls cost ~17 ms per file, most of the
  // import time per RAW file once LibRaw unpack was gone.
  auto* bytes = reinterpret_cast<unsigned char*>(text.data());
  for (size_t index = 0; index < text.size(); ++index) {
    const unsigned char byte = bytes[index];
    bytes[index] =
        static_cast<unsigned char>(byte | (static_cast<unsigned char>(byte - 'A') < 26 ? 0x20 : 0));
  }

  const std::string_view folded(text);
  const bool             anchored = std::any_of(
      kHdrSignatureAnchors.begin(), kHdrSignatureAnchors.end(),
      [folded](std::string_view anchor) { return folded.find(anchor) != std::string_view::npos; });
  if (!anchored) {
    return false;
  }
  return std::any_of(kHdrFileSignatures.begin(), kHdrFileSignatures.end(),
                     [folded](std::string_view signature) {
                       return folded.find(signature) != std::string_view::npos;
                     });
}
}  // namespace

auto DetectHdrMetadata(const image_path_t& image_path, const Exiv2::Image* exif_image) -> bool {
  if (exif_image) {
    for (const auto& datum : exif_image->xmpData()) {
      const std::string text = ToLowerAscii(datum.key() + " " + datum.toString());
      if (ContainsAny(text, {"ultrahdr", "ultra hdr", "hdrgm", "gainmap", "gain map", "iso 21496",
                             "iso:ts:21496", "rec2100", "rec.2100", "bt2100", "bt.2100", "st2084",
                             "st 2084", "smpte2084", "smpte 2084", "hlg"})) {
        return true;
      }
    }
  }

  try {
    auto input = ImageInput::open(PathToUtf8(image_path));
    if (input && OiioMetadataSuggestsHdr(input->spec())) {
      return true;
    }
  } catch (...) {
  }

  return FileSignaturesSuggestHdr(image_path);
}

}  // namespace alcedo::metadata_detail
