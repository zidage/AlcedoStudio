//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/input/raster_input_loader.hpp"

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imageio.h>
#include <lcms2.h>
#include <turbojpeg.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/input/detail/prepared_input_finish.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "image/icc_profile_reader.hpp"
#include "image/image_content_class.hpp"
#include "image/raster_container_reader.hpp"
#include "utils/hash/sha256.hpp"

namespace alcedo {
namespace {

/// Decoded pixels before orientation: RGBA at native depth, alpha at the maximum code value.
struct DecodedPlane {
  HostImagePlane         plane;
  /// Full-resolution size before any DCT scaling.
  Extent2D               full_extent{};
  std::uint8_t           downsample_passes = 0;
  /// EXIF orientation 1..8.
  int                    orientation       = 1;
  /// ICC profile embedded in the file, if any.
  std::vector<std::byte> icc_profile;
};

auto AllocatePlane(std::uint32_t width, std::uint32_t height, HostPixelFormat format)
    -> HostImagePlane {
  HostImagePlane plane;
  plane.extent       = Extent2D{width, height};
  plane.format       = format;
  plane.stride_bytes = width * HostPixelFormatBytesPerPixel(format);
  plane.bytes        = std::shared_ptr<const std::byte>(new std::byte[plane.ByteCount()],
                                                        std::default_delete<std::byte[]>());
  return plane;
}

auto MutableBytes(HostImagePlane& plane) -> std::byte* {
  return const_cast<std::byte*>(plane.bytes.get());
}

// -------------------------------------------------------------------------------------------
// JPEG
// -------------------------------------------------------------------------------------------

struct TurboJpegCloser {
  void operator()(void* handle) const { tj3Destroy(handle); }
};

auto DecodeJpeg(std::span<const std::byte> bytes, DecodeRes decode_res) -> DecodedPlane {
  const auto container = ReadJpegContainer(bytes);
  if (container.component_count_ == 4) {
    throw std::runtime_error("RasterInputLoader: CMYK JPEG is not supported");
  }
  std::unique_ptr<void, TurboJpegCloser> handle(tj3Init(TJINIT_DECOMPRESS));
  if (!handle) {
    throw std::runtime_error("RasterInputLoader: TurboJPEG initialization failed");
  }
  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  if (tj3DecompressHeader(handle.get(), data, bytes.size()) != 0) {
    throw std::runtime_error(std::string("RasterInputLoader: JPEG header: ") +
                             tj3GetErrorStr(handle.get()));
  }
  const int colorspace = tj3Get(handle.get(), TJPARAM_COLORSPACE);
  if (colorspace == TJCS_CMYK || colorspace == TJCS_YCCK) {
    throw std::runtime_error("RasterInputLoader: CMYK JPEG is not supported");
  }
  const int             width  = tj3Get(handle.get(), TJPARAM_JPEGWIDTH);
  const int             height = tj3Get(handle.get(), TJPARAM_JPEGHEIGHT);
  const auto            passes = DecodeResToDownsamplePasses(decode_res);
  const tjscalingfactor scaling{1, 1 << passes};
  if (tj3SetScalingFactor(handle.get(), scaling) != 0) {
    throw std::runtime_error(std::string("RasterInputLoader: JPEG scaling: ") +
                             tj3GetErrorStr(handle.get()));
  }
  const int    scaled_width  = TJSCALED(width, scaling);
  const int    scaled_height = TJSCALED(height, scaling);

  DecodedPlane decoded;
  decoded.plane = AllocatePlane(static_cast<std::uint32_t>(scaled_width),
                                static_cast<std::uint32_t>(scaled_height), HostPixelFormat::U8Rgba);
  decoded.full_extent =
      Extent2D{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
  decoded.downsample_passes = passes;
  // TJPF_RGBA writes 0xFF alpha; a corrupt or truncated stream (also a warning) fails import.
  if (tj3Decompress8(handle.get(), data, bytes.size(),
                     reinterpret_cast<unsigned char*>(MutableBytes(decoded.plane)),
                     static_cast<int>(decoded.plane.stride_bytes), TJPF_RGBA) != 0) {
    throw std::runtime_error(std::string("RasterInputLoader: JPEG decode: ") +
                             tj3GetErrorStr(handle.get()));
  }
  const auto exif     = ReadExifColorTags(container.exif_tiff_);
  decoded.orientation = exif.orientation_.value_or(1);
  decoded.icc_profile = container.icc_profile_;
  return decoded;
}

// -------------------------------------------------------------------------------------------
// PNG, TIFF, OpenEXR (OpenImageIO)
// -------------------------------------------------------------------------------------------

auto FindChannel(const OIIO::ImageSpec& spec, const char* name) -> int {
  for (int i = 0; i < spec.nchannels; ++i) {
    if (spec.channelnames[static_cast<std::size_t>(i)] == name) {
      return i;
    }
  }
  return -1;
}

template <typename T>
void CopyToRgba(const std::vector<T>& source, int source_channels, const std::array<int, 3>& rgb,
                T alpha, HostImagePlane& plane) {
  auto*             out    = reinterpret_cast<T*>(MutableBytes(plane));
  const std::size_t pixels = static_cast<std::size_t>(plane.extent.width) * plane.extent.height;
  for (std::size_t i = 0; i < pixels; ++i) {
    const T* in    = source.data() + i * static_cast<std::size_t>(source_channels);
    out[i * 4]     = in[rgb[0]];
    out[i * 4 + 1] = in[rgb[1]];
    out[i * 4 + 2] = in[rgb[2]];
    out[i * 4 + 3] = alpha;
  }
}

auto DecodeWithOpenImageIo(std::span<const std::byte> bytes, RasterFileKind kind) -> DecodedPlane {
  DecodedPlane decoded;
  const char*  name = "raster.png";
  if (kind == RasterFileKind::Png) {
    const auto container = ReadPngContainer(bytes);
    decoded.orientation  = ReadExifColorTags(container.exif_tiff_).orientation_.value_or(1);
    decoded.icc_profile  = container.icc_profile_;
  } else if (kind == RasterFileKind::Tiff) {
    name                 = "raster.tif";
    const auto container = ReadTiffContainer(bytes);
    if (container.photometric_ == 5) {
      throw std::runtime_error("RasterInputLoader: CMYK TIFF is not supported");
    }
    decoded.orientation = container.orientation_;
    decoded.icc_profile = container.icc_profile_;
  } else {
    name = "raster.exr";
  }

  OIIO::Filesystem::IOMemReader reader(bytes.data(), bytes.size());
  // Keep stored color values: alpha is discarded (decision D6), so PNG and TIFF color must not
  // be multiplied by alpha. OpenEXR stores associated color and is read as stored.
  // With a config spec, OpenImageIO takes the proxy from its "oiio:ioproxy" attribute.
  OIIO::ImageSpec               config;
  config.attribute("oiio:UnassociatedAlpha", 1);
  OIIO::Filesystem::IOProxy* proxy = &reader;
  config.attribute("oiio:ioproxy", OIIO::TypeDesc::PTR, &proxy);
  auto input = OIIO::ImageInput::open(name, &config, &reader);
  if (!input) {
    throw std::runtime_error("RasterInputLoader: cannot open the image: " + OIIO::geterror());
  }
  const auto& spec = input->spec();
  if (spec.width <= 0 || spec.height <= 0 || spec.nchannels <= 0) {
    throw std::runtime_error("RasterInputLoader: the image has no pixels");
  }
  std::array<int, 3> rgb{};
  if (kind == RasterFileKind::OpenExr) {
    rgb = {FindChannel(spec, "R"), FindChannel(spec, "G"), FindChannel(spec, "B")};
    if (rgb[0] < 0 || rgb[1] < 0 || rgb[2] < 0) {
      const int y = FindChannel(spec, "Y");
      if (y < 0) {
        throw std::runtime_error("RasterInputLoader: OpenEXR has neither R, G, B nor Y");
      }
      rgb = {y, y, y};
    }
  } else if (spec.nchannels >= 3) {
    rgb = {0, 1, 2};
  } else {
    rgb = {0, 0, 0};  // Gray, with or without alpha.
  }

  const auto width    = static_cast<std::uint32_t>(spec.width);
  const auto height   = static_cast<std::uint32_t>(spec.height);
  decoded.full_extent = Extent2D{width, height};
  const auto count =
      static_cast<std::size_t>(width) * height * static_cast<std::size_t>(spec.nchannels);
  const bool floating = spec.format.basetype == OIIO::TypeDesc::FLOAT ||
                        spec.format.basetype == OIIO::TypeDesc::HALF ||
                        spec.format.basetype == OIIO::TypeDesc::DOUBLE ||
                        kind == RasterFileKind::OpenExr;
  if (floating) {
    std::vector<float> data(count);
    if (!input->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, data.data())) {
      throw std::runtime_error("RasterInputLoader: pixel decode failed: " + input->geterror());
    }
    decoded.plane = AllocatePlane(width, height, HostPixelFormat::F32Rgba);
    CopyToRgba<float>(data, spec.nchannels, rgb, 1.0f, decoded.plane);
  } else if (spec.format.size() <= 1) {
    std::vector<std::uint8_t> data(count);
    if (!input->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::UINT8, data.data())) {
      throw std::runtime_error("RasterInputLoader: pixel decode failed: " + input->geterror());
    }
    decoded.plane = AllocatePlane(width, height, HostPixelFormat::U8Rgba);
    CopyToRgba<std::uint8_t>(data, spec.nchannels, rgb, 255, decoded.plane);
  } else {
    std::vector<std::uint16_t> data(count);
    if (!input->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::UINT16, data.data())) {
      throw std::runtime_error("RasterInputLoader: pixel decode failed: " + input->geterror());
    }
    decoded.plane = AllocatePlane(width, height, HostPixelFormat::U16Rgba);
    CopyToRgba<std::uint16_t>(data, spec.nchannels, rgb, 65535, decoded.plane);
  }
  input->close();
  return decoded;
}

// -------------------------------------------------------------------------------------------
// LUT-based ICC conversion
// -------------------------------------------------------------------------------------------

struct ProfileCloser {
  void operator()(void* profile) const { cmsCloseProfile(profile); }
};

struct TransformCloser {
  void operator()(void* transform) const { cmsDeleteTransform(transform); }
};

/// Transforms per profile hash, kept for the process: a library holds few LUT profiles.
auto LutTransform(std::span<const std::byte> profile_bytes, const std::string& sha256) -> void* {
  static std::mutex                                                    mutex;
  static std::map<std::string, std::unique_ptr<void, TransformCloser>> cache;
  std::lock_guard<std::mutex>                                          lock(mutex);
  if (const auto it = cache.find(sha256); it != cache.end()) {
    return it->second.get();
  }
  std::unique_ptr<void, ProfileCloser> source(cmsOpenProfileFromMem(
      profile_bytes.data(), static_cast<cmsUInt32Number>(profile_bytes.size())));
  if (!source) {
    throw std::runtime_error("RasterInputLoader: the LUT ICC profile cannot be parsed");
  }
  const cmsCIExyY       white{0.3127, 0.3290, 1.0};
  const cmsCIExyYTRIPLE primaries{{0.708, 0.292, 1.0}, {0.170, 0.797, 1.0}, {0.131, 0.046, 1.0}};
  std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)> linear(cmsBuildGamma(nullptr, 1.0),
                                                                    &cmsFreeToneCurve);
  cmsToneCurve*                        curves[3] = {linear.get(), linear.get(), linear.get()};
  std::unique_ptr<void, ProfileCloser> target(cmsCreateRGBProfile(&white, &primaries, curves));
  if (!target) {
    throw std::runtime_error("RasterInputLoader: cannot build the linear Rec.2020 profile");
  }
  std::unique_ptr<void, TransformCloser> transform(
      cmsCreateTransform(source.get(), TYPE_RGB_FLT, target.get(), TYPE_RGB_FLT,
                         INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE));
  if (!transform) {
    throw std::runtime_error("RasterInputLoader: cannot build the LUT ICC transform");
  }
  void* raw = transform.get();
  cache.emplace(sha256, std::move(transform));
  return raw;
}

/// Convert @p plane through the LUT profile to linear Rec.2020 F32 RGBA.
auto ConvertWithLutProfile(const HostImagePlane& plane, std::span<const std::byte> profile_bytes,
                           const std::string& sha256) -> HostImagePlane {
  void*              transform = LutTransform(profile_bytes, sha256);
  const std::size_t  pixels    = static_cast<std::size_t>(plane.extent.width) * plane.extent.height;
  std::vector<float> rgb(pixels * 3);
  const auto*        bytes = plane.bytes.get();
  for (std::size_t i = 0; i < pixels; ++i) {
    for (std::size_t c = 0; c < 3; ++c) {
      float value = 0.0f;
      if (plane.format == HostPixelFormat::U8Rgba) {
        value = std::to_integer<std::uint8_t>(bytes[i * 4 + c]) / 255.0f;
      } else if (plane.format == HostPixelFormat::U16Rgba) {
        std::uint16_t code = 0;
        std::memcpy(&code, bytes + (i * 4 + c) * 2, 2);
        value = code / 65535.0f;
      } else {
        std::memcpy(&value, bytes + (i * 4 + c) * 4, 4);
      }
      rgb[i * 3 + c] = value;
    }
  }
  std::vector<float> converted(pixels * 3);
  cmsDoTransform(transform, rgb.data(), converted.data(), static_cast<cmsUInt32Number>(pixels));
  auto  out = AllocatePlane(plane.extent.width, plane.extent.height, HostPixelFormat::F32Rgba);
  auto* f   = reinterpret_cast<float*>(MutableBytes(out));
  for (std::size_t i = 0; i < pixels; ++i) {
    f[i * 4]     = converted[i * 3];
    f[i * 4 + 1] = converted[i * 3 + 1];
    f[i * 4 + 2] = converted[i * 3 + 2];
    f[i * 4 + 3] = 1.0f;
  }
  return out;
}

// -------------------------------------------------------------------------------------------
// Orientation
// -------------------------------------------------------------------------------------------

void MirrorHorizontally(HostImagePlane& plane) {
  const std::size_t      bpp   = HostPixelFormatBytesPerPixel(plane.format);
  auto*                  bytes = MutableBytes(plane);
  std::vector<std::byte> pixel(bpp);
  for (std::uint32_t y = 0; y < plane.extent.height; ++y) {
    auto* row = bytes + static_cast<std::size_t>(y) * plane.stride_bytes;
    for (std::uint32_t x = 0; x < plane.extent.width / 2; ++x) {
      auto* left  = row + x * bpp;
      auto* right = row + (plane.extent.width - 1 - x) * bpp;
      std::memcpy(pixel.data(), left, bpp);
      std::memcpy(left, right, bpp);
      std::memcpy(right, pixel.data(), bpp);
    }
  }
}

/// RawSensorGeometry flip of EXIF orientation @p orientation after any mirror was applied.
/// 3: 180 degrees, 6: 90 clockwise, 5: 90 counterclockwise (LibRaw flip codes).
auto FlipForOrientation(int orientation, bool& mirror) -> int {
  mirror = false;
  switch (orientation) {
    case 2:
      mirror = true;
      return 0;
    case 3:
      return 3;
    case 4:
      mirror = true;
      return 3;
    case 5:
      mirror = true;
      return 5;
    case 6:
      return 6;
    case 7:
      mirror = true;
      return 6;
    case 8:
      return 5;
    default:
      return 0;
  }
}

auto ToPrepared(HostImagePlane plane, Extent2D full_extent, std::uint8_t passes, int flip,
                std::uint64_t encoded_hash, std::uint64_t encoded_bytes) -> PreparedRawInput {
  PreparedRawInput input;
  input.pixels                  = std::move(plane);
  input.host_extent             = input.pixels.extent;
  input.downsample_passes       = passes;
  input.sensor.raw_width        = static_cast<std::int32_t>(full_extent.width);
  input.sensor.raw_height       = static_cast<std::int32_t>(full_extent.height);
  input.sensor.width            = input.sensor.raw_width;
  input.sensor.height           = input.sensor.raw_height;
  input.sensor.orientation_flip = flip;
  return input_detail::FinishRasterPrepared(std::move(input), encoded_hash, encoded_bytes);
}

}  // namespace

auto RasterInputLoader::LoadEncoded(std::span<const std::byte> encoded, DecodeRes decode_res,
                                    RasterFileKind kind) -> PreparedRawInput {
  DecodedPlane decoded;
  try {
    decoded = kind == RasterFileKind::Jpeg ? DecodeJpeg(encoded, decode_res)
                                           : DecodeWithOpenImageIo(encoded, kind);
  } catch (const RasterContainerError& error) {
    throw std::runtime_error(std::string("RasterInputLoader: malformed file: ") + error.what());
  }

  bool        lut_converted = false;
  std::string icc_sha256;
  if (!decoded.icc_profile.empty() &&
      ReadIccProfile(decoded.icc_profile).layout_ == IccProfileLayout::RgbLut) {
    icc_sha256    = ComputeSha256Hex(decoded.icc_profile);
    decoded.plane = ConvertWithLutProfile(decoded.plane, decoded.icc_profile, icc_sha256);
    lut_converted = true;
  }

  bool      mirror = false;
  const int flip   = FlipForOrientation(decoded.orientation, mirror);
  if (mirror) {
    MirrorHorizontally(decoded.plane);
  }
  auto input = ToPrepared(std::move(decoded.plane), decoded.full_extent, decoded.downsample_passes,
                          flip, HashContentBytes(encoded), encoded.size());
  input.raster_lut_icc_converted = lut_converted;
  input.raster_icc_sha256        = std::move(icc_sha256);
  return input;
}

auto RasterInputLoader::FromHostPlane(HostImagePlane plane, int orientation_flip)
    -> PreparedRawInput {
  if (plane.format == HostPixelFormat::U16Cfa || plane.extent.Empty() || !plane.bytes ||
      plane.stride_bytes != plane.extent.width * HostPixelFormatBytesPerPixel(plane.format)) {
    throw std::invalid_argument("RasterInputLoader::FromHostPlane: tightly packed RGBA required");
  }
  const auto hash  = HashContentBytes(plane.Span());
  const auto bytes = plane.ByteCount();
  const auto full  = plane.extent;
  return ToPrepared(std::move(plane), full, 0, orientation_flip, hash, bytes);
}

auto LoadEncodedImage(std::span<const std::byte> encoded, DecodeRes decode_res)
    -> PreparedRawInput {
  if (const auto kind = RasterFileKindFor(ClassifyImageContent(encoded))) {
    return RasterInputLoader::LoadEncoded(encoded, decode_res, *kind);
  }
  return RawInputLoader::LoadEncoded(encoded, decode_res);
}

}  // namespace alcedo
