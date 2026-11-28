//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// The specification of the FP16 ONNX graph of the UtNet2 Bayer denoise model of AI Denoise
// (alcedo-denoise-lab, scripts/export/export_fp16_onnx.py; docs/alcedo_integration.md of the
// lab). The platform sessions (UtNet2DirectMlSession on Windows) check a graph against these
// values when they load it.

namespace alcedo::nn {

/// The device memory that a run with tiles of `tile_size` packed px needs.
struct UtNet2TileMemory {
  int           tile_size    = 0;
  std::uint64_t device_bytes = 0;
};

struct UtNet2Spec {
  // Graph input `input`: float16 (1, 4, height, width), the packed RGGB mosaic.
  static constexpr std::string_view                kInputName                          = "input";
  // Graph output `output`: float16 (1, 3, 2 height, 2 width), camera RGB divided by the
  // `output_scale` of the graph metadata.
  static constexpr std::string_view                kOutputName                         = "output";
  static constexpr int                             kInputChannels                      = 4;
  static constexpr int                             kOutputChannels                     = 3;
  // Each packed pixel gives 2 x 2 output pixels (PixelShuffle(2) at the end of the graph).
  static constexpr int                             kOutputUpscale                      = 2;
  // 4 max pooling layers 2 x 2: the packed height and width are multiples of 16.
  static constexpr int                             kSizeAlignment                      = 16;

  // The overlap in packed px that gives the one-pass output (integration plan, DN1 and
  // section 9).
  static constexpr int                             kTileOverlap                        = 96;

  // The device memory of a run with tiles of each size on DirectML, largest first: the growth
  // of the local video memory of the process (DXGI `CurrentUsage`) from before the session to
  // after the runs with that tile size, rounded up to 16 MiB. RTX 3080 Laptop GPU (integration
  // plan, DN3 completion record): 1536 after the 24.3, 36.2, and 45.4 MP photos 2699 MiB; 1024 and
  // 768 after the 45.4 MP photo 753 and 730 MiB.
  static constexpr std::array<UtNet2TileMemory, 3> kDirectMlTileMemory                 = {{
      {.tile_size = 1536, .device_bytes = std::uint64_t{2704} << 20},
      {.tile_size = 1024, .device_bytes = std::uint64_t{768} << 20},
      {.tile_size = 768, .device_bytes = std::uint64_t{736} << 20},
  }};

  // The memory of a run with tiles of each size on the Neural Engine (Core ML, one session for
  // each tile shape), largest first: the growth of the physical footprint of the process from
  // the photo and its output image in memory without a session to after the runs of the 45.4 MP
  // photo (4 tile shapes), rounded up to 16 MiB. MacBook Air M4 (integration plan, DN10
  // completion record): 402, 146, and 108 MiB.
  static constexpr std::array<UtNet2TileMemory, 3> kCoreMlTileMemory                   = {{
      {.tile_size = 1536, .device_bytes = std::uint64_t{416} << 20},
      {.tile_size = 1024, .device_bytes = std::uint64_t{160} << 20},
      {.tile_size = 768, .device_bytes = std::uint64_t{112} << 20},
  }};

  // `slow_device_limit` (integration plan, 2.4 and section 9): the largest projected time of a
  // photo for each megapixel of its mosaic. About 3 times `target_time` (RTX 3080 Laptop GPU
  // 0.016 s, M4 Neural Engine 0.013 s, Intel Iris Xe 0.114 s for each megapixel).
  static constexpr double                          kSlowDeviceLimitSecondsPerMegapixel = 0.05;
  // The slow-device check projects the time of a photo from the fastest of its measured tiles,
  // and it starts after this count of measured tiles (or after the last tile). Under the load of
  // another process, single tiles of the RTX 3080 Laptop GPU took 5 times their time (issue
  // #1043, integration plan DN6); the Intel Iris Xe is slow in each tile.
  static constexpr int                             kSlowDeviceMeasuredTiles            = 3;

  // Metadata of the graph (`metadata_props`) and the values that a session requires.
  static constexpr std::string_view                kMetadataArchitecture = "architecture";
  static constexpr std::string_view                kArchitecture         = "utnet2_bayer_funit32";
  static constexpr std::string_view                kMetadataPrecision    = "precision";
  static constexpr std::string_view                kPrecision            = "float16";
  static constexpr std::string_view kMetadataInputPreparationVersion = "input_preparation_version";
  static constexpr std::string_view kInputPreparationVersion         = "1";
  // A finite value above 0: the float32 output is the graph output times this value.
  static constexpr std::string_view kMetadataOutputScale             = "output_scale";
  // The SHA-256 (64 lower-case hex digits) of the safetensors weights that the graph comes from.
  static constexpr std::string_view kMetadataWeightsSha256           = "weights_sha256";
};

}  // namespace alcedo::nn
