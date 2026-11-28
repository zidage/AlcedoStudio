//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Tests of the overlap tile planner and of the tile loop of AI Denoise (integration plan, DN3)
// with a test session and a test clock. The tiled output on DirectML against one pass is in
// utnet2_directml_session_test.cpp.

#include "decoders/processor/nn/utnet2_tile_loop.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "decoders/processor/nn/denoise_errors.hpp"
#include "decoders/processor/nn/overlap_tile_jobs.hpp"
#include "decoders/processor/nn/utnet2_specs.hpp"

namespace alcedo::nn {
namespace {

constexpr int kAlignment = UtNet2Spec::kSizeAlignment;
constexpr int kOverlap   = UtNet2Spec::kTileOverlap;

/// A clock that moves only when the test session runs a tile.
struct TestClock {
  std::chrono::steady_clock::time_point now{};

  [[nodiscard]] auto                    Function() -> UtNet2SteadyClock {
    return [this] { return now; };
  }
};

/// A packed mosaic whose value at channel c, row y, column x is unique: c * 1e6 + y * 1e3 + x.
struct TestMosaic {
  int                width;
  int                height;
  std::vector<float> data;

  TestMosaic(int width_px, int height_px)
      : width(width_px), height(height_px), data(std::size_t{4} * width_px * height_px) {
    for (int channel = 0; channel < 4; ++channel) {
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          data[(std::size_t{1} * channel * height + y) * width + x] = Value(channel, y, x);
        }
      }
    }
  }

  static auto Value(int channel, int y, int x) -> float {
    return static_cast<float>(channel * 1000000 + y * 1000 + x);
  }

  /// The value of the tile input at (y, x) of the image, past the border by reflection.
  [[nodiscard]] auto Reflected(int channel, int y, int x) const -> float {
    const int ry = y < height ? y : 2 * (height - 1) - y;
    const int rx = x < width ? x : 2 * (width - 1) - x;
    return Value(channel, ry, rx);
  }

  [[nodiscard]] auto View() const -> PackedMosaicView { return {data, width, height}; }
};

/// A session whose output pixel (channel c, output row v, output column u) is the input value of
/// channel c at packed (v / 2, u / 2) of the tile: its one-pass output of a mosaic is known.
/// Each run moves the test clock by `seconds_per_megapixel` times the written output px, plus
/// `first_run_seconds` for the first run of a session that is not ready.
class TestSession final : public UtNet2TileSession {
 public:
  TestSession(TestClock& clock, const TestMosaic* mosaic) : clock_(clock), mosaic_(mosaic) {}

  void Run(std::span<const float> input, const PackedTileRect& tile,
           const TileOutputCrop& crop) override {
    const auto plane = static_cast<std::size_t>(tile.width) * tile.height;
    if (input.size() != 4 * plane) {
      throw std::runtime_error("test session: the input size does not match the tile");
    }
    if (mosaic_ != nullptr) {
      for (int channel = 0; channel < 4; ++channel) {
        for (int y = 0; y < tile.height; ++y) {
          for (int x = 0; x < tile.width; ++x) {
            const float value = input[channel * plane + std::size_t{1} * y * tile.width + x];
            if (value != mosaic_->Reflected(channel, tile.y + y, tile.x + x)) {
              ++wrong_input_values;
            }
          }
        }
      }
    }
    const auto& target       = crop.destination;
    const auto  target_plane = static_cast<std::size_t>(target.width) * target.height;
    for (int channel = 0; channel < 3; ++channel) {
      for (int v = 0; v < crop.height; ++v) {
        for (int u = 0; u < crop.width; ++u) {
          const int ty = (crop.source_y + v) / 2;
          const int tx = (crop.source_x + u) / 2;
          target.data[channel * target_plane +
                      std::size_t{1} * (crop.destination_y + v) * target.width +
                      crop.destination_x + u] =
              input[channel * plane + std::size_t{1} * ty * tile.width + tx];
        }
      }
    }
    const double rate    = runs < static_cast<int>(seconds_per_megapixel_by_run.size())
                               ? seconds_per_megapixel_by_run[runs]
                               : seconds_per_megapixel;
    double       seconds = rate * crop.width * crop.height / 1e6;
    if (!ready_) {
      seconds += first_run_seconds;
      ready_ = true;
    }
    clock_.now += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(seconds));
    ++runs;
  }

  [[nodiscard]] auto IsReadyFor(int /*width*/, int /*height*/) const -> bool override {
    return ready_;
  }

  [[nodiscard]] auto DeviceMemory() const -> DeviceMemoryState override { return memory; }

  [[nodiscard]] auto TileMemory() const -> std::span<const UtNet2TileMemory> override {
    return tile_memory;
  }

  void               StartNewSession() { ready_ = false; }

  double             seconds_per_megapixel = 0.0;
  /// The rate of the first runs, one value for each run (then `seconds_per_megapixel`).
  std::vector<double> seconds_per_megapixel_by_run;
  double             first_run_seconds     = 0.0;
  int                runs                  = 0;
  int                wrong_input_values    = 0;
  DeviceMemoryState  memory{.budget_bytes = std::uint64_t{8} << 30};
  /// The table of the platform of the session (`TileMemory`).
  std::vector<UtNet2TileMemory> tile_memory{UtNet2Spec::kDirectMlTileMemory.begin(),
                                            UtNet2Spec::kDirectMlTileMemory.end()};

 private:
  TestClock&        clock_;
  const TestMosaic* mosaic_;
  bool              ready_ = true;
};

struct LoopOutput {
  int                width;
  int                height;
  std::vector<float> data;

  explicit LoopOutput(const TestMosaic& mosaic)
      : width(2 * mosaic.width),
        height(2 * mosaic.height),
        data(std::size_t{3} * width * height, -1.0f) {}

  [[nodiscard]] auto View() -> RgbPlanesView { return {data, width, height}; }
};

auto NoCancel() -> UtNet2CancelCheck {
  return [] { return false; };
}

auto Options(int tile_size) -> UtNet2TileLoopOptions {
  UtNet2TileLoopOptions options;
  options.tile_size = tile_size;
  return options;
}

TEST(OverlapTileJobsTest, OverlapTilesCoverTheImageExactlyOnce) {
  struct Case {
    int width;
    int height;
    int tile;
  };
  // Sizes that are and are not multiples of the tile and of 16, the packed sizes of the 24.3,
  // 36.2, and 45.4 MP photos, and images smaller than one tile.
  const std::vector<Case> cases = {
      {16, 16, 256},      {100, 37, 256},     {256, 256, 256},    {257, 511, 256},
      {1536, 3072, 1536}, {1537, 1000, 1536}, {3016, 2016, 1536}, {3680, 2456, 1536},
      {4128, 2752, 1536}, {4128, 2752, 1024}, {4128, 2752, 768},  {3984, 2660, 1024},
  };
  for (const auto& c : cases) {
    const auto       tiles = PlanOverlapTiles(c.width, c.height, c.tile, kOverlap, kAlignment);
    std::vector<int> cover(static_cast<std::size_t>(c.width) * c.height, 0);
    for (const auto& tile : tiles) {
      for (int y = tile.kept.y; y < tile.kept.y + tile.kept.height; ++y) {
        for (int x = tile.kept.x; x < tile.kept.x + tile.kept.width; ++x) {
          ++cover[static_cast<std::size_t>(y) * c.width + x];
        }
      }
      // The kept rectangle is inside the input, with the overlap of context inside the image.
      EXPECT_GE(tile.kept.x, tile.input.x);
      EXPECT_GE(tile.kept.y, tile.input.y);
      EXPECT_TRUE(tile.kept.x == 0 || tile.kept.x - tile.input.x >= kOverlap);
      EXPECT_TRUE(tile.kept.y == 0 || tile.kept.y - tile.input.y >= kOverlap);
      const int kept_right  = tile.kept.x + tile.kept.width;
      const int kept_bottom = tile.kept.y + tile.kept.height;
      EXPECT_TRUE(kept_right == c.width ||
                  tile.input.x + tile.input.width - kept_right >= kOverlap);
      EXPECT_TRUE(kept_bottom == c.height ||
                  tile.input.y + tile.input.height - kept_bottom >= kOverlap);
      EXPECT_LE(tile.input.width, c.tile);
      EXPECT_LE(tile.input.height, c.tile);
    }
    int not_once = 0;
    for (const int count : cover) {
      not_once += count != 1 ? 1 : 0;
    }
    EXPECT_EQ(not_once, 0) << c.width << " x " << c.height << ", tile " << c.tile;
  }
}

TEST(OverlapTileJobsTest, OverlapTileInputsAreMultiplesOfSixteen) {
  for (const int length : {16, 17, 100, 1535, 1536, 1537, 2456, 2660, 2752, 3016, 3984, 4128}) {
    for (const int tile : {256, 768, 1024, 1536}) {
      for (const auto& axis : PlanOverlapAxis(length, tile, kOverlap, kAlignment)) {
        EXPECT_EQ(axis.input_start % kAlignment, 0) << length << ", tile " << tile;
        EXPECT_EQ(axis.input_size % kAlignment, 0) << length << ", tile " << tile;
        EXPECT_GE(axis.input_start, 0);
        EXPECT_LT(axis.input_start, length);
        // The part past the end of the axis is shorter than the alignment: reflection fills it.
        EXPECT_LT(axis.input_start + axis.input_size - length, kAlignment);
      }
    }
  }
  // The last tile of the 45.4 MP width is shrunk to its need, not filled to the full tile.
  const auto axis = PlanOverlapAxis(4128, 1536, kOverlap, kAlignment);
  ASSERT_EQ(axis.size(), 3U);
  EXPECT_EQ(axis.back().input_start, 2688);
  EXPECT_EQ(axis.back().input_size, 1440);
  EXPECT_THROW((void)PlanOverlapAxis(1000, 200, kOverlap, kAlignment), std::invalid_argument);
  EXPECT_THROW((void)PlanOverlapAxis(1000, 192, kOverlap, kAlignment), std::invalid_argument);
  EXPECT_THROW((void)PlanOverlapAxis(1000, 512, 90, kAlignment), std::invalid_argument);
}

TEST(UtNet2TileLoopTest, TileSizeSelectionUsesTheLargestThatFits) {
  const auto table    = std::span<const UtNet2TileMemory>(UtNet2Spec::kDirectMlTileMemory);
  const auto bytes_of = [&](int tile_size) {
    for (const auto& entry : table) {
      if (entry.tile_size == tile_size) {
        return entry.device_bytes;
      }
    }
    throw std::logic_error("no entry");
  };
  EXPECT_EQ(SelectUtNet2TileSize(table, std::uint64_t{64} << 30), 1536);
  EXPECT_EQ(SelectUtNet2TileSize(table, bytes_of(1536)), 1536);
  EXPECT_EQ(SelectUtNet2TileSize(table, bytes_of(1536) - 1), 1024);
  EXPECT_EQ(SelectUtNet2TileSize(table, bytes_of(1024)), 1024);
  EXPECT_EQ(SelectUtNet2TileSize(table, bytes_of(1024) - 1), 768);
  EXPECT_EQ(SelectUtNet2TileSize(table, bytes_of(768)), 768);
  try {
    (void)SelectUtNet2TileSize(table, bytes_of(768) - 1);
    ADD_FAILURE() << "no error below the smallest tile";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("not enough device memory"), std::string::npos)
        << error.what();
  }

  // The loop adds the memory that the session holds to the free memory.
  TestClock   clock;
  TestMosaic  mosaic(64, 48);
  LoopOutput  output(mosaic);
  TestSession session(clock, nullptr);
  session.memory = {.budget_bytes = bytes_of(1536) + (std::uint64_t{100} << 20),
                    .usage_bytes  = bytes_of(1536),
                    .held_bytes   = bytes_of(1536)};
  EXPECT_EQ(
      DenoiseBayerMosaic(session, mosaic.View(), output.View(), {}, NoCancel(), clock.Function())
          .tile_size,
      1536);
  session.memory.held_bytes = 0;
  EXPECT_THROW((void)DenoiseBayerMosaic(session, mosaic.View(), output.View(), {}, NoCancel(),
                                        clock.Function()),
               std::runtime_error);
}

TEST(UtNet2TileLoopTest, TileSizeSelectionCountsTheVideoMemoryOfOtherProcesses) {
  constexpr std::uint64_t kMiB = std::uint64_t{1} << 20;
  // RTX 3080 Laptop GPU (issue #1043): the budget of the process is 4331 MiB, but other processes
  // use 6250 MiB of the 8016 MiB of the adapter: 1766 MiB are free, enough for tiles of 1024.
  DeviceMemoryState       memory{.budget_bytes              = 4331 * kMiB,
                                 .usage_bytes               = 0,
                                 .held_bytes                = 0,
                                 .adapter_local_bytes       = 8016 * kMiB,
                                 .adapter_local_usage_bytes = 6250 * kMiB};
  EXPECT_EQ(AvailableTileMemory(memory), 1766 * kMiB);
  EXPECT_EQ(SelectUtNet2TileSize(UtNet2Spec::kDirectMlTileMemory, AvailableTileMemory(memory)),
            1024);

  // The session holds its arena of tiles of 1536: it counts in both uses, and is available.
  memory.usage_bytes               = 2704 * kMiB;
  memory.held_bytes                = 2704 * kMiB;
  memory.budget_bytes              = 7248 * kMiB;
  memory.adapter_local_usage_bytes = 5363 * kMiB;
  EXPECT_EQ(AvailableTileMemory(memory), (8016 - 5363 + 2704) * kMiB);
  EXPECT_EQ(SelectUtNet2TileSize(UtNet2Spec::kDirectMlTileMemory, AvailableTileMemory(memory)),
            1536);

  // An adapter without local memory (UMA, Intel Iris Xe): the budget of the process only.
  memory = {.budget_bytes = 15872 * kMiB, .usage_bytes = 0, .held_bytes = 0};
  EXPECT_EQ(AvailableTileMemory(memory), 15872 * kMiB);

  // Other processes use more than the local memory: nothing is free.
  memory = {.budget_bytes              = 4000 * kMiB,
            .usage_bytes               = 0,
            .held_bytes                = 0,
            .adapter_local_bytes       = 8016 * kMiB,
            .adapter_local_usage_bytes = 8100 * kMiB};
  EXPECT_EQ(AvailableTileMemory(memory), 0U);
}

// DN10: the product passes no table, so the loop selects with the table of the platform session
// (Core ML on macOS has another table than DirectML); a table of the options replaces it.
TEST(UtNet2TileLoopTest, LoopSelectsWithTheTableOfTheSessionWhenTheOptionsGiveNone) {
  constexpr std::uint64_t kMiB = std::uint64_t{1} << 20;
  TestClock               clock;
  TestMosaic              mosaic(64, 48);
  LoopOutput              output(mosaic);
  TestSession             session(clock, nullptr);
  session.tile_memory = {{.tile_size = 512, .device_bytes = 900 * kMiB},
                         {.tile_size = 256, .device_bytes = 300 * kMiB}};
  session.memory      = {.budget_bytes = 1000 * kMiB};
  EXPECT_EQ(
      DenoiseBayerMosaic(session, mosaic.View(), output.View(), {}, NoCancel(), clock.Function())
          .tile_size,
      512);
  session.memory.budget_bytes = 500 * kMiB;
  EXPECT_EQ(
      DenoiseBayerMosaic(session, mosaic.View(), output.View(), {}, NoCancel(), clock.Function())
          .tile_size,
      256);

  const std::array<UtNet2TileMemory, 1> options_table = {
      {{.tile_size = 768, .device_bytes = 100 * kMiB}}};
  UtNet2TileLoopOptions options;
  options.tile_memory = options_table;
  EXPECT_EQ(DenoiseBayerMosaic(session, mosaic.View(), output.View(), {}, NoCancel(),
                               clock.Function(), options)
                .tile_size,
            768);
}

TEST(UtNet2TileLoopTest, TileInputsAreTheMosaicWithReflectionAndTheCropsFormTheOutput) {
  TestClock   clock;
  TestMosaic  mosaic(400, 300);
  LoopOutput  output(mosaic);
  TestSession session(clock, &mosaic);
  int         callbacks = 0;

  const auto  record    = DenoiseBayerMosaic(
      session, mosaic.View(), output.View(), [&](const UtNet2TileResult&) { ++callbacks; },
      NoCancel(), clock.Function(), Options(256));

  EXPECT_FALSE(record.cancelled);
  EXPECT_EQ(record.tile_count, 8);
  EXPECT_EQ(record.tiles_run, 8);
  EXPECT_EQ(callbacks, 8);
  EXPECT_EQ(session.wrong_input_values, 0);
  int wrong_output_values = 0;
  for (int channel = 0; channel < 3; ++channel) {
    for (int v = 0; v < output.height; ++v) {
      for (int u = 0; u < output.width; ++u) {
        const float value =
            output.data[(std::size_t{1} * channel * output.height + v) * output.width + u];
        wrong_output_values += value != TestMosaic::Value(channel, v / 2, u / 2) ? 1 : 0;
      }
    }
  }
  EXPECT_EQ(wrong_output_values, 0);
}

TEST(UtNet2TileLoopTest, TileLoopStopsAtCancelRequest) {
  TestClock   clock;
  TestMosaic  mosaic(400, 300);
  LoopOutput  output(mosaic);
  TestSession session(clock, nullptr);
  int         callbacks = 0;

  const auto  record    = DenoiseBayerMosaic(
      session, mosaic.View(), output.View(), [&](const UtNet2TileResult&) { ++callbacks; },
      [&] { return callbacks >= 2; }, clock.Function(), Options(256));

  EXPECT_TRUE(record.cancelled);
  EXPECT_EQ(callbacks, 2);
  EXPECT_EQ(session.runs, 2);
  EXPECT_EQ(record.tiles_run, 2);
}

TEST(UtNet2TileLoopTest, TileLoopStopsWhenTheProjectedTimeIsAboveTheLimit) {
  TestClock  clock;
  TestMosaic mosaic(400, 300);
  LoopOutput output(mosaic);
  {
    // Intel Iris Xe: 0.114 s for each megapixel.
    TestSession session(clock, nullptr);
    session.seconds_per_megapixel = 0.114;
    int callbacks                 = 0;
    try {
      (void)DenoiseBayerMosaic(
          session, mosaic.View(), output.View(), [&](const UtNet2TileResult&) { ++callbacks; },
          NoCancel(), clock.Function(), Options(256));
      ADD_FAILURE() << "the loop ran all tiles";
    } catch (const DenoiseDeviceTooSlowError& error) {
      EXPECT_EQ(error.TilesRun(), UtNet2Spec::kSlowDeviceMeasuredTiles);
      EXPECT_EQ(error.TileCount(), 8);
      EXPECT_NEAR(error.ProjectedSeconds(), 0.114 * 0.48, 1e-6);
      EXPECT_NEAR(error.LimitSeconds(), UtNet2Spec::kSlowDeviceLimitSecondsPerMegapixel * 0.48,
                  1e-9);
    }
    EXPECT_EQ(session.runs, UtNet2Spec::kSlowDeviceMeasuredTiles);
    EXPECT_EQ(callbacks, UtNet2Spec::kSlowDeviceMeasuredTiles);
  }
  {
    // RTX 3080 Laptop GPU: 0.016 s for each megapixel.
    TestSession session(clock, nullptr);
    session.seconds_per_megapixel = 0.016;
    const auto record = DenoiseBayerMosaic(session, mosaic.View(), output.View(), {}, NoCancel(),
                                           clock.Function(), Options(256));
    EXPECT_EQ(record.tiles_run, 8);
    EXPECT_EQ(record.measured_tiles, 8);
    EXPECT_NEAR(std::chrono::duration<double>(record.run_time).count(), 0.016 * 0.48, 1e-6);
  }
}

TEST(UtNet2TileLoopTest, SlowDeviceCheckExcludesTheFirstRunOfASession) {
  TestClock   clock;
  TestMosaic  mosaic(400, 300);
  LoopOutput  output(mosaic);
  TestSession session(clock, nullptr);
  session.seconds_per_megapixel = 0.016;
  session.first_run_seconds     = 2.0;

  session.StartNewSession();
  const auto record = DenoiseBayerMosaic(session, mosaic.View(), output.View(), {}, NoCancel(),
                                         clock.Function(), Options(256));
  EXPECT_EQ(record.tiles_run, 8);
  EXPECT_EQ(record.measured_tiles, 7);

  // A photo of one tile on a new session: no measured tile, no check.
  TestMosaic one_tile(200, 100);
  LoopOutput one_tile_output(one_tile);
  session.StartNewSession();
  const auto single = DenoiseBayerMosaic(session, one_tile.View(), one_tile_output.View(), {},
                                         NoCancel(), clock.Function(), Options(256));
  EXPECT_EQ(single.tile_count, 1);
  EXPECT_EQ(single.measured_tiles, 0);

  // A slow device on a new session stops after the first tile and its measured tiles.
  session.seconds_per_megapixel = 0.114;
  session.StartNewSession();
  try {
    (void)DenoiseBayerMosaic(session, mosaic.View(), output.View(), {}, NoCancel(),
                             clock.Function(), Options(256));
    ADD_FAILURE() << "the loop ran all tiles";
  } catch (const DenoiseDeviceTooSlowError& error) {
    EXPECT_EQ(error.TilesRun(), 1 + UtNet2Spec::kSlowDeviceMeasuredTiles);
  }
}

TEST(UtNet2TileLoopTest, SlowDeviceCheckUsesTheFastestMeasuredTile) {
  TestClock  clock;
  TestMosaic mosaic(400, 300);
  LoopOutput output(mosaic);
  {
    // A fast device whose first two tiles wait for another process (issue #1043: 0.16 s for a
    // tile of 0.03 s): the third tile has the time of the device, and the photo completes.
    TestSession session(clock, nullptr);
    session.seconds_per_megapixel        = 0.016;
    session.seconds_per_megapixel_by_run = {0.09, 0.08};
    const auto record = DenoiseBayerMosaic(session, mosaic.View(), output.View(), {}, NoCancel(),
                                           clock.Function(), Options(256));
    EXPECT_EQ(record.tiles_run, 8);
    EXPECT_EQ(record.measured_tiles, 8);
  }
  {
    // A slow device whose second tile is faster: the projection uses the fastest tile.
    TestSession session(clock, nullptr);
    session.seconds_per_megapixel        = 0.114;
    session.seconds_per_megapixel_by_run = {0.114, 0.07};
    try {
      (void)DenoiseBayerMosaic(session, mosaic.View(), output.View(), {}, NoCancel(),
                               clock.Function(), Options(256));
      ADD_FAILURE() << "the loop ran all tiles";
    } catch (const DenoiseDeviceTooSlowError& error) {
      EXPECT_EQ(error.TilesRun(), UtNet2Spec::kSlowDeviceMeasuredTiles);
      EXPECT_NEAR(error.ProjectedSeconds(), 0.07 * 0.48, 1e-6);
    }
  }
  {
    // A photo of two tiles on a slow device: the check after the last tile.
    TestMosaic  two_tiles(300, 100);
    LoopOutput  two_tiles_output(two_tiles);
    TestSession session(clock, nullptr);
    session.seconds_per_megapixel = 0.114;
    try {
      (void)DenoiseBayerMosaic(session, two_tiles.View(), two_tiles_output.View(), {}, NoCancel(),
                               clock.Function(), Options(256));
      ADD_FAILURE() << "the loop has no check after the last tile";
    } catch (const DenoiseDeviceTooSlowError& error) {
      EXPECT_EQ(error.TilesRun(), 2);
      EXPECT_EQ(error.TileCount(), 2);
    }
  }
}

}  // namespace
}  // namespace alcedo::nn
