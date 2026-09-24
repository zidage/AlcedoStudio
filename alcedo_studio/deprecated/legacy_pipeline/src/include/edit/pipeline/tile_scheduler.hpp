//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>

#include "concurrency/thread_pool.hpp"
#include "edit/operators/op_base.hpp"
#include "edit/operators/op_kernel.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
template <typename KernelStreamT>
class StaticTileScheduler {
 private:
  std::shared_ptr<ImageBuffer> input_img_;
  KernelStreamT                stream_;

  ThreadPool                   thread_pool_;
  int                          total_tiles_;   // total number of tiles
  int                          tile_per_row_;  // number of tiles per row
  int                          tile_per_col_;  // number of tiles per column
  int                          tile_size_;     // dynamic tile size based on image size

 public:
  StaticTileScheduler() = delete;
  StaticTileScheduler(std::shared_ptr<ImageBuffer> input_img, KernelStreamT stream,
                      int num_threads = 20)
      : input_img_(input_img), stream_(std::move(stream)), thread_pool_(num_threads) {
    auto& input_buffer = input_img_->GetCPUData();
    // Determine tile size based on image dimensions
    // _tile_size         = std::max(input_buffer.cols, input_buffer.rows) / num_threads;
    tile_size_         = 64;  // Fixed tile size for better cache locality
    tile_per_col_      = static_cast<int>(std::ceil(static_cast<float>(input_buffer.cols) / tile_size_));
    tile_per_row_      = static_cast<int>(std::ceil(static_cast<float>(input_buffer.rows) / tile_size_));
    total_tiles_       = tile_per_col_ * tile_per_row_;
  }
  void SetInputImage(std::shared_ptr<ImageBuffer> img) { input_img_ = img; }
  auto ApplyOps(OperatorParams& params) -> std::shared_ptr<ImageBuffer> {
    // Similar implementation as TileScheduler but using static stream
    using clock = std::chrono::high_resolution_clock;
    auto start  = clock::now();

    if (!input_img_) {
      throw std::runtime_error("TileScheduler: Input image not set.");
    }

    // Use const& for the input buffer.
    const cv::Mat&          input_buffer = input_img_->GetCPUData();
    cv::Mat                 output_buffer{input_buffer.size(), input_buffer.type()};

    std::atomic<int>        tiles_completed = 0;
    std::mutex              mtx;
    std::condition_variable cv;
    const int               channels = input_buffer.channels();

    for (int tile_idx = 0; tile_idx < total_tiles_; ++tile_idx) {
      thread_pool_.Submit([this, &params = params, tile_idx, &input_buffer, &output_buffer,
                           &tiles_completed, &mtx, &cv, channels]() {
        // Get tile's starting coordinates
        // EASY_BLOCK("Tile Processing");
        int            tile_x = (tile_idx % tile_per_col_) * tile_size_;
        int            tile_y = (tile_idx / tile_per_col_) * tile_size_;

        // Define the tile's region of interest (ROI), clamping to image boundaries
        int            height = std::min(tile_size_, input_buffer.rows - tile_y);
        int            width  = std::min(tile_size_, input_buffer.cols - tile_x);

        Tile           tile   = Tile::CopyFrom(input_buffer, {tile_x, tile_y, width, height}, 0);

        OperatorParams params_copy = params;  // Copy params for thread safety
        stream_.ProcessTile(tile, params_copy);

        Tile::CopyInto(output_buffer, tile);

        // Atomically signal completion and notify the main thread if all tasks are done
        if (tiles_completed.fetch_add(1, std::memory_order_release) + 1 == total_tiles_) {
          std::lock_guard<std::mutex> lock(mtx);
          cv.notify_one();
        }
        // EASY_END_BLOCK;
      });
    }
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock,
            [&]() { return tiles_completed.load(std::memory_order_acquire) == total_tiles_; });

    auto                                      end      = clock::now();
    std::chrono::duration<double, std::milli> duration = end - start;
    printf("TileScheduler: Processed %d tiles in %.2f ms\n", total_tiles_, duration.count());
    return std::make_shared<ImageBuffer>(std::move(output_buffer));
  }
  auto HasOps() const -> bool { return true; }
};
};  // namespace alcedo