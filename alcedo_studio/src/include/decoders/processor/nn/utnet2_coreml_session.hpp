//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "decoders/processor/nn/overlap_tile_jobs.hpp"
#include "decoders/processor/nn/utnet2_specs.hpp"
#include "decoders/processor/nn/utnet2_tile_loop.hpp"

namespace alcedo::nn {

/// The identity of the device of a Core ML session: the Neural Engine of the chip, for example
/// "Apple M4 Neural Engine". Creates no session. Throws `std::runtime_error` when the chip name
/// cannot be read.
[[nodiscard]] auto        CoreMlDeviceKey() -> std::string;

/// The folder of the compiled Core ML models of AI Denoise: `alcedo-studio/ai-denoise-coreml`
/// in the cache folder of the user that macOS manages (`_CS_DARWIN_USER_CACHE_DIR`; macOS
/// removes its content when the disk is low). Throws `std::runtime_error` when macOS gives no
/// cache folder.
[[nodiscard]] auto        CoreMlModelCacheRoot() -> std::filesystem::path;

/// The model cache folder of `model_version` below `root` (`root/<model_version>`), created when
/// absent. The other entries of `root` (the compiled models of other model versions) are
/// removed. Throws `std::runtime_error` when `model_version` is not 64 lower-case hex digits
/// (the SHA-256 of the graph, D10) or the folder cannot be created; a failed removal of another
/// entry is not an error (the next call removes it again).
[[nodiscard]] auto        PrepareCoreMlModelCache(const std::filesystem::path& root,
                                                  std::string_view model_version) -> std::filesystem::path;

/// Checks the ONNX Runtime library of the process (libonnxruntime.1.24.4.dylib): its version is
/// the version of the headers and it has the Core ML execution provider. Creates no session.
/// Throws `std::runtime_error` that names the difference.
void                      LoadCoreMlRuntime();

/// The ONNX Runtime sessions of the FP16 UtNet2 graph on the Neural Engine (macOS, Apple silicon;
/// integration plan, DN10).
///
/// The constructor checks the graph against `UtNet2Spec` with a session of the CPU execution
/// provider, which it ends at once. A run of a tile shape (height, width) that has no session
/// creates one: the free dimensions of the graph fixed to the shape, graph optimization level
/// all, the Core ML execution provider with `ModelFormat` MLProgram and `MLComputeUnits`
/// CPUAndNeuralEngine, and no CPU fallback (a node that Core ML does not take is a load error).
/// A session with free height and width puts half of the operations on the CPU (DN1 addition,
/// FP16 on the Neural Engine), so each shape has its own session. The sessions stay until
/// `Release`.
///
/// A run converts the float32 tile input to float16 on the host, runs the session of its shape
/// with float16 host tensors, and converts the rows of the written rectangle to float32 times
/// `output_scale` (UtNet2 tile I/O: the code of the DirectML session).
///
/// Errors: every failure throws `std::runtime_error`. A load error names the graph path and the
/// first difference or the ONNX Runtime error; a run error names the tile rectangle.
///
/// Thread safety: one call at a time. The owner of the session (DenoiseModelSession) serializes
/// the runs of the editor and the export.
class UtNet2CoreMlSession final : public UtNet2TileSession {
 public:
  /// Checks the graph at `graph_path`. `model_cache_folder` (it can be empty) holds the compiled
  /// Core ML models of the sessions, one folder `<width>x<height>` for each tile shape (the
  /// `ModelCacheDirectory` of the Core ML execution provider, which keys its cache on the graph and
  /// not on the shape). The folder belongs to one graph: `PrepareCoreMlModelCache` gives one for
  /// each model version. A session of a shape in the cache is created in 0.01 to 0.02 s instead of
  /// 0.3 to 0.4 s (DN10 completion record); empty compiles each session. Throws on a missing file
  /// or a graph that differs from `UtNet2Spec`.
  explicit UtNet2CoreMlSession(std::filesystem::path graph_path,
                               std::filesystem::path model_cache_folder = {});
  ~UtNet2CoreMlSession() override;

  UtNet2CoreMlSession(const UtNet2CoreMlSession&)                    = delete;
  auto operator=(const UtNet2CoreMlSession&) -> UtNet2CoreMlSession& = delete;
  UtNet2CoreMlSession(UtNet2CoreMlSession&&) noexcept;
  auto operator=(UtNet2CoreMlSession&&) noexcept -> UtNet2CoreMlSession&;

  /// Runs one tile and writes the rectangle of `crop` (see `UtNet2TileSession::Run`). The first
  /// run of a tile shape creates its session.
  ///
  /// Throws, with the destination rectangle unspecified, when the height or the width is not a
  /// positive multiple of 16, when the input size does not match the tile or the crop is outside
  /// the tile output or the destination, when ONNX Runtime or Core ML fails (its status text), or
  /// when an output value in the rectangle is not finite (FP16 overflow; the text names the tile
  /// rectangle and the first such value).
  void Run(std::span<const float> input, const PackedTileRect& tile,
           const TileOutputCrop& crop) override;

  /// Runs one tile and writes its whole output: `output` receives camera RGB in float32 planes
  /// (3, 2 tile.height, 2 tile.width). The errors of the crop overload.
  void Run(std::span<const float> input, const PackedTileRect& tile, std::span<float> output);

  /// True when the session of a tile of `width` x `height` packed px exists and has run: the
  /// creation of a session and its first run (Core ML loads the compiled model on the Neural
  /// Engine) are not measured by the slow-device check.
  [[nodiscard]] auto IsReadyFor(int width, int height) const -> bool override;

  /// The physical memory of the Mac (the Neural Engine uses the unified memory): the memory that
  /// macOS does not count as available (`kern.memorystatus_level`) is in use; `held_bytes` is the
  /// memory of the largest tile size of `UtNet2Spec::kCoreMlTileMemory` that the sessions ran.
  [[nodiscard]] auto DeviceMemory() const -> DeviceMemoryState override;

  /// `UtNet2Spec::kCoreMlTileMemory`.
  [[nodiscard]] auto TileMemory() const -> std::span<const UtNet2TileMemory> override;

  /// Ends the sessions of all tile shapes. The graph path and the scale stay; a later `Run`
  /// creates the session of its shape again.
  void               Release();

  /// True while a session of a tile shape exists (after a `Run`, before `Release`).
  [[nodiscard]] auto IsLoaded() const -> bool;

  /// The count of the sessions of tile shapes that exist.
  [[nodiscard]] auto SessionCount() const -> int;

  /// The `output_scale` of the graph metadata.
  [[nodiscard]] auto OutputScale() const -> float;

  [[nodiscard]] auto GraphPath() const -> const std::filesystem::path&;

  /// The sum of the creation times of the sessions since the constructor or the last
  /// `Release`.
  [[nodiscard]] auto SessionCreationTime() const -> std::chrono::nanoseconds;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace alcedo::nn
