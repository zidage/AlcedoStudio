//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// Backend-neutral Bayer / X-Trans DemosaicNet topology and tile constants.
// Shared by CUDA, OpenCL, and Metal product paths so tile lists and export geometry match.

namespace alcedo {

// Hard-coded Bayer student DemosaicNet (`bayer_s24_d8`).
struct DemosaicNetBayerSpec {
  static constexpr const char* kArchitecture = "bayer_s24_d8";
  static constexpr int         kDepth        = 8;
  static constexpr int         kWidth        = 24;
  static constexpr int         kPackFactor   = 2;
  static constexpr int         kPackOutCh    = 4;   // collapse-colors 2×2
  static constexpr int         kResidualCh   = 12;  // 3 * pack_factor^2

  // Tile / CFA contract (product tiling + export goldens).
  static constexpr int kTileInput  = 1086;
  static constexpr int kTileOutput = 1024;
  static constexpr int kTileBorder = 31;  // (input - output) / 2
  static constexpr int kTilePad    = 32;  // period-aligned virtual pad (NOT 31)
  static constexpr int kTileStep   = 1024;
  static constexpr int kCfaPeriod  = 2;

  // Natural valid-conv shrink before optional export center-crop:
  //   H - 2*pack_factor*depth - 2  = H - 34
  static constexpr int kNaturalSpatialLoss = 2 * kPackFactor * kDepth + 2;
  // Export shrink on the fixed training tile (1086 → 1024).
  static constexpr int kSpatialLoss = kTileInput - kTileOutput;  // 62
  // Smallest even spatial size that yields a positive natural output.
  static constexpr int kMinSpatial = kNaturalSpatialLoss + 2;  // 36
  // Smallest owned axis for product export (P4-C strips may be 128-high).
  static constexpr int kMinProductOwned = 128;

  [[nodiscard]] static auto NaturalOutputHeight(int input_h) -> int {
    return input_h - kNaturalSpatialLoss;
  }
  [[nodiscard]] static auto NaturalOutputWidth(int input_w) -> int {
    return input_w - kNaturalSpatialLoss;
  }

  // True when both axes use product export: input = owned + 2*kTileBorder,
  // owned >= kMinProductOwned, and period-aligned (Bayer pack/CFA).
  [[nodiscard]] static auto IsProductExportInput(int input_h, int input_w) -> bool {
    const int owned_h = input_h - 2 * kTileBorder;
    const int owned_w = input_w - 2 * kTileBorder;
    if (owned_h < kMinProductOwned || owned_w < kMinProductOwned) {
      return false;
    }
    if ((owned_h % kCfaPeriod) != 0 || (owned_w % kCfaPeriod) != 0) {
      return false;
    }
    return true;
  }

  // Product export owned edge when input is a square product tile; else -1.
  [[nodiscard]] static auto ProductOwnedOutput(int input_h, int input_w) -> int {
    if (input_h != input_w || !IsProductExportInput(input_h, input_w)) {
      return -1;
    }
    return input_h - 2 * kTileBorder;
  }

  [[nodiscard]] static auto OutputHeight(int input_h, int input_w = -1) -> int {
    const int w = input_w < 0 ? input_h : input_w;
    if (IsProductExportInput(input_h, w)) {
      return input_h - 2 * kTileBorder;
    }
    return NaturalOutputHeight(input_h);
  }

  [[nodiscard]] static auto OutputWidth(int input_w, int input_h = -1) -> int {
    const int h = input_h < 0 ? input_w : input_h;
    if (IsProductExportInput(h, input_w)) {
      return input_w - 2 * kTileBorder;
    }
    return NaturalOutputWidth(input_w);
  }
};

// Hard-coded X-Trans student DemosaicNet (`xtrans_p2_s32_d4`).
struct DemosaicNetXTransSpec {
  static constexpr const char* kArchitecture = "xtrans_p2_s32_d4";
  static constexpr int         kDepth        = 4;
  static constexpr int         kWidth        = 32;
  static constexpr int         kPackFactor   = 2;
  static constexpr int         kPackOutCh    = 12;  // space-to-depth 3*2*2
  static constexpr int         kResidualCh   = 12;

  // Tile / CFA contract (product tiling + export goldens).
  static constexpr int kTileInput  = 1048;
  static constexpr int kTileOutput = 1024;
  static constexpr int kTileBorder = 12;
  static constexpr int kTilePad    = 12;
  static constexpr int kTileStep   = 1020;  // 1024 % 6 != 0 → period-safe step
  static constexpr int kCfaPeriod  = 6;

  // Natural valid-conv shrink: H - 2*pack_factor*depth - 2 = H - 18
  static constexpr int kNaturalSpatialLoss = 2 * kPackFactor * kDepth + 2;
  // Export shrink on the fixed training tile (1048 → 1024).
  static constexpr int kSpatialLoss = kTileInput - kTileOutput;  // 24
  static constexpr int kMinSpatial  = kNaturalSpatialLoss + 2;   // 20
  static constexpr int kMinProductOwned = 128;

  [[nodiscard]] static auto NaturalOutputHeight(int input_h) -> int {
    return input_h - kNaturalSpatialLoss;
  }
  [[nodiscard]] static auto NaturalOutputWidth(int input_w) -> int {
    return input_w - kNaturalSpatialLoss;
  }

  // True when both axes use product export: input = owned + 2*kTileBorder.
  // X-Trans step period safety is enforced by the tile planner, not export crop.
  [[nodiscard]] static auto IsProductExportInput(int input_h, int input_w) -> bool {
    const int owned_h = input_h - 2 * kTileBorder;
    const int owned_w = input_w - 2 * kTileBorder;
    return owned_h >= kMinProductOwned && owned_w >= kMinProductOwned;
  }

  [[nodiscard]] static auto ProductOwnedOutput(int input_h, int input_w) -> int {
    if (input_h != input_w || !IsProductExportInput(input_h, input_w)) {
      return -1;
    }
    return input_h - 2 * kTileBorder;
  }

  [[nodiscard]] static auto OutputHeight(int input_h, int input_w = -1) -> int {
    const int w = input_w < 0 ? input_h : input_w;
    if (IsProductExportInput(input_h, w)) {
      return input_h - 2 * kTileBorder;
    }
    return NaturalOutputHeight(input_h);
  }

  [[nodiscard]] static auto OutputWidth(int input_w, int input_h = -1) -> int {
    const int h = input_h < 0 ? input_w : input_h;
    if (IsProductExportInput(h, input_w)) {
      return input_w - 2 * kTileBorder;
    }
    return NaturalOutputWidth(input_w);
  }
};

}  // namespace alcedo
