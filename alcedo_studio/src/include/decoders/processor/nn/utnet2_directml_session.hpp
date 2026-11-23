//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include "decoders/processor/nn/overlap_tile_jobs.hpp"
#include "decoders/processor/nn/utnet2_tile_loop.hpp"

namespace alcedo::nn {

/// The adapter that the session selects (DXGI GPU preference; software adapters are skipped).
/// The product uses `HighPerformance`; `MinimumPower` selects the integrated GPU of a laptop
/// for the timing tool.
enum class DirectMlAdapterPreference {
  HighPerformance,
  MinimumPower,
};

/// The identity of the adapter that a session of `preference` selects: its description and its
/// LUID, for example "NVIDIA GeForce RTX 3080 Laptop GPU (LUID 0x000000000001A376)". Creates no
/// session. Throws `std::runtime_error` when no hardware adapter has Direct3D 12.
[[nodiscard]] auto DirectMlAdapterKey(
    DirectMlAdapterPreference preference = DirectMlAdapterPreference::HighPerformance)
    -> std::string;

/// Loads onnxruntime.dll and DirectML.dll as a session does (the folder of the executable first)
/// and checks the API version of ONNX Runtime. Creates no session. Throws `std::runtime_error`
/// with the error of the load. A later call after a success does nothing.
void                        LoadDirectMlRuntime();

/// The ONNX Runtime session of the FP16 UtNet2 graph on DirectML (Windows x64).
///
/// The constructor selects the adapter (DXGI), loads ONNX Runtime (onnxruntime.dll beside the
/// executable) and creates the session. The session creates its own Direct3D 12 device, command
/// queue, and DirectML device on that adapter and gives them to the DirectML execution provider,
/// so that the tile input and output stay in float16 DirectML buffers that it binds to the graph
/// (`Ort::IoBinding`) and reuses for each tile. One session with free height and width: graph
/// optimization level all, sequential execution, memory pattern off, the DirectML memory arena
/// on, and no CPU fallback (a node that DirectML cannot run is a load error). It checks the graph
/// against `UtNet2Spec`: the input and output names and float16 types, and the metadata values
/// `architecture`, `precision`, and `input_preparation_version`, a finite `output_scale` above 0,
/// and a `weights_sha256` of 64 hex digits.
///
/// A run converts the float32 tile input to float16 on the host into an upload buffer, copies it
/// to the input buffer on the queue, runs the graph, copies the rows of the written rectangle
/// back, and converts them to float32 times `output_scale` on the host. The host conversions run
/// on the threads of `cv::parallel_for_`.
///
/// Errors: every failure throws `std::runtime_error`. A load error names the graph path and the
/// first difference (or the ONNX Runtime or Direct3D 12 error); a run error names the tile
/// rectangle.
///
/// Thread safety: one call at a time. The owner of the session (DenoiseModelSession, DN6)
/// serializes the runs of the editor and the export.
class UtNet2DirectMlSession final : public UtNet2TileSession {
 public:
  /// Loads the graph at `graph_path` and creates the session on the adapter of `preference`.
  /// Throws on a missing file, a graph that differs from `UtNet2Spec`, a node that DirectML cannot
  /// run, no hardware adapter with Direct3D 12, or a failed load of ONNX Runtime or DirectML.
  explicit UtNet2DirectMlSession(
      std::filesystem::path     graph_path,
      DirectMlAdapterPreference preference = DirectMlAdapterPreference::HighPerformance);
  ~UtNet2DirectMlSession() override;

  UtNet2DirectMlSession(const UtNet2DirectMlSession&)                    = delete;
  auto operator=(const UtNet2DirectMlSession&) -> UtNet2DirectMlSession& = delete;
  UtNet2DirectMlSession(UtNet2DirectMlSession&&) noexcept;
  auto operator=(UtNet2DirectMlSession&&) noexcept -> UtNet2DirectMlSession&;

  /// Runs one tile and writes the rectangle of `crop` (see `UtNet2TileSession::Run`). A released
  /// session is created again first (the same checks as the constructor).
  ///
  /// Throws, with the destination rectangle unspecified, when the height or the width is not a
  /// positive multiple of 16, when the input size does not match the tile or the crop is outside
  /// the tile output or the destination, when ONNX Runtime or Direct3D 12 fails (its status
  /// text), or when an output value in the rectangle is not finite (FP16 overflow; the text names
  /// the tile rectangle and the first such value).
  void Run(std::span<const float> input, const PackedTileRect& tile,
           const TileOutputCrop& crop) override;

  /// Runs one tile and writes its whole output: `output` receives camera RGB in float32 planes
  /// (3, 2 tile.height, 2 tile.width). The errors of the crop overload.
  void Run(std::span<const float> input, const PackedTileRect& tile, std::span<float> output);

  /// True when the loaded session has run a tile of `width` x `height` packed px. DirectML
  /// compiles the graph at the first run of each input shape of a session (0.3 s for a tile of
  /// 1536 on the RTX 3080 Laptop GPU, DN3 completion record), so that run is not measured by the
  /// slow-device check.
  [[nodiscard]] auto IsReadyFor(int width, int height) const -> bool override;

  /// The local video memory of the adapter. `held_bytes` is the device memory of the largest
  /// tile size of `UtNet2Spec::kDirectMlTileMemory` that the loaded session ran (its memory
  /// arena keeps it).
  [[nodiscard]] auto DeviceMemory() const -> DeviceMemoryState override;

  /// `UtNet2Spec::kDirectMlTileMemory`.
  [[nodiscard]] auto TileMemory() const -> std::span<const UtNet2TileMemory> override;

  /// Ends the session, frees its memory arena and its buffers, and ends its Direct3D 12 and
  /// DirectML devices. The graph path, the adapter, and the scale stay; a later `Run` creates the
  /// session again.
  void               Release();

  /// True while the session exists (after the constructor or a `Run`, before `Release`).
  [[nodiscard]] auto IsLoaded() const -> bool;

  /// The `output_scale` of the graph metadata.
  [[nodiscard]] auto OutputScale() const -> float;

  [[nodiscard]] auto GraphPath() const -> const std::filesystem::path&;

  /// The description of the adapter (DXGI), for example "NVIDIA GeForce RTX 3080 Laptop GPU".
  [[nodiscard]] auto AdapterName() const -> std::string;

  /// The time on the queue of the last run from the end of the input copy to the start of the
  /// output copy (Direct3D 12 timestamps): the graph on the device and the time that ONNX
  /// Runtime takes to submit it.
  [[nodiscard]] auto LastRunDeviceTime() const -> std::chrono::nanoseconds;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace alcedo::nn
