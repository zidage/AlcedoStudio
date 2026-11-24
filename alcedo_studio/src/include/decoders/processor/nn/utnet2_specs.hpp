//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <string_view>

// The contract of the FP16 ONNX graph of the UtNet2 Bayer denoise model of AI Denoise
// (alcedo-denoise-lab, scripts/export/export_fp16_onnx.py; docs/alcedo_integration.md of the
// lab). The platform sessions (UtNet2DirectMlSession on Windows) check a graph against these
// values when they load it.

namespace alcedo::nn {

struct UtNet2Spec {
  // Graph input `input`: float16 (1, 4, height, width), the packed RGGB mosaic.
  static constexpr std::string_view kInputName     = "input";
  // Graph output `output`: float16 (1, 3, 2 height, 2 width), camera RGB divided by the
  // `output_scale` of the graph metadata.
  static constexpr std::string_view kOutputName    = "output";
  static constexpr int              kInputChannels  = 4;
  static constexpr int              kOutputChannels = 3;
  // Each packed pixel gives 2 x 2 output pixels (PixelShuffle(2) at the end of the graph).
  static constexpr int              kOutputUpscale  = 2;
  // 4 max pooling layers 2 x 2: the packed height and width are multiples of 16.
  static constexpr int              kSizeAlignment  = 16;

  // Tile sizes in packed px, largest first, and the overlap that gives the one-pass output
  // (integration plan, DN1 and section 9).
  static constexpr std::array<int, 3> kTileSizes  = {1536, 1024, 768};
  static constexpr int                kTileOverlap = 96;

  // Metadata of the graph (`metadata_props`) and the values that a session requires.
  static constexpr std::string_view kMetadataArchitecture             = "architecture";
  static constexpr std::string_view kArchitecture                     = "utnet2_bayer_funit32";
  static constexpr std::string_view kMetadataPrecision                = "precision";
  static constexpr std::string_view kPrecision                        = "float16";
  static constexpr std::string_view kMetadataInputPreparationVersion  = "input_preparation_version";
  static constexpr std::string_view kInputPreparationVersion          = "1";
  // A finite value above 0: the float32 output is the graph output times this value.
  static constexpr std::string_view kMetadataOutputScale              = "output_scale";
  // The SHA-256 (64 lower-case hex digits) of the safetensors weights that the graph comes from.
  static constexpr std::string_view kMetadataWeightsSha256            = "weights_sha256";
};

}  // namespace alcedo::nn
