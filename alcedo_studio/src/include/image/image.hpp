//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <exiv2/exif.hpp>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <json.hpp>
#include <memory>
#include <opencv2/opencv.hpp>
#include <ostream>
#include <string>
#include <type/type.hpp>

#include "decoders/processor/raw_color_context.hpp"
#include "image/image_buffer.hpp"
#include "image/metadata.hpp"

namespace alcedo {
enum class ImageType { DEFAULT, JPEG, PNG, TIFF, ARW, CR2, CR3, NEF, DNG };
enum class ThumbState : uint8_t { NOT_PRESENT = 0, PENDING, READY, FAILED };
enum class ImageSyncState : uint8_t { SYNCED, UNSYNCED, MODIFIED, DELETED };

/**
 * @brief Represent a tracked image file
 *
 */
class Image {
  // FIXME: Image should not hold any image data, only metadata
  // FIXME: Remove all has_xxx_ atomic flags, use sync_state_ only
 public:
  image_id_t                  image_id_;
  image_path_t                image_path_;
  file_name_t                 image_name_;

  Exiv2::Image::UniquePtr     exif_data_;
  nlohmann::json              exif_json_;
  ExifDisplayMetaData         exif_display_;
  RawRuntimeColorContext       raw_color_context_;
  std::atomic<bool>           has_raw_color_context_{false};

  ImageBuffer                 image_data_;
  ImageBuffer                 thumbnail_;
  ImageType                   image_type_ = ImageType::DEFAULT;

  std::atomic<bool>           has_thumbnail_;

  std::atomic<ThumbState>     thumb_state_ = ThumbState::NOT_PRESENT;

  std::atomic<ImageSyncState> sync_state_  = ImageSyncState::SYNCED;

  p_hash_t                    checksum_;

  std::atomic<bool>           has_full_img_;
  std::atomic<bool>           has_thumb_;
  std::atomic<bool>           has_exif_;
  std::atomic<bool>           has_exif_json_;
  std::atomic<bool>           has_exif_display_;

  std::atomic<bool>           thumb_pinned_ = false;
  std::atomic<bool>           full_pinned_  = false;

  explicit Image()                          = default;
  explicit Image(image_id_t image_id);
  explicit Image(image_id_t image_id, image_path_t image_path, ImageType image_type);
  explicit Image(image_id_t image_id, image_path_t image_path, file_name_t image_name,
                 ImageType image_type);
  explicit Image(image_path_t image_path, ImageType image_type);
  explicit Image(Image&& other);

  friend std::wostream& operator<<(std::wostream& os, const Image& img);

  void                  LoadOriginalData(ImageBuffer&& load_image);
  void                  LoadThumbnailData(ImageBuffer&& thumbnail);

  auto                  GetImageData() -> cv::Mat&;
  auto                  GetThumbnailData() -> cv::Mat&;
  auto                  GetThumbnailBuffer() -> ImageBuffer&;

  void                  SetId(image_id_t image_id);
  void                  SetExifDisplayMetaData(ExifDisplayMetaData&& exif_display);
  void                  SetHdrDisplayMetadata(bool is_hdr);
  void                  SetRawColorContext(RawRuntimeColorContext&& ctx);
  auto                  GetRawColorContext() const -> const RawRuntimeColorContext&;
  auto                  HasRawColorContext() const -> bool;

  void                  ClearData();
  void                  ClearThumbnail();
  void                  ComputeChecksum();
  auto                  ExifToJson() -> std::string;
  void                  JsonToExif(std::string json_str);

  void                  MarkSyncState(ImageSyncState state);
  auto                  GetSyncState() -> ImageSyncState;
};
};  // namespace alcedo
