//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/raster_develop_params.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "edit/graph/develop_node_model.hpp"
#include "edit/runtime/raster_linearize_math.h"

namespace alcedo {
namespace {

auto InputScale(HostPixelFormat format) -> float {
  switch (format) {
    case HostPixelFormat::U8Rgba:
      return 1.0f / 255.0f;
    case HostPixelFormat::U16Rgba:
      return 1.0f / 65535.0f;
    case HostPixelFormat::F32Rgba:
      return 1.0f;
    case HostPixelFormat::U16Cfa:
      break;
  }
  throw std::invalid_argument("PackRasterLinearize: a CFA plane is not raster input");
}

/// Factor from a curve's own maximum to 1.0 = 100 nits.
auto OutputScale(RasterTransferKind kind) -> float {
  switch (kind) {
    case RasterTransferKind::St2084:
      return 10000.0f / 100.0f;
    case RasterTransferKind::Hlg:
      return 1000.0f / 100.0f;
    default:
      return 1.0f;
  }
}

}  // namespace

auto PackRasterLinearize(const RasterColorDescription& description, HostPixelFormat format)
    -> std::vector<float> {
  std::vector<float> packed(ALCEDO_RL_HEADER_SIZE, 0.0f);
  packed[ALCEDO_RL_SCENE_LINEAR] =
      description.referral_ == RasterReferral::SceneLinear ? 1.0f : 0.0f;
  bool all_hlg = true;
  for (const auto& transfer : description.transfer_) {
    all_hlg = all_hlg && transfer.kind_ == RasterTransferKind::Hlg;
  }
  packed[ALCEDO_RL_HLG_LUMINANCE] = all_hlg ? 1.0f : 0.0f;
  // One output scale for the pixel: mixed PQ/SDR channels do not occur in real files, and the
  // first channel decides.
  packed[ALCEDO_RL_OUTPUT_SCALE]  = OutputScale(description.transfer_[0].kind_);
  packed[ALCEDO_RL_INPUT_SCALE]   = InputScale(format);
  for (int channel = 0; channel < 3; ++channel) {
    const auto& transfer = description.transfer_[static_cast<std::size_t>(channel)];
    float*      c        = packed.data() + ALCEDO_RL_CHANNELS + channel * ALCEDO_RL_CHANNEL_STRIDE;
    c[0]                 = static_cast<float>(transfer.kind_);
    c[1]                 = transfer.gamma_;
    c[2]                 = static_cast<float>(transfer.icc_parametric_type_);
    for (int i = 0; i < 7; ++i) {
      c[3 + i] = transfer.icc_params_[static_cast<std::size_t>(i)];
    }
    c[10] = -1.0f;
    if (transfer.kind_ == RasterTransferKind::IccSampled) {
      if (transfer.sampled_.size() != ALCEDO_RL_SAMPLED_ENTRIES) {
        throw std::invalid_argument("PackRasterLinearize: a sampled curve needs 4096 entries");
      }
      // Reuse an equal table of an earlier channel.
      float offset = -1.0f;
      for (int earlier = 0; earlier < channel; ++earlier) {
        if (description.transfer_[static_cast<std::size_t>(earlier)].sampled_ ==
            transfer.sampled_) {
          offset = packed[ALCEDO_RL_CHANNELS + earlier * ALCEDO_RL_CHANNEL_STRIDE + 10];
        }
      }
      if (offset < 0.0f) {
        offset = static_cast<float>(packed.size());
        packed.insert(packed.end(), transfer.sampled_.begin(), transfer.sampled_.end());
      }
      packed[ALCEDO_RL_CHANNELS + channel * ALCEDO_RL_CHANNEL_STRIDE + 10] = offset;
    }
  }
  return packed;
}

auto ResolveDisplayToAp1Block(const RasterColorDescription& description) -> DisplayToAp1Block {
  DisplayToAp1Block block;
  if (description.referral_ == RasterReferral::SceneLinear) {
    block.scene_ = PackSceneLinearToAp1(description.primaries_xy_);
  } else {
    block.inverse_ =
        ResolveAces2ReferenceRuntime(description.primaries_xy_, description.peak_luminance_nits_);
  }
  return block;
}

auto MakeDisplayToAp1ArenaBlock(const DisplayToAp1Block& block) -> DisplayToAp1ArenaBlock {
  DisplayToAp1ArenaBlock arena{};
  const auto             packed = block.Packed();
  std::copy(packed.begin(), packed.end(), arena.values);
  return arena;
}

auto RequireRasterInput(const PipelineDocument& document, const char* caller)
    -> DevelopRasterInput {
  const auto* develop = document.Develop();
  if (develop == nullptr) {
    throw std::runtime_error(std::string(caller) + ": missing develop node");
  }
  auto input = develop->Params().RasterInput();
  if (!input.has_value()) {
    throw std::runtime_error(std::string(caller) +
                             ": the document has no raster input object for raster pixels");
  }
  return *input;
}

void RequireRasterPixelsMatchDescription(const PreparedRawInput&       input,
                                         const RasterColorDescription& description,
                                         const char*                   caller) {
  const bool lut_description = description.origin_ == RasterColorOrigin::IccLutConverted;
  if (lut_description != input.raster_lut_icc_converted) {
    throw std::runtime_error(
        std::string(caller) +
        (lut_description
             ? ": the description needs pixels converted by a LUT ICC profile, but the file has "
               "no such profile"
             : ": the file's pixels were converted by a LUT ICC profile, but the document "
               "description does not use it"));
  }
  if (lut_description && input.raster_icc_sha256 != description.icc_sha256_) {
    throw std::runtime_error(std::string(caller) +
                             ": the file's LUT ICC profile differs from the profile of the "
                             "document description");
  }
}

}  // namespace alcedo
