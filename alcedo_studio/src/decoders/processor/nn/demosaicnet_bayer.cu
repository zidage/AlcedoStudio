//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/nn/demosaicnet_bayer.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cuda/nn/common.hpp"
#include "cuda/nn/conv2d.hpp"
#include "cuda/nn/demosaicnet_nhwc.hpp"
#include "cuda/nn/fused_post_output.hpp"
#include "cuda/nn/layout.hpp"
#include "decoders/processor/nn/demosaicnet_activation_slots.hpp"
#include "decoders/processor/nn/demosaicnet_profiler.hpp"

namespace alcedo {
namespace {

using cuda::nn::Conv2d;
using cuda::nn::Conv2dBiasRelu;
using cuda::nn::Conv2dParams;
using cuda::nn::DeviceTensor;
using cuda::nn::WorkspacePool;

[[nodiscard]] auto ViewNchw(float* base, std::int64_t n, std::int64_t c, std::int64_t h,
                            std::int64_t w) -> DeviceTensor {
  return DeviceTensor::Contiguous(base, {n, c, h, w});
}

[[nodiscard]] auto ViewNhwc(float* base, std::int64_t n, std::int64_t h, std::int64_t w,
                            std::int64_t c) -> DeviceTensor {
  return DeviceTensor::Contiguous(base, {n, h, w, c});
}

void RequireContiguousNchw3(const DeviceTensor& t, const char* what) {
  if (t.rank != 4 || t.data == nullptr) {
    throw std::runtime_error(std::string(what) + ": expected rank-4 NCHW tensor");
  }
  if (t.shape[1] != 3) {
    throw std::runtime_error(std::string(what) + ": expected 3 channels");
  }
  if (!t.IsContiguous()) {
    throw std::runtime_error(std::string(what) + ": expected contiguous NCHW");
  }
}

void RequireMetadata(const cuda::nn::SafetensorsTensorMap& tensors, std::string_view key,
                     std::string_view expected) {
  const auto actual = tensors.metadata(key);
  if (actual != expected) {
    throw std::runtime_error("BayerDemosaicNet: metadata '" + std::string(key) + "' expected '" +
                             std::string(expected) + "', got '" + std::string(actual) + "'");
  }
}

// Fixed collapse-colors pack: out_i = py*2+px sums all input colors at that sub-pixel.
[[nodiscard]] auto ExpectedBayerPackWeight() -> std::vector<float> {
  // shape [4, 3, 2, 2]
  std::vector<float> w(4 * 3 * 2 * 2, 0.0f);
  for (int py = 0; py < 2; ++py) {
    for (int px = 0; px < 2; ++px) {
      const int out_i = py * 2 + px;
      for (int c = 0; c < 3; ++c) {
        // layout [Cout, Cin, kH, kW]
        w[((out_i * 3 + c) * 2 + py) * 2 + px] = 1.0f;
      }
    }
  }
  return w;
}

// Fixed grouped unpack: residual channels g*4:(g+1)*4 → RGB sub-pixels.
[[nodiscard]] auto ExpectedUnpackWeight() -> std::vector<float> {
  // shape [12, 1, 2, 2]
  std::vector<float> w(12 * 1 * 2 * 2, 0.0f);
  for (int g = 0; g < 3; ++g) {
    for (int py = 0; py < 2; ++py) {
      for (int px = 0; px < 2; ++px) {
        const int in_i = g * 4 + py * 2 + px;
        w[(in_i * 2 + py) * 2 + px] = 1.0f;
      }
    }
  }
  return w;
}

void RequireExactHostWeight(const cuda::nn::SafetensorsTensor& host,
                            const std::vector<float>& expected, std::string_view key) {
  if (host.data.size() != expected.size()) {
    throw std::runtime_error("BayerDemosaicNet: fixed weight size mismatch for " +
                             std::string(key));
  }
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (std::fabs(host.data[i] - expected[i]) > 0.0f) {
      throw std::runtime_error("BayerDemosaicNet: fixed one-hot mismatch for " + std::string(key) +
                               " at index " + std::to_string(i));
    }
  }
}

}  // namespace

void BayerDemosaicNet::LoadWeightsImpl(const cuda::nn::SafetensorsTensorMap& tensors,
                                       cudaStream_t stream) {
  // Full student identity (Phase 8A): reject teacher/other architectures.
  RequireMetadata(tensors, "format", "demosaicnet-pytorch-state_dict");
  RequireMetadata(tensors, "architecture", kArchitecture);
  RequireMetadata(tensors, "architecture_version", "1");
  RequireMetadata(tensors, "variant", "bayer");
  RequireMetadata(tensors, "cfa_period", "2");
  RequireMetadata(tensors, "pack_factor", "2");
  RequireMetadata(tensors, "tile_input", "1086");
  RequireMetadata(tensors, "tile_output", "1024");
  RequireMetadata(tensors, "tile_border", "31");
  RequireMetadata(tensors, "tile_pad", "32");
  RequireMetadata(tensors, "tile_step", "1024");
  RequireMetadata(tensors, "checkpoint_sha256",
                  "f00fb0e4f4a49e32344ffb0add583bee98c7d5dbfda6c593b5b066d08f9de69f");

  // Fixed pack (bias-free). Validate unpack structural one-hots without storing them —
  // the NHWC path fuses unpack into DemosaicNetUnpackCropConcatNhwc.
  {
    const auto& pack =
        cuda::nn::RequireF32Tensor(tensors, "pack.weight", {kPackOutCh, 3, kPackFactor, kPackFactor});
    RequireExactHostWeight(pack, ExpectedBayerPackWeight(), "pack.weight");
    pack_w_ = cuda::nn::UploadToDevice(pack, stream);
  }
  {
    const auto& unpack =
        cuda::nn::RequireF32Tensor(tensors, "unpack.weight", {kResidualCh, 1, kPackFactor, kPackFactor});
    RequireExactHostWeight(unpack, ExpectedUnpackWeight(), "unpack.weight");
  }

  // Trunk: first 4→24 NCHW; square 24→24 layers derive NHWC CKCO at load time.
  RequireUpload(tensors, "trunk.0.weight", {kWidth, kPackOutCh, 3, 3}, trunk0_w_, stream);
  RequireUpload(tensors, "trunk.0.bias", {kWidth}, trunk_b_[0], stream);
  for (int i = 1; i < kDepth; ++i) {
    const std::string wk = "trunk." + std::to_string(i) + ".weight";
    const std::string bk = "trunk." + std::to_string(i) + ".bias";
    const auto& weight = cuda::nn::RequireF32Tensor(tensors, wk, {kWidth, kWidth, 3, 3});
    std::vector<float> ckco(weight.data.size());
    cuda::nn::TransformConv2d3x3WeightsNhwc(weight.data.data(), kWidth, kWidth, ckco.data());
    trunk_w_nhwc_[i] = cuda::nn::DeviceBufferF32(ckco.size());
    trunk_w_nhwc_[i].Upload(ckco, stream);
    RequireUpload(tensors, bk, {kWidth}, trunk_b_[i], stream);
  }

  {
    const auto& weight = cuda::nn::RequireF32Tensor(
        tensors, "residual.weight", {kResidualCh, kWidth, 1, 1});
    std::vector<float> cio(weight.data.size());
    cuda::nn::TransformDemosaicNetResidualWeightsNhwc(weight.data.data(), kWidth, cio.data());
    residual_w_nhwc_ = cuda::nn::DeviceBufferF32(cio.size());
    residual_w_nhwc_.Upload(cio, stream);
  }
  RequireUpload(tensors, "residual.bias", {kResidualCh}, residual_b_, stream);
  RequireUpload(tensors, "post_conv.weight", {kWidth, 6, 3, 3}, post_w_, stream);
  RequireUpload(tensors, "post_conv.bias", {kWidth}, post_b_, stream);
  RequireUpload(tensors, "output.bias", {3}, output_b_, stream);
  {
    const auto& out_w =
        cuda::nn::RequireF32Tensor(tensors, "output.weight", {3, kWidth, 1, 1});
    std::vector<float> cio(static_cast<std::size_t>(kWidth) * 3);
    cuda::nn::PrepackOutputWeightsCio(out_w.data.data(), kWidth, cio.data());
    output_w_cio_ = cuda::nn::DeviceBufferF32(cio.size());
    output_w_cio_.Upload(cio, stream);
  }

  if (stream != nullptr) {
    cuda::nn::CheckCuda(::cudaStreamSynchronize(stream), "BayerDemosaicNet::LoadWeightsImpl sync");
  }
}

auto BayerDemosaicNet::ResidentWeightBytes() const -> std::size_t {
  std::size_t total = 0;
  auto        add   = [&](const cuda::nn::DeviceBufferF32& b) { total += b.bytes(); };
  add(pack_w_);
  add(trunk0_w_);
  for (int i = 0; i < kDepth; ++i) {
    add(trunk_w_nhwc_[i]);
    add(trunk_b_[i]);
  }
  add(residual_w_nhwc_);
  add(residual_b_);
  add(post_w_);
  add(post_b_);
  add(output_w_cio_);
  add(output_b_);
  return total;
}

auto BayerDemosaicNet::EstimateWorkspaceBytes(int input_h, int input_w, int batch) -> std::size_t {
  if (batch < 1 || input_h < kMinSpatial || input_w < kMinSpatial) {
    return 0;
  }
  if ((input_h % kPackFactor) != 0 || (input_w % kPackFactor) != 0) {
    return 0;
  }
  return demosaicnet_slots::ComputePeakLiveSlots(input_h, input_w, batch, kPackOutCh, kWidth,
                                                 kResidualCh, kDepth, kPackFactor,
                                                 /*fuse_post_output=*/true)
      .estimate_bytes;
}

void BayerDemosaicNet::ForwardHwcChannelsLast(
    const DeviceTensor& input, float* rgb_hwc, const std::size_t rgb_step_bytes,
    WorkspacePool& workspace, cudaStream_t stream, const bool apply_gamma_decode) const {
  if (!weights_loaded() || output_w_cio_.get() == nullptr) {
    throw std::runtime_error("BayerDemosaicNet::ForwardHwcChannelsLast: weights not loaded");
  }
  if (rgb_hwc == nullptr) {
    throw std::runtime_error("BayerDemosaicNet::ForwardHwcChannelsLast: null rgb pointer");
  }
  RequireContiguousNchw3(input, "BayerDemosaicNet::ForwardHwcChannelsLast input");

  const int N = static_cast<int>(input.shape[0]);
  const int H = static_cast<int>(input.shape[2]);
  const int W = static_cast<int>(input.shape[3]);
  if (N != 1 || (H % kPackFactor) != 0 || (W % kPackFactor) != 0 || H < kMinSpatial ||
      W < kMinSpatial) {
    throw std::runtime_error("BayerDemosaicNet::ForwardHwcChannelsLast: invalid input geometry");
  }

  const int out_h = OutputHeight(H, W);
  const int out_w = OutputWidth(W, H);
  const demosaicnet_slots::PeakLiveSlots slots = demosaicnet_slots::ComputePeakLiveSlots(
      H, W, N, kPackOutCh, kWidth, kResidualCh, kDepth, kPackFactor,
      /*fuse_post_output=*/true);
  if (slots.estimate_bytes == 0) {
    throw std::runtime_error(
        "BayerDemosaicNet::ForwardHwcChannelsLast: invalid activation geometry");
  }

  workspace.Reset();
  if (workspace.capacity_bytes() < slots.estimate_bytes) {
    workspace.Reserve(slots.estimate_bytes);
  }
  float* slot_a = static_cast<float*>(workspace.Allocate(slots.trunk_slot_bytes));
  float* slot_b = static_cast<float*>(workspace.Allocate(slots.trunk_slot_bytes));
  if (slot_a == nullptr || slot_b == nullptr) {
    throw std::runtime_error(
        "BayerDemosaicNet::ForwardHwcChannelsLast: failed to allocate activation slots");
  }

  DemosaicNetProfiler* profiler = ActiveDemosaicNetProfiler();
  const std::int64_t n64 = N;
  const int ph = static_cast<int>(slots.pack_h);
  const int pw = static_cast<int>(slots.pack_w);

  DeviceTensor packed = ViewNchw(slot_a, n64, kPackOutCh, ph, pw);
  if (profiler != nullptr) {
    profiler->BeginRange(DemosaicNetProfileRange::PackConv, stream);
  }
  {
    Conv2dParams p;
    p.in_channels  = 3;
    p.out_channels = kPackOutCh;
    p.kH = p.kW = kPackFactor;
    p.sH = p.sW = kPackFactor;
    p.weight    = pack_w_.get();
    Conv2d(input, packed, p, stream, &workspace);
  }
  if (profiler != nullptr) {
    profiler->EndRange(DemosaicNetProfileRange::PackConv, stream);
  }

  // The unequal-width first trunk remains on the retained NCHW direct kernel.
  // Its dead packed-input slot is the destination of the single NCHW→NHWC conversion.
  int cur_h = ph - 2;
  int cur_w = pw - 2;
  DeviceTensor first_nchw = ViewNchw(slot_b, n64, kWidth, cur_h, cur_w);
  if (profiler != nullptr) {
    profiler->BeginTrunkLayer(0, stream);
  }
  {
    Conv2dParams p;
    p.in_channels  = kPackOutCh;
    p.out_channels = kWidth;
    p.kH = p.kW = 3;
    p.sH = p.sW = 1;
    p.weight    = trunk0_w_.get();
    p.bias      = trunk_b_[0].get();
    Conv2dBiasRelu(packed, first_nchw, p, stream, &workspace);
  }
  DeviceTensor first_nhwc = ViewNhwc(slot_a, n64, cur_h, cur_w, kWidth);
  cuda::nn::UnpackNchwToHwc(first_nchw, first_nhwc, stream);
  if (profiler != nullptr) {
    profiler->EndTrunkLayer(0, stream);
  }

  float* cur_base  = slot_a;
  float* next_base = slot_b;
  for (int i = 1; i < kDepth; ++i) {
    const int next_h = cur_h - 2;
    const int next_w = cur_w - 2;
    if (profiler != nullptr) {
      profiler->BeginTrunkLayer(i, stream);
    }
    cuda::nn::Conv2d3x3NhwcBiasRelu(cur_base, next_base, trunk_w_nhwc_[i].get(),
                                    trunk_b_[i].get(), N, cur_h, cur_w, kWidth, stream);
    if (profiler != nullptr) {
      profiler->EndTrunkLayer(i, stream);
    }
    std::swap(cur_base, next_base);
    cur_h = next_h;
    cur_w = next_w;
  }

  const int up_h = cur_h * kPackFactor;
  const int up_w = cur_w * kPackFactor;
  const std::size_t cat_bytes =
      static_cast<std::size_t>(N) * up_h * up_w * 6 * sizeof(float);
  if (cat_bytes > slots.trunk_slot_bytes) {
    throw std::runtime_error(
        "BayerDemosaicNet::ForwardHwcChannelsLast: concat exceeds trunk slot");
  }

  if (profiler != nullptr) {
    profiler->BeginRange(DemosaicNetProfileRange::ResidualUnpackCropConcat, stream);
  }
  cuda::nn::DemosaicNetResidual1x1Nhwc(cur_base, next_base, residual_w_nhwc_.get(),
                                       residual_b_.get(), N, cur_h, cur_w, kWidth, stream);
  // residual is now in next_base; cur_base is dead and becomes the NHWC concat output.
  cuda::nn::DemosaicNetUnpackCropConcatNhwc(input.data, H, W, next_base, cur_h, cur_w, cur_base, N,
                                            stream);
  if (profiler != nullptr) {
    profiler->EndRange(DemosaicNetProfileRange::ResidualUnpackCropConcat, stream);
    profiler->BeginRange(DemosaicNetProfileRange::PostOutput, stream);
  }

  cuda::nn::FusedPostOutputParams fp;
  fp.post_channels      = kWidth;
  fp.post_weight        = post_w_.get();
  fp.post_bias          = post_b_.get();
  fp.output_weight_cio  = output_w_cio_.get();
  fp.output_bias        = output_b_.get();
  fp.apply_gamma_decode = apply_gamma_decode;
  cuda::nn::FusedPostOutputNhwcToHwc(cur_base, N, up_h, up_w, rgb_hwc, rgb_step_bytes, out_h,
                                     out_w, fp, stream);
  if (profiler != nullptr) {
    profiler->EndRange(DemosaicNetProfileRange::PostOutput, stream);
  }
}

}  // namespace alcedo
