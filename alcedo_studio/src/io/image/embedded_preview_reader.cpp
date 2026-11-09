//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "io/image/embedded_preview_reader.hpp"

#include <libraw/libraw.h>
#include <turbojpeg.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <string>

namespace alcedo {
namespace {

struct TurboJpegCloser {
  void operator()(void* handle) const { tj3Destroy(handle); }
};

auto PathText(const std::filesystem::path& path) -> std::string {
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

auto OpenRaw(LibRaw& processor, const std::filesystem::path& path) -> int {
#ifdef _WIN32
  return processor.open_file(path.wstring().c_str());
#else
  return processor.open_file(path.string().c_str());
#endif
}

/// Applies the LibRaw flip @p flip: 3 is 180 degrees, 5 is 90 degrees counterclockwise, 6 is 90
/// degrees clockwise. Other values leave the image unchanged.
auto Orient(cv::Mat image, int flip) -> cv::Mat {
  cv::Mat oriented;
  switch (flip) {
    case 3:
      cv::rotate(image, oriented, cv::ROTATE_180);
      return oriented;
    case 5:
      cv::rotate(image, oriented, cv::ROTATE_90_COUNTERCLOCKWISE);
      return oriented;
    case 6:
      cv::rotate(image, oriented, cv::ROTATE_90_CLOCKWISE);
      return oriented;
    default:
      return image;
  }
}

/// The TurboJPEG scaling factor, at most 1, with the smallest output whose shorter edge is at least
/// @p min_short_edge. The caller checked that the full-size shorter edge reaches it.
auto ScalingFactorFor(int width, int height, uint32_t min_short_edge) -> tjscalingfactor {
  int                    count   = 0;
  const tjscalingfactor* factors = tj3GetScalingFactors(&count);
  tjscalingfactor        best{1, 1};
  int                    best_short_edge = std::min(width, height);
  for (int i = 0; i < count && factors != nullptr; ++i) {
    const auto factor = factors[i];
    if (factor.num > factor.denom) {
      continue;
    }
    const int short_edge = std::min(TJSCALED(width, factor), TJSCALED(height, factor));
    if (short_edge >= static_cast<int>(min_short_edge) && short_edge < best_short_edge) {
      best            = factor;
      best_short_edge = short_edge;
    }
  }
  return best;
}

auto DecodeJpegPreview(const unsigned char* data, size_t size, uint32_t min_short_edge,
                       const std::filesystem::path& path, EmbeddedPreview* preview) -> bool {
  std::unique_ptr<void, TurboJpegCloser> handle(tj3Init(TJINIT_DECOMPRESS));
  if (!handle) {
    throw std::runtime_error("EmbeddedPreviewReader: TurboJPEG decompressor is not available");
  }
  if (tj3DecompressHeader(handle.get(), data, size) != 0) {
    throw std::runtime_error("EmbeddedPreviewReader: corrupt JPEG preview in " + PathText(path) +
                             ": " + tj3GetErrorStr(handle.get()));
  }
  const int width  = tj3Get(handle.get(), TJPARAM_JPEGWIDTH);
  const int height = tj3Get(handle.get(), TJPARAM_JPEGHEIGHT);
  if (width <= 0 || height <= 0 || std::min(width, height) < static_cast<int>(min_short_edge)) {
    return false;
  }
  const int colorspace = tj3Get(handle.get(), TJPARAM_COLORSPACE);
  if (colorspace == TJCS_CMYK || colorspace == TJCS_YCCK) {
    return false;
  }

  const tjscalingfactor scaling = ScalingFactorFor(width, height, min_short_edge);
  if (tj3SetScalingFactor(handle.get(), scaling) != 0) {
    throw std::runtime_error(std::string("EmbeddedPreviewReader: TurboJPEG scaling failed: ") +
                             tj3GetErrorStr(handle.get()));
  }
  cv::Mat rgb8(TJSCALED(height, scaling), TJSCALED(width, scaling), CV_8UC3);
  if (tj3Decompress8(handle.get(), data, size, rgb8.data, static_cast<int>(rgb8.step[0]),
                     TJPF_RGB) != 0) {
    throw std::runtime_error("EmbeddedPreviewReader: corrupt JPEG preview in " + PathText(path) +
                             ": " + tj3GetErrorStr(handle.get()));
  }
  preview->rgb8          = std::move(rgb8);
  preview->format        = EmbeddedPreviewFormat::kJpeg;
  preview->stored_width  = static_cast<uint32_t>(width);
  preview->stored_height = static_cast<uint32_t>(height);
  return true;
}

auto CopyBitmapPreview(const libraw_thumbnail_t& thumbnail, uint32_t min_short_edge,
                       EmbeddedPreview* preview) -> bool {
  const int width  = thumbnail.twidth;
  const int height = thumbnail.theight;
  if (width <= 0 || height <= 0 || std::min(width, height) < static_cast<int>(min_short_edge)) {
    return false;
  }
  const auto pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (thumbnail.tcolors == 3 && thumbnail.tlength >= pixel_count * 3) {
    preview->rgb8 = cv::Mat(height, width, CV_8UC3, thumbnail.thumb).clone();
  } else if (thumbnail.tcolors == 1 && thumbnail.tlength >= pixel_count) {
    cv::cvtColor(cv::Mat(height, width, CV_8UC1, thumbnail.thumb), preview->rgb8,
                 cv::COLOR_GRAY2RGB);
  } else {
    return false;
  }
  preview->format        = EmbeddedPreviewFormat::kRgb8Bitmap;
  preview->stored_width  = static_cast<uint32_t>(width);
  preview->stored_height = static_cast<uint32_t>(height);
  return true;
}

/// Samples per pixel of a LibRaw thumbnail descriptor: `tmisc` and `thumb_misc` hold the bits per
/// sample in bits 0 to 4 and the samples per pixel in bits 5 to 7.
auto ThumbnailSamples(unsigned misc) -> unsigned { return (misc >> 5) & 7U; }

/// Index into the thumbnail list of @p processor of the preview to read, or -1 for LibRaw's main
/// preview.
///
/// LibRaw's main preview is the largest image of the file, and a one-sample JPEG counts. A DNG that
/// Lightroom merged from several frames (HDR or panorama) stores a full-size JPEG-compressed
/// transparency mask, so LibRaw picks the mask over the 1024-pixel colour preview. When the main
/// preview has one sample and the file lists a colour (three-sample) image, the preview is the
/// largest colour image of the list. A monochrome camera stores only one-sample previews and keeps
/// its main preview.
auto ColourPreviewIndex(LibRaw& processor) -> int {
  const unsigned main_samples =
      ThumbnailSamples(processor.get_internal_data_pointer()->unpacker_data.thumb_misc);
  if (main_samples != 1) {
    return -1;
  }
  const libraw_thumbnail_list_t& list      = processor.imgdata.thumbs_list;
  int                            best      = -1;
  int64_t                        best_area = 0;
  for (int i = 0; i < std::min(list.thumbcount, LIBRAW_THUMBNAIL_MAXCOUNT); ++i) {
    const libraw_thumbnail_item_t& item = list.thumblist[i];
    if (ThumbnailSamples(item.tmisc) != 3 || item.tlength == 0) {
      continue;
    }
    const int64_t area = static_cast<int64_t>(item.twidth) * static_cast<int64_t>(item.theight);
    if (best < 0 || area > best_area) {
      best      = i;
      best_area = area;
    }
  }
  return best;
}

}  // namespace

auto EmbeddedPreviewReader::Read(const std::filesystem::path& raw_path, uint32_t min_short_edge)
    -> std::optional<EmbeddedPreview> {
  // Heap allocation: a LibRaw processor holds several hundred KB of tables.
  auto processor = std::make_unique<LibRaw>();
  int  rc        = OpenRaw(*processor, raw_path);
  if (rc == LIBRAW_FILE_UNSUPPORTED) {
    return std::nullopt;
  }
  if (rc != LIBRAW_SUCCESS) {
    throw std::runtime_error("EmbeddedPreviewReader: cannot open " + PathText(raw_path) + ": " +
                             libraw_strerror(rc));
  }

  const int preview_index = ColourPreviewIndex(*processor);
  rc = preview_index < 0 ? processor->unpack_thumb() : processor->unpack_thumb_ex(preview_index);
  if (rc == LIBRAW_NO_THUMBNAIL || rc == LIBRAW_UNSUPPORTED_THUMBNAIL ||
      rc == LIBRAW_REQUEST_FOR_NONEXISTENT_THUMBNAIL) {
    return std::nullopt;
  }
  if (rc != LIBRAW_SUCCESS) {
    throw std::runtime_error("EmbeddedPreviewReader: cannot read the preview of " +
                             PathText(raw_path) + ": " + libraw_strerror(rc));
  }

  const libraw_thumbnail_t& thumbnail = processor->imgdata.thumbnail;
  if (thumbnail.thumb == nullptr || thumbnail.tlength == 0) {
    return std::nullopt;
  }

  EmbeddedPreview preview;
  bool            decoded = false;
  switch (thumbnail.tformat) {
    case LIBRAW_THUMBNAIL_JPEG:
      decoded = DecodeJpegPreview(reinterpret_cast<const unsigned char*>(thumbnail.thumb),
                                  thumbnail.tlength, min_short_edge, raw_path, &preview);
      break;
    case LIBRAW_THUMBNAIL_BITMAP:
      decoded = CopyBitmapPreview(thumbnail, min_short_edge, &preview);
      break;
    default:
      decoded = false;
      break;
  }
  if (!decoded) {
    return std::nullopt;
  }

  preview.orientation_flip = processor->imgdata.sizes.flip;
  preview.rgb8             = Orient(std::move(preview.rgb8), preview.orientation_flip);
  return preview;
}

}  // namespace alcedo
