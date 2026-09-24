//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// TODO: Delete all files expect the ones under "processor" 

#include "decoders/decoder_scheduler.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "decoders/image_decoder.hpp"
#include "decoders/metadata_decoder.hpp"
#include "decoders/thumbnail_decoder.hpp"
#include "image/image.hpp"
#include "type/type.hpp"
#include "utils/queue/queue.hpp"

namespace alcedo {
/**
 * @brief Construct a new Image Decoder::Image Decoder object
 *
 * @param thread_count
 * @param total_request
 */
DecoderScheduler::DecoderScheduler(size_t thread_count, std::shared_ptr<BufferQueue> decoded_buffer)
    : file_read_thread_pool_(thread_count),
      thread_pool_(thread_count),
      decoded_buffer_(decoded_buffer) {}

/**
 * @brief Schedule a decode task for initialize image data. The decode type can only be
 * SLEEVE_LOADING, therefore decode_type field is omitted.
 *
 * @param image_path the path of the file to be decoded
 * @param decode_promise the corresponding promise to be collected
 */
void DecoderScheduler::ScheduleDecode(image_id_t id, image_path_t image_path,
                                      std::shared_ptr<std::promise<image_id_t>> decode_promise) {
  file_read_thread_pool_.Submit([id, image_path, decode_promise, this] {
    // Open file as an ifstream

    std::ifstream file(image_path, std::ios::binary | std::ios::ate);

    if (!file.is_open()) {
      // Check whether file openned successfully
      decode_promise->set_exception(
          std::make_exception_ptr(std::runtime_error("File not exists or no read permission.")));
      return;
    }

    // Assign a decoder for the task
    std::shared_ptr<LoadingDecoder> decoder  = std::make_shared<MetadataDecoder>();

    // Read file into memory
    std::streamsize                 fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(fileSize);
    if (!file.read(buffer.data(), fileSize)) {
      decode_promise->set_exception(
          std::make_exception_ptr(std::runtime_error("File not exists or no read permission.")));
      return;
    }
    file.close();

    // Submit a new decode request
    auto&                 decoded_buffer = decoded_buffer_;
    std::filesystem::path file_path(image_path);

    // auto                  task = std::make_shared<std::packaged_task<void()>>(
    //     [decoder, buffer = std::move(buffer), file_path, decoded_buffer, id, decode_promise]()
    //     mutable {
    //       decoder->Decode(std::move(buffer), file_path, decoded_buffer, id, decode_promise);
    //     });

    thread_pool_.Submit([decoder, buffer = std::move(buffer), file_path, decoded_buffer, id,
                         decode_promise]() mutable {
      decoder->Decode(std::move(buffer), file_path, decoded_buffer, id, decode_promise);
    });
  });
}

/**
 * @brief Schedule a decode task for loading image data into an Image object
 *
 * @param source_img
 * @param decode_promise
 */
void DecoderScheduler::ScheduleDecode(std::shared_ptr<Image> source_img, DecodeType decode_type,
                                      std::shared_ptr<std::promise<image_id_t>> decode_promise) {
  // Open file as an ifstream

  std::ifstream file(source_img->image_path_, std::ios::binary | std::ios::ate);

  if (!file.is_open()) {
    // Check file status
    decode_promise->set_exception(
        std::make_exception_ptr(std::runtime_error("File not exists or no read permission.")));
    return;
  }

  // Assign a decoder for the task
  std::shared_ptr<DataDecoder> decoder;

  // Assign a decoder according to the decode type
  switch (decode_type) {
    case DecodeType::THUMB:
      decoder = std::make_shared<ThumbnailDecoder>();
      break;
    case DecodeType::REGULAR:
      // FIXME: Add RegularDecoder
      decoder = std::make_shared<ThumbnailDecoder>();
      break;
    default:
      throw std::runtime_error("Incompatible decode type.");
  }

  // Read file into memory
  std::streamsize fileSize = file.tellg();
  file.seekg(0, std::ios::beg);
  std::vector<char> buffer(fileSize);
  if (!file.read(buffer.data(), fileSize)) {
    decode_promise->set_exception(
        std::make_exception_ptr(std::runtime_error("File not exists or no read permission.")));
    return;
  }
  file.close();

  // Submit a new decode request
  auto                  decoded_buffer = decoded_buffer_;
  std::filesystem::path file_path(source_img->image_path_);

  thread_pool_.Submit(
      [decoder, buffer = std::move(buffer), decoded_buffer, source_img, decode_promise]() mutable {
        decoder->Decode(std::move(buffer), source_img, decoded_buffer, decode_promise);
      });
}
};  // namespace alcedo
