//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/nn/utnet2_tile_io.hpp"

#include <algorithm>
#include <atomic>
#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <sstream>
#include <stdexcept>

#include "decoders/processor/nn/utnet2_specs.hpp"

namespace alcedo::nn {

auto UtNet2TileText(const PackedTileRect& tile) -> std::string {
  std::ostringstream text;
  text << "UtNet2 tile at (" << tile.x << ", " << tile.y << ") of " << tile.width << " x "
       << tile.height << " packed px";
  return text.str();
}

void CheckUtNet2TileRun(std::span<const float> input, const PackedTileRect& tile,
                        const TileOutputCrop& crop) {
  constexpr int kAlignment = UtNet2Spec::kSizeAlignment;
  if (tile.width <= 0 || tile.height <= 0 || tile.width % kAlignment != 0 ||
      tile.height % kAlignment != 0) {
    throw std::runtime_error(UtNet2TileText(tile) + ": the height and the width must be positive " +
                             "multiples of " + std::to_string(kAlignment));
  }
  const auto input_count =
      UtNet2Spec::kInputChannels * static_cast<std::size_t>(tile.width) * tile.height;
  if (input.size() != input_count) {
    throw std::runtime_error(UtNet2TileText(tile) + ": the input has " +
                             std::to_string(input.size()) + " values, expected " +
                             std::to_string(input_count));
  }
  constexpr int kUpscale = UtNet2Spec::kOutputUpscale;
  const auto&   target   = crop.destination;
  const bool    in_tile  = crop.source_x >= 0 && crop.source_y >= 0 && crop.width > 0 &&
                       crop.height > 0 && crop.source_x + crop.width <= kUpscale * tile.width &&
                       crop.source_y + crop.height <= kUpscale * tile.height;
  const bool in_target =
      crop.destination_x >= 0 && crop.destination_y >= 0 &&
      crop.destination_x + crop.width <= target.width &&
      crop.destination_y + crop.height <= target.height &&
      target.data.size() ==
          UtNet2Spec::kOutputChannels * static_cast<std::size_t>(target.width) * target.height;
  if (!in_tile || !in_target) {
    std::ostringstream text;
    text << UtNet2TileText(tile) << ": the output rectangle (" << crop.source_x << ", "
         << crop.source_y << ") of " << crop.width << " x " << crop.height << " to ("
         << crop.destination_x << ", " << crop.destination_y << ") of an image of " << target.width
         << " x " << target.height << " is outside the tile output or the image";
    throw std::runtime_error(text.str());
  }
}

void ConvertUtNet2TileInput(std::span<const float> input, const PackedTileRect& tile,
                            void* float16_destination) {
  const int     rows = UtNet2Spec::kInputChannels * tile.height;
  const cv::Mat input_float(rows, tile.width, CV_32F, const_cast<float*>(input.data()));
  cv::Mat       input_half(rows, tile.width, CV_16F, float16_destination);
  cv::parallel_for_(cv::Range(0, rows), [&](const cv::Range& range) {
    cv::Mat target = input_half.rowRange(range.start, range.end);
    input_float.rowRange(range.start, range.end).convertTo(target, CV_16F);
  });
}

void WriteUtNet2OutputCrop(const UtNet2Float16OutputRows& source, const PackedTileRect& tile,
                           const TileOutputCrop& crop, float output_scale) {
  const int   output_width = UtNet2Spec::kOutputUpscale * tile.width;
  const auto& target       = crop.destination;
  const auto  target_plane = static_cast<std::size_t>(target.width) * target.height;
  const auto  source_rect  = [&](int channel) {
    auto* rows = const_cast<std::uint8_t*>(source.first_row) + channel * source.plane_bytes;
    return cv::Mat(crop.height, output_width, CV_16F, rows,
                     source.row_bytes)(cv::Rect(crop.source_x, 0, crop.width, crop.height));
  };
  const auto destination_rect = [&](int channel) {
    return cv::Mat(target.height, target.width, CV_32F,
                   target.data.data() + channel * target_plane)(
        cv::Rect(crop.destination_x, crop.destination_y, crop.width, crop.height));
  };
  std::atomic<bool> all_finite{true};
  cv::parallel_for_(
      cv::Range(0, UtNet2Spec::kOutputChannels * crop.height), [&](const cv::Range& range) {
        for (int row = range.start; row < range.end;) {
          const int channel = row / crop.height;
          const int first   = row % crop.height;
          const int last    = std::min(crop.height, first + (range.end - row));
          cv::Mat   rows    = destination_rect(channel).rowRange(first, last);
          source_rect(channel).rowRange(first, last).convertTo(rows, CV_32F, output_scale);
          if (!cv::checkRange(rows, true)) {
            all_finite.store(false, std::memory_order_relaxed);
          }
          row += last - first;
        }
      });
  if (all_finite.load()) {
    return;
  }
  for (int channel = 0; channel < UtNet2Spec::kOutputChannels; ++channel) {
    cv::Point first_invalid;
    if (!cv::checkRange(destination_rect(channel), true, &first_invalid)) {
      std::ostringstream text;
      text << UtNet2TileText(tile) << ": the output has a value that is not finite (FP16 overflow) "
           << "at channel " << channel << ", output row " << crop.source_y + first_invalid.y
           << ", output column " << crop.source_x + first_invalid.x;
      throw std::runtime_error(text.str());
    }
  }
}

}  // namespace alcedo::nn
