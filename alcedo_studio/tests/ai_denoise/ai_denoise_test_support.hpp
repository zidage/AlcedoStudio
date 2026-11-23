//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Builders of the AI Denoise loader tests (integration plan, DN6): a Bayer CFA input with LibRaw
// levels, a camera matrix, a platform with a test session whose output is known, and a model
// folder with a graph file.

#pragma once

#include <libraw/libraw.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <opencv2/core.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "decoders/processor/nn/utnet2_specs.hpp"
#include "decoders/processor/nn/utnet2_tile_loop.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/ai_denoise_model_session.hpp"
#include "edit/input/ai_denoise_platform.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "image/image.hpp"
#include "image/metadata_extractor.hpp"

namespace alcedo::ai_denoise_test {

/// The camera matrix of the gain match of the RAW file @p path: the D65 matrix of the render of
/// its import camera profile (#1055), as the editor binds it.
inline auto ImportedXyzToCamera(const std::filesystem::path& path) -> std::array<double, 9> {
  Image image(1, path, ImageType::DEFAULT);
  MetadataExtractor::ExtractEXIF_ToImage(path, image);
  if (!image.HasRawColorContext()) {
    throw std::runtime_error("no RAW colour context in " + path.string());
  }
  auto document = CreateDefaultPipelineDocument();
  BindImportedCameraProfile(document, image.GetRawColorContext());
  const auto resolved = ResolveDevelopXyzToCameraD65(document.Develop()->Params().Params());
  if (!resolved.ok) {
    throw std::runtime_error("no camera matrix for " + path.string() + ": " +
                             std::string(ColorTransformErrorMessage(resolved.error)));
  }
  return resolved.xyz_to_camera;
}

/// True when LibRaw `color.cam_xyz` of @p path, the matrix that DN6 read, has no inverse.
inline auto LibRawCamXyzIsSingular(const std::filesystem::path& path) -> bool {
  auto raw = std::make_unique<LibRaw>();
  if (raw->open_file(path.string().c_str()) != LIBRAW_SUCCESS) {
    throw std::runtime_error("LibRaw cannot open " + path.string());
  }
  cv::Matx33d matrix;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      matrix(r, c) = raw->imgdata.color.cam_xyz[r][c];
    }
  }
  const double determinant = cv::determinant(matrix);
  return !std::isfinite(determinant) || std::abs(determinant) < 1e-12;
}

/// The output of the test session: output channel c at output (v, u) of a tile is the input of
/// packed channel kChannelOfOutput[c] at (v / 2, u / 2) times kTestOutputScale (the model output
/// is about 1e5 times its input, lab docs/alcedo_integration.md).
inline constexpr int    kChannelOfOutput[3] = {0, 1, 3};
inline constexpr double kTestOutputScale    = 1e5;

/// A camera matrix (XYZ to camera, the gain match input of Denoise) that has an inverse.
inline constexpr std::array<double, 9> kTestXyzToCamera    = {0.70,  0.20, 0.10,  0.30, 0.90,
                                                              -0.10, 0.05, -0.10, 0.80};

/// The classic Bayer pattern whose top left cell is @p name ("RGGB", "GBRG", "GRBG", "BGGR").
inline auto             BayerPattern(const std::string& name) -> RawCfaPattern {
  RawCfaPattern pattern;
  pattern.kind     = RawCfaKind::Bayer2x2;
  bool first_green = true;
  for (int i = 0; i < 4; ++i) {
    switch (name[i]) {
      case 'R':
        pattern.bayer_pattern.raw_fc[i] = 0;
        pattern.bayer_pattern.rgb_fc[i] = 0;
        break;
      case 'B':
        pattern.bayer_pattern.raw_fc[i] = 2;
        pattern.bayer_pattern.rgb_fc[i] = 2;
        break;
      default:
        pattern.bayer_pattern.raw_fc[i] = first_green ? 1 : 3;
        pattern.bayer_pattern.rgb_fc[i] = 1;
        first_green                     = false;
        break;
    }
  }
  return pattern;
}

/// LibRaw levels: a black level for each raw color, white level 15360, maximum 16383.
inline auto TestLinearization() -> RawLinearizationParams {
  RawLinearizationParams linearization;
  const float            black[4] = {512.0f, 510.0f, 514.0f, 508.0f};
  for (int c = 0; c < 4; ++c) {
    linearization.black_level[c] = black[c];
    linearization.white_level[c] = 15360.0f;
  }
  linearization.cam_mul[0]     = 2.2f;
  linearization.cam_mul[1]     = 1.0f;
  linearization.cam_mul[2]     = 1.6f;
  linearization.cam_mul[3]     = 1.0f;
  linearization.libraw_maximum = 16383.0f;
  return linearization;
}

/// The raw code of host position (y, x): a unique value, some below the black level and some
/// above the maximum.
inline auto TestCode(int y, int x) -> std::uint16_t {
  switch ((x + 3 * y) % 7) {
    case 0:
      return 450;  // below each black level
    case 1:
      return 16500;  // above the maximum
    default:
      return static_cast<std::uint16_t>(600 + (y * 131 + x * 17) % 15000);
  }
}

/// A full-size Bayer CFA input of @p width x @p height host px with @p pattern and the test
/// levels. @p sensor sets margins or a crop; empty: the whole plane. @p code
/// gives the code of host position (y, x).
inline auto MakeBayerInput(std::uint32_t width, std::uint32_t height, const RawCfaPattern& pattern,
                           std::optional<RawSensorGeometry>              sensor = std::nullopt,
                           const std::function<std::uint16_t(int, int)>& code   = TestCode)
    -> PreparedRawInput {
  HostImagePlane plane;
  plane.extent       = Extent2D{width, height};
  plane.stride_bytes = width * static_cast<std::uint32_t>(sizeof(std::uint16_t));
  plane.format       = HostPixelFormat::U16Cfa;
  auto  storage      = std::shared_ptr<std::byte>(new std::byte[plane.ByteCount()],
                                                  [](std::byte* p) { delete[] p; });
  auto* codes        = reinterpret_cast<std::uint16_t*>(storage.get());
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      codes[static_cast<std::size_t>(y) * width + x] =
          code(static_cast<int>(y), static_cast<int>(x));
    }
  }
  plane.bytes = std::const_pointer_cast<const std::byte>(storage);
  return RawInputLoader::FromUnpackedCfa(std::move(plane), pattern, TestLinearization(),
                                         sensor.value_or(RawSensorGeometry{}), DecodeRes::FULL);
}

/// The code of a dark photo: each 2 x 2 cell has one code (so G1 and G2 are equal), mostly up to
/// 120 above the black level, with a few cells below the black level, at the LibRaw maximum, and
/// above it.
inline auto DarkCellCode(int y, int x) -> std::uint16_t {
  const int cell = (y / 2) * 7919 + x / 2;
  if (cell % 997 == 0) return 16383;  // the LibRaw maximum
  if (cell % 991 == 0) return 16500;  // above the maximum
  if (cell % 13 == 0) return 500;     // below the black level
  return static_cast<std::uint16_t>(512 + ((y / 2) * 37 + (x / 2) * 11) % 120);
}

/// A dark RGGB input of DarkCellCode with the black level 512 for each color: the prepared values
/// of a cell are equal for G1 and G2. Even sizes keep every cell inside the area.
inline auto MakeDarkBayerInput(std::uint32_t width, std::uint32_t height) -> PreparedRawInput {
  auto input = MakeBayerInput(width, height, BayerPattern("RGGB"), std::nullopt, DarkCellCode);
  for (int c = 0; c < 4; ++c) {
    input.linearization.black_level[c] = 512.0f;
  }
  return input;
}

/// What the test platform and its sessions record.
struct TestPlatformRecord {
  std::atomic<int>          sessions_created{0};
  std::atomic<int>          sessions_alive{0};
  std::atomic<int>          tile_runs{0};
  std::atomic<int>          active_runs{0};
  std::atomic<int>          most_active_runs{0};
  std::string               device_key = "test device A";
  /// The run of this index (0-based, over all sessions) throws.
  std::atomic<int>          failing_tile_run{-1};
  /// Each tile run sleeps this long (the concurrency test).
  std::chrono::milliseconds run_sleep{0};
  /// CheckRuntime throws this text when it is not empty (DN7 platform check).
  std::string               runtime_load_error;
  std::atomic<int>          runtime_checks{0};
};

/// A session whose output is known (kChannelOfOutput, kTestOutputScale) and that is ready for
/// each tile shape (each tile is measured by the slow-device check).
class TestTileSession final : public nn::UtNet2TileSession {
 public:
  explicit TestTileSession(std::shared_ptr<TestPlatformRecord> record)
      : record_(std::move(record)) {
    ++record_->sessions_alive;
  }
  ~TestTileSession() override { --record_->sessions_alive; }

  void Run(std::span<const float> input, const nn::PackedTileRect& tile,
           const nn::TileOutputCrop& crop) override {
    const int active = ++record_->active_runs;
    int       most   = record_->most_active_runs.load();
    while (active > most && !record_->most_active_runs.compare_exchange_weak(most, active)) {
    }
    const int run = record_->tile_runs++;
    if (record_->run_sleep.count() > 0) {
      std::this_thread::sleep_for(record_->run_sleep);
    }
    if (run == record_->failing_tile_run.load()) {
      --record_->active_runs;
      throw std::runtime_error("test session: DirectML device removed (887A0005)");
    }
    const auto  plane        = static_cast<std::size_t>(tile.width) * tile.height;
    const auto& target       = crop.destination;
    const auto  target_plane = static_cast<std::size_t>(target.width) * target.height;
    for (int c = 0; c < 3; ++c) {
      for (int v = 0; v < crop.height; ++v) {
        for (int u = 0; u < crop.width; ++u) {
          const int ty = (crop.source_y + v) / 2;
          const int tx = (crop.source_x + u) / 2;
          target.data[c * target_plane +
                      static_cast<std::size_t>(crop.destination_y + v) * target.width +
                      crop.destination_x + u] =
              static_cast<float>(input[kChannelOfOutput[c] * plane +
                                       static_cast<std::size_t>(ty) * tile.width + tx] *
                                 kTestOutputScale);
        }
      }
    }
    --record_->active_runs;
  }

  [[nodiscard]] auto IsReadyFor(int /*width*/, int /*height*/) const -> bool override {
    return true;
  }

  [[nodiscard]] auto DeviceMemory() const -> nn::DeviceMemoryState override {
    return {.budget_bytes = std::uint64_t{8} << 30};
  }

  [[nodiscard]] auto TileMemory() const -> std::span<const nn::UtNet2TileMemory> override {
    return nn::UtNet2Spec::kDirectMlTileMemory;
  }

 private:
  std::shared_ptr<TestPlatformRecord> record_;
};

class TestPlatform final : public AiDenoisePlatform {
 public:
  explicit TestPlatform(std::shared_ptr<TestPlatformRecord> record) : record_(std::move(record)) {}

  [[nodiscard]] auto DeviceKey() -> std::string override { return record_->device_key; }

  void               CheckRuntime() override {
    ++record_->runtime_checks;
    if (!record_->runtime_load_error.empty()) {
      throw std::runtime_error(record_->runtime_load_error);
    }
  }

  [[nodiscard]] auto CreateSession(const std::filesystem::path& graph_path,
                                   std::string_view /*model_version*/)
      -> std::unique_ptr<nn::UtNet2TileSession> override {
    if (!std::filesystem::is_regular_file(graph_path)) {
      throw std::runtime_error("test platform: no graph at " + graph_path.string());
    }
    ++record_->sessions_created;
    return std::make_unique<TestTileSession>(record_);
  }

 private:
  std::shared_ptr<TestPlatformRecord> record_;
};

/// The file name of the FP16 graph of the lab release (the test assets folder holds it).
inline constexpr std::string_view kFp16GraphFileName = "utnet2_bayer_dark_loss_10k_fp16.onnx";

/// A model folder below build/tmp with `alcedo-ai-denoise-v1/<graph file>` holding @p content.
class TestModelFolder {
 public:
  explicit TestModelFolder(const std::string& name, const std::string& content = "graph v1") {
    folder_ = std::filesystem::path(ALCEDO_AI_DENOISE_TEST_TEMP_ROOT) / name;
    std::filesystem::remove_all(folder_);
    Write(content);
  }
  ~TestModelFolder() {
    std::error_code ignored;
    std::filesystem::remove_all(folder_, ignored);
  }

  void Write(const std::string& content) const {
    std::filesystem::create_directories(GraphPath().parent_path());
    std::ofstream stream(GraphPath(), std::ios::binary | std::ios::trunc);
    stream << content;
  }

  [[nodiscard]] auto Folder() const -> const std::filesystem::path& { return folder_; }
  [[nodiscard]] auto GraphPath() const -> std::filesystem::path {
    return folder_ / "alcedo-ai-denoise-v1" / kFp16GraphFileName;
  }
  /// A locator that finds the graph file of this folder (the application resolves the
  /// installed package; ResolveAiDenoiseGraph has its own tests).
  [[nodiscard]] auto Locator() const -> DenoiseModelSession::GraphLocator {
    return [graph = GraphPath()] { return AiDenoiseGraphLookup{graph, {}}; };
  }

 private:
  std::filesystem::path folder_;
};

/// A DenoiseModelSession on a test platform with tiles of 256 packed px, reading its graph
/// from @p folder.
inline auto MakeTestModelSession(const std::shared_ptr<TestPlatformRecord>& record,
                                 const TestModelFolder& folder, double slow_limit = 1e9)
    -> std::shared_ptr<DenoiseModelSession> {
  nn::UtNet2TileLoopOptions options;
  options.tile_size                               = 256;
  options.slow_device_limit_seconds_per_megapixel = slow_limit;
  options.slow_device_measured_tiles              = 1;
  auto session =
      std::make_shared<DenoiseModelSession>(std::make_unique<TestPlatform>(record), options);
  session->SetGraphLocator(folder.Locator());
  return session;
}

}  // namespace alcedo::ai_denoise_test
