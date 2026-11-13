//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/image_analysis_encoder.hpp"

#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <sstream>
#include <string>
#include <thread>

namespace alcedo {
namespace {

OIIO_NAMESPACE_USING

// Converts any channel count (1/3/4) and depth (8U/32F) to a continuous CV_8UC3 RGB
// mat suitable for OIIO JPEG encoding. Mirrors `PrepareForOiioEncoding` in
// thumbnail_disk_cache_service.cpp (anon-namespace there, unreachable here).
cv::Mat PrepareRgb8ForOiio(const cv::Mat& src) {
  if (src.empty()) {
    return {};
  }

  cv::Mat    rgb;
  const int  channels = src.channels();
  if (channels == 4) {
    cv::cvtColor(src, rgb, cv::COLOR_RGBA2RGB);
  } else if (channels == 3) {
    if (src.depth() == CV_32F) {
      rgb = src;
    } else {
      cv::cvtColor(src, rgb, cv::COLOR_BGR2RGB);
    }
  } else if (channels == 1) {
    cv::cvtColor(src, rgb, cv::COLOR_GRAY2RGB);
  } else {
    return {};
  }

  cv::Mat rgb8;
  if (rgb.depth() == CV_8U) {
    rgb8 = rgb;
  } else if (rgb.depth() == CV_32F) {
    rgb.convertTo(rgb8, CV_8UC3, 255.0);
  } else {
    rgb.convertTo(rgb8, CV_8UC3);
  }
  return rgb8.isContinuous() ? rgb8 : rgb8.clone();
}

std::string PathToUtf8(const std::filesystem::path& path) {
  auto u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

// Unique temp file name under `dir`. Combines a steady-clock epoch, a thread-id hash,
// and a process-wide atomic counter so concurrent encodes never collide.
std::filesystem::path MakeUniqueTempPath(const std::filesystem::path& dir) {
  static std::atomic<uint64_t> counter{0};
  const auto                   epoch = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
  std::ostringstream oss;
  oss << "alcedo_ia_" << epoch << "_" << tid << "_" << counter.fetch_add(1) << ".jpg";
  return dir / oss.str();
}

// RAII guard that removes the temp file on destruction (idempotent: a missing file is
// not an error). Guarantees no temp-file leak on success, OIIO failure, or readback
// failure — the Phase 5d review focus.
class TempFileGuard {
 public:
  explicit TempFileGuard(std::filesystem::path path) : path_(std::move(path)) {}
  ~TempFileGuard() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  TempFileGuard(const TempFileGuard&)            = delete;
  TempFileGuard& operator=(const TempFileGuard&) = delete;
  const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

auto EncodeThumbnailForRemoteAnalysis(const ThumbnailGuard&        guard,
                                      int                         quality,
                                      uint32_t                    max_edge_hint,
                                      const std::filesystem::path& temp_dir,
                                      std::string*                error) -> EncodedRendition {
  EncodedRendition out;
  out.rendition_kind = "thumbnail";
  out.quality        = quality;
  (void)max_edge_hint;  // recorded only in format_hint via the actual max_edge

  if (!guard.thumbnail_buffer_) {
    out.error = "thumbnail buffer is null";
    if (error) *error = out.error;
    return out;
  }

  auto* buffer = guard.thumbnail_buffer_.get();
  try {
    if (!buffer->cpu_data_valid_ && buffer->gpu_data_valid_) {
      buffer->SyncToCPU();
    }
  } catch (const std::exception& e) {
    out.error = std::string("thumbnail CPU sync failed: ") + e.what();
    if (error) *error = out.error;
    return out;
  } catch (...) {
    out.error = "thumbnail CPU sync failed";
    if (error) *error = out.error;
    return out;
  }
  if (!buffer->cpu_data_valid_) {
    out.error = "thumbnail has no valid CPU data";
    if (error) *error = out.error;
    return out;
  }

  cv::Mat src;
  try {
    src = buffer->GetCPUData();
  } catch (const std::exception& e) {
    out.error = std::string("thumbnail CPU read failed: ") + e.what();
    if (error) *error = out.error;
    return out;
  }

  cv::Mat rgb8 = PrepareRgb8ForOiio(src);
  if (rgb8.empty() || rgb8.type() != CV_8UC3) {
    out.error = "thumbnail could not be converted to RGB8 for encoding";
    if (error) *error = out.error;
    return out;
  }

  out.width    = static_cast<uint32_t>(rgb8.cols);
  out.height   = static_cast<uint32_t>(rgb8.rows);
  out.max_edge = std::max(out.width, out.height);
  out.mime_type  = "image/jpeg";
  out.format_hint = "image/jpeg;max_edge=" + std::to_string(out.max_edge);

  std::error_code mkec;
  std::filesystem::create_directories(temp_dir, mkec);

  const auto  temp_path = MakeUniqueTempPath(temp_dir);
  TempFileGuard file_guard(temp_path);
  const auto   dst = PathToUtf8(temp_path);

  auto oiio_out = ImageOutput::create(dst);
  if (!oiio_out) {
    out.error = "OpenImageIO could not create JPEG encoder";
    if (error) *error = out.error;
    return out;
  }

  ImageSpec spec(rgb8.cols, rgb8.rows, 3, TypeDesc::UINT8);
  spec.attribute("CompressionQuality", quality);
  if (!oiio_out->open(dst, spec)) {
    out.error = "OpenImageIO could not open output for JPEG encoding";
    if (error) *error = out.error;
    return out;
  }
  const bool wrote = oiio_out->write_image(TypeDesc::UINT8, rgb8.data);
  oiio_out->close();
  if (!wrote) {
    out.error = "OpenImageIO JPEG write failed";
    if (error) *error = out.error;
    return out;
  }

  std::ifstream input(temp_path, std::ios::binary);
  if (!input.is_open()) {
    out.error = "could not read back encoded JPEG";
    if (error) *error = out.error;
    return out;
  }
  out.bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  // TempFileGuard removes temp_path on return.

  if (out.bytes.empty()) {
    out.error = "encoded JPEG is empty";
    if (error) *error = out.error;
    return out;
  }
  out.ok = true;
  if (error) {
    error->clear();
  }
  return out;
}

}  // namespace alcedo
