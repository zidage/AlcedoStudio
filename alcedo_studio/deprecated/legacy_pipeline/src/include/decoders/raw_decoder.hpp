//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <libraw/libraw.h>

#include <memory>

#include "data_decoder.hpp"
#include "type/type.hpp"

namespace alcedo {
enum class OutputColorSpace : int {
  RAW         = 0,
  sRGB        = 1,
  AdobeRGB    = 2,
  Wide        = 3,
  ProPhotoRGB = 4,
  XYZ         = 5,
  ACES        = 6,
  DCIP3       = 7,
  REC2020     = 8
};

class RawDecoder : public DataDecoder {
 public:
  RawDecoder() = default;
  void Decode(std::vector<char> buffer, std::filesystem::path file_path,
              std::shared_ptr<BufferQueue> result, image_id_t id,
              std::shared_ptr<std::promise<image_id_t>> promise);

  void Decode(std::vector<char> buffer, std::shared_ptr<Image> source_img,
              std::shared_ptr<BufferQueue>              result,
              std::shared_ptr<std::promise<image_id_t>> promise);

  void Decode(std::vector<char>&& buffer, std::shared_ptr<Image> source_img);
};

};  // namespace alcedo
