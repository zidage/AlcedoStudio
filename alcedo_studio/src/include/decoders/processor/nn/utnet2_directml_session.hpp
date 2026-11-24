//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <memory>
#include <span>

namespace alcedo::nn {

/// The rectangle of one tile in the packed image of a photo, in packed px. `x` and `y` are the
/// origin of the tile in the photo (0 for a tile that is the whole input); the error texts of
/// `UtNet2DirectMlSession::Run` name it.
struct PackedTileRect {
  int x      = 0;
  int y      = 0;
  int width  = 0;
  int height = 0;
};

/// The ONNX Runtime session of the FP16 UtNet2 graph on DirectML (Windows x64).
///
/// The constructor loads ONNX Runtime (onnxruntime.dll beside the executable) and creates one
/// session with free height and width on the DirectML adapter of high performance: graph
/// optimization level all, sequential execution, memory pattern off, the DirectML memory arena
/// on, and no CPU fallback (a node that DirectML cannot run is a load error). It checks the graph
/// against `UtNet2Spec`: the input and output names and float16 types, and the metadata values
/// `architecture`, `precision`, and `input_preparation_version`, a finite `output_scale` above 0,
/// and a `weights_sha256` of 64 hex digits.
///
/// Errors: every failure throws `std::runtime_error`. A load error names the graph path and the
/// first difference (or the ONNX Runtime status text); a run error names the tile rectangle.
///
/// Thread safety: one call at a time. The owner of the session (DenoiseModelSession, DN6)
/// serializes the runs of the editor and the export.
class UtNet2DirectMlSession {
 public:
  /// Loads the graph at `graph_path` and creates the session. Throws on a missing file, a graph
  /// that differs from `UtNet2Spec`, a node that DirectML cannot run, or a failed load of ONNX
  /// Runtime or DirectML.
  explicit UtNet2DirectMlSession(std::filesystem::path graph_path);
  ~UtNet2DirectMlSession();

  UtNet2DirectMlSession(const UtNet2DirectMlSession&)                    = delete;
  auto operator=(const UtNet2DirectMlSession&) -> UtNet2DirectMlSession& = delete;
  UtNet2DirectMlSession(UtNet2DirectMlSession&&) noexcept;
  auto operator=(UtNet2DirectMlSession&&) noexcept -> UtNet2DirectMlSession&;

  /// Runs one tile. `input` is the packed tile in float32 planes (4, tile.height, tile.width);
  /// `output` receives camera RGB in float32 planes (3, 2 tile.height, 2 tile.width), already
  /// multiplied by `OutputScale()`. The input is converted to float16 on the host and the output
  /// back to float32. A released session is created again first (the same checks as the
  /// constructor).
  ///
  /// Throws, with `output` unspecified, when the height or the width is not a positive multiple
  /// of 16, when a span size does not match the tile, when ONNX Runtime fails (its status text),
  /// or when an output value is not finite (FP16 overflow; the text names the tile rectangle and
  /// the first such value).
  void Run(std::span<const float> input, const PackedTileRect& tile, std::span<float> output);

  /// Ends the session and frees its DirectML memory arena. The graph path and the scale stay; a
  /// later `Run` creates the session again.
  void Release();

  /// True while the session exists (after the constructor or a `Run`, before `Release`).
  [[nodiscard]] auto IsLoaded() const -> bool;

  /// The `output_scale` of the graph metadata.
  [[nodiscard]] auto OutputScale() const -> float;

  [[nodiscard]] auto GraphPath() const -> const std::filesystem::path&;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace alcedo::nn
