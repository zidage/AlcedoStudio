//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <vector>

#include "decoders/decoder_scheduler.hpp"
#include "image/image.hpp"
#include "type/type.hpp"
#include "utils/queue/queue.hpp"

#ifdef LoadImage
#undef LoadImage
#endif

namespace alcedo {

class ImageLoader {
 private:
  // Image decoding part
  std::shared_ptr<BufferQueue>                           buffer_decoded_;
  uint32_t                                               buffer_size_;
  size_t                                                 use_thread_;
  image_id_t                                             start_id_;
  image_id_t                                             next_id_;
  DecoderScheduler                                       decoder_scheduler_;

  std::vector<std::shared_ptr<std::promise<image_id_t>>> promises_;
  std::vector<std::future<image_id_t>>                   futures_;

 public:
  explicit ImageLoader(uint32_t buffer_size, size_t use_thread, image_id_t start_id);

  void StartLoading(std::vector<image_path_t> images, DecodeType decode_type);
  void StartLoading(std::shared_ptr<Image> source_img, DecodeType decode_type);
  auto LoadImage() -> std::shared_ptr<Image>;
};

class ByteBufferLoader {
 public:
  // TODO: This function has been deprecated, use LoadByteBufferFromImage instead
  /**
   * @brief Load image data from disk into a byte buffer, used only from ImagePool
   *
   * @param img
   * @return std::shared_ptr<std::vector<uint8_t>>
   */
  static auto LoadFromImage(std::shared_ptr<Image> img) -> std::shared_ptr<std::vector<uint8_t>>;

  static auto LoadByteBufferFromImage(std::shared_ptr<Image> img) -> std::vector<uint8_t>;

  static auto LoadByteBufferFromPath(const image_path_t& path) -> std::vector<uint8_t>;
};
};  // namespace alcedo