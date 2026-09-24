//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <exiv2/exif.hpp>
#include <exiv2/image.hpp>
#include <future>
#include <memory>
#include <opencv2/imgcodecs.hpp>

#include "concurrency/thread_pool.hpp"
#include "image/image.hpp"
#include "type/type.hpp"
#include "utils/queue/queue.hpp"

#define MAX_REQUEST_SIZE 64u
namespace alcedo {

enum class DecodeType { SLEEVE_LOADING, THUMB, RAW, REGULAR };

class DecoderScheduler {
 private:
  ThreadPool                   file_read_thread_pool_;
  ThreadPool                   thread_pool_;
  std::shared_ptr<BufferQueue> decoded_buffer_;

 public:
  explicit DecoderScheduler(size_t thread_count, std::shared_ptr<BufferQueue> decoded_buffer);

  void ScheduleDecode(image_id_t id, image_path_t image_path,
                      std::shared_ptr<std::promise<image_id_t>> decode_promise);

  void ScheduleDecode(std::shared_ptr<Image> source_img, DecodeType decode_type,
                      std::shared_ptr<std::promise<image_id_t>> decode_promise);
};

};  // namespace alcedo