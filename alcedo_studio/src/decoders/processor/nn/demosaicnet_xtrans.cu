//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/nn/demosaicnet_xtrans.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cuda/nn/common.hpp"
#include "cuda/nn/concat.hpp"
#include "cuda/nn/conv2d.hpp"
#include "cuda/nn/conv_transpose2d.hpp"
#include "cuda/nn/crop.hpp"

namespace alcedo {
namespace {

using cuda::nn::CenterCropLike;
using cuda::nn::CenterCropSpatial;
using cuda::nn::ConcatChannels;
using cuda::nn::Conv2d;
using cuda::nn::Conv2dBiasRelu;
using cuda::nn::Conv2dParams;
using cuda::nn::ConvTranspose2d;
using cuda::nn::ConvTranspose2dParams;
using cuda::nn::DeviceTensor;
using cuda::nn::WorkspacePool;

[[nodiscard]] auto AlignUp(std::size_t value, std::size_t alignment) -> std::size_t {
  return (value + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] auto TensorBytes(std::int64_t n, std::int64_t c, std::int64_t h, std::int64_t w)
    -> std::size_t {
  if (n <= 0 || c <= 0 || h <= 0 || w <= 0) {
    return 0;
  }
  const std::size_t elems =
      static_cast<std::size_t>(n) * static_cast<std::size_t>(c) * static_cast<std::size_t>(h) *
      static_cast<std::size_t>(w);
  return AlignUp(elems * sizeof(float), WorkspacePool::kDefaultAlignment);
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
    throw std::runtime_error("XTransDemosaicNet: metadata '" + std::string(key) + "' expected '" +
                             std::string(expected) + "', got '" + std::string(actual) + "'");
  }
}

// Fixed space-to-depth pack: out_i = c*4 + py*2 + px.
[[nodiscard]] auto ExpectedXTransPackWeight() -> std::vector<float> {
  // shape [12, 3, 2, 2]
  std::vector<float> w(12 * 3 * 2 * 2, 0.0f);
  for (int c = 0; c < 3; ++c) {
    for (int py = 0; py < 2; ++py) {
      for (int px = 0; px < 2; ++px) {
        const int out_i = c * 4 + py * 2 + px;
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
    throw std::runtime_error("XTransDemosaicNet: fixed weight size mismatch for " +
                             std::string(key));
  }
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (std::fabs(host.data[i] - expected[i]) > 0.0f) {
      throw std::runtime_error("XTransDemosaicNet: fixed one-hot mismatch for " + std::string(key) +
                               " at index " + std::to_string(i));
    }
  }
}

}  // namespace

void XTransDemosaicNet::LoadWeightsImpl(const cuda::nn::SafetensorsTensorMap& tensors,
                                        cudaStream_t stream) {
  RequireMetadata(tensors, "format", "demosaicnet-pytorch-state_dict");
  RequireMetadata(tensors, "architecture", kArchitecture);
  RequireMetadata(tensors, "architecture_version", "1");
  RequireMetadata(tensors, "variant", "xtrans");
  RequireMetadata(tensors, "cfa_period", "6");
  RequireMetadata(tensors, "pack_factor", "2");
  RequireMetadata(tensors, "tile_input", "1048");
  RequireMetadata(tensors, "tile_output", "1024");
  RequireMetadata(tensors, "tile_border", "12");
  RequireMetadata(tensors, "tile_pad", "12");
  RequireMetadata(tensors, "tile_step", "1020");
  RequireMetadata(tensors, "checkpoint_sha256",
                  "f985ba64404a4ef9e4662d4f556d184de1e47127ab046f7140fa4b614f4c7546");

  {
    const auto& pack =
        cuda::nn::RequireF32Tensor(tensors, "pack.weight", {kPackOutCh, 3, kPackFactor, kPackFactor});
    RequireExactHostWeight(pack, ExpectedXTransPackWeight(), "pack.weight");
    pack_w_ = cuda::nn::UploadToDevice(pack, stream);
  }
  {
    const auto& unpack =
        cuda::nn::RequireF32Tensor(tensors, "unpack.weight", {kResidualCh, 1, kPackFactor, kPackFactor});
    RequireExactHostWeight(unpack, ExpectedUnpackWeight(), "unpack.weight");
    unpack_w_ = cuda::nn::UploadToDevice(unpack, stream);
  }

  RequireUpload(tensors, "trunk.0.weight", {kWidth, kPackOutCh, 3, 3}, trunk_w_[0], stream);
  RequireUpload(tensors, "trunk.0.bias", {kWidth}, trunk_b_[0], stream);
  for (int i = 1; i < kDepth; ++i) {
    const std::string wk = "trunk." + std::to_string(i) + ".weight";
    const std::string bk = "trunk." + std::to_string(i) + ".bias";
    RequireUpload(tensors, wk, {kWidth, kWidth, 3, 3}, trunk_w_[i], stream);
    RequireUpload(tensors, bk, {kWidth}, trunk_b_[i], stream);
  }

  RequireUpload(tensors, "residual.weight", {kResidualCh, kWidth, 1, 1}, residual_w_, stream);
  RequireUpload(tensors, "residual.bias", {kResidualCh}, residual_b_, stream);
  RequireUpload(tensors, "post_conv.weight", {kWidth, 6, 3, 3}, post_w_, stream);
  RequireUpload(tensors, "post_conv.bias", {kWidth}, post_b_, stream);
  RequireUpload(tensors, "output.weight", {3, kWidth, 1, 1}, output_w_, stream);
  RequireUpload(tensors, "output.bias", {3}, output_b_, stream);

  if (stream != nullptr) {
    cuda::nn::CheckCuda(::cudaStreamSynchronize(stream), "XTransDemosaicNet::LoadWeightsImpl sync");
  }
}

auto XTransDemosaicNet::ResidentWeightBytes() const -> std::size_t {
  std::size_t total = 0;
  auto        add   = [&](const cuda::nn::DeviceBufferF32& b) { total += b.bytes(); };
  add(pack_w_);
  for (int i = 0; i < kDepth; ++i) {
    add(trunk_w_[i]);
    add(trunk_b_[i]);
  }
  add(residual_w_);
  add(residual_b_);
  add(unpack_w_);
  add(post_w_);
  add(post_b_);
  add(output_w_);
  add(output_b_);
  return total;
}

auto XTransDemosaicNet::EstimateWorkspaceBytes(int input_h, int input_w, int batch) -> std::size_t {
  if (batch < 1 || input_h < kMinSpatial || input_w < kMinSpatial) {
    return 0;
  }
  if ((input_h % kPackFactor) != 0 || (input_w % kPackFactor) != 0) {
    return 0;
  }

  const std::int64_t N     = batch;
  std::size_t        total = 0;

  const std::int64_t ph = input_h / kPackFactor;
  const std::int64_t pw = input_w / kPackFactor;
  total += TensorBytes(N, kPackOutCh, ph, pw);

  std::int64_t ch = ph;
  std::int64_t cw = pw;
  for (int i = 0; i < kDepth; ++i) {
    const std::int64_t oh = ch - 2;
    const std::int64_t ow = cw - 2;
    total += TensorBytes(N, kWidth, oh, ow);
    ch = oh;
    cw = ow;
  }
  total += TensorBytes(N, kResidualCh, ch, cw);
  const std::int64_t uh = ch * kPackFactor;
  const std::int64_t uw = cw * kPackFactor;
  total += TensorBytes(N, 3, uh, uw);
  total += TensorBytes(N, 3, uh, uw);
  total += TensorBytes(N, 6, uh, uw);
  const std::int64_t natural_h = uh - 2;
  const std::int64_t natural_w = uw - 2;
  total += TensorBytes(N, kWidth, natural_h, natural_w);
  total += TensorBytes(N, 3, natural_h, natural_w);

  return total + (256 * 1024);
}

void XTransDemosaicNet::Forward(const DeviceTensor& input, DeviceTensor& output,
                                WorkspacePool& workspace, cudaStream_t stream) const {
  if (!weights_loaded()) {
    throw std::runtime_error("XTransDemosaicNet::Forward: weights not loaded");
  }
  RequireContiguousNchw3(input, "XTransDemosaicNet::Forward input");
  RequireContiguousNchw3(output, "XTransDemosaicNet::Forward output");

  const int N = static_cast<int>(input.shape[0]);
  const int H = static_cast<int>(input.shape[2]);
  const int W = static_cast<int>(input.shape[3]);
  if (N < 1) {
    throw std::runtime_error("XTransDemosaicNet::Forward: invalid batch");
  }
  if ((H % kPackFactor) != 0 || (W % kPackFactor) != 0) {
    throw std::runtime_error("XTransDemosaicNet::Forward: H and W must be divisible by pack factor");
  }
  if (H < kMinSpatial || W < kMinSpatial) {
    throw std::runtime_error("XTransDemosaicNet::Forward: spatial size below minimum");
  }

  const int out_h = OutputHeight(H, W);
  const int out_w = OutputWidth(W, H);
  if (output.shape[0] != N || output.shape[2] != out_h || output.shape[3] != out_w) {
    throw std::runtime_error("XTransDemosaicNet::Forward: output shape mismatch");
  }

  workspace.Reset();
  const std::size_t need = EstimateWorkspaceBytes(H, W, N);
  if (workspace.capacity_bytes() < need) {
    workspace.Reserve(need);
  }

  const int ph = H / kPackFactor;
  const int pw = W / kPackFactor;
  DeviceTensor cur = workspace.AllocateTensor(
      {static_cast<std::int64_t>(N), kPackOutCh, static_cast<std::int64_t>(ph),
       static_cast<std::int64_t>(pw)});
  {
    Conv2dParams p;
    p.in_channels  = 3;
    p.out_channels = kPackOutCh;
    p.kH = p.kW = kPackFactor;
    p.sH = p.sW = kPackFactor;
    p.weight    = pack_w_.get();
    p.bias      = nullptr;
    Conv2d(input, cur, p, stream, &workspace);
  }

  for (int i = 0; i < kDepth; ++i) {
    const int cin = static_cast<int>(cur.shape[1]);
    const int oh  = static_cast<int>(cur.shape[2]) - 2;
    const int ow  = static_cast<int>(cur.shape[3]) - 2;
    if (oh < 1 || ow < 1) {
      throw std::runtime_error("XTransDemosaicNet::Forward: spatial collapsed in trunk");
    }
    DeviceTensor next = workspace.AllocateTensor({static_cast<std::int64_t>(N), kWidth,
                                                  static_cast<std::int64_t>(oh),
                                                  static_cast<std::int64_t>(ow)});
    Conv2dParams p;
    p.in_channels  = cin;
    p.out_channels = kWidth;
    p.kH = p.kW = 3;
    p.sH = p.sW = 1;
    p.weight    = trunk_w_[i].get();
    p.bias      = trunk_b_[i].get();
    Conv2dBiasRelu(cur, next, p, stream, &workspace);
    cur = next;
  }

  const int mh = static_cast<int>(cur.shape[2]);
  const int mw = static_cast<int>(cur.shape[3]);
  DeviceTensor residual = workspace.AllocateTensor(
      {static_cast<std::int64_t>(N), kResidualCh, static_cast<std::int64_t>(mh),
       static_cast<std::int64_t>(mw)});
  {
    Conv2dParams p;
    p.in_channels  = kWidth;
    p.out_channels = kResidualCh;
    p.kH = p.kW = 1;
    p.sH = p.sW = 1;
    p.weight    = residual_w_.get();
    p.bias      = residual_b_.get();
    Conv2d(cur, residual, p, stream, &workspace);
  }

  const int uh = mh * kPackFactor;
  const int uw = mw * kPackFactor;
  DeviceTensor up = workspace.AllocateTensor({static_cast<std::int64_t>(N), 3,
                                              static_cast<std::int64_t>(uh),
                                              static_cast<std::int64_t>(uw)});
  {
    ConvTranspose2dParams p;
    p.in_channels  = kResidualCh;
    p.out_channels = 3;
    p.kH = p.kW = kPackFactor;
    p.sH = p.sW = kPackFactor;
    p.groups    = 3;
    p.weight    = unpack_w_.get();
    p.bias      = nullptr;
    ConvTranspose2d(residual, up, p, stream, &workspace);
  }

  DeviceTensor cropped = workspace.AllocateTensor({static_cast<std::int64_t>(N), 3,
                                                   static_cast<std::int64_t>(uh),
                                                   static_cast<std::int64_t>(uw)});
  CenterCropLike(input, up, cropped, stream);

  DeviceTensor cat = workspace.AllocateTensor({static_cast<std::int64_t>(N), 6,
                                               static_cast<std::int64_t>(uh),
                                               static_cast<std::int64_t>(uw)});
  ConcatChannels(cropped, up, cat, stream);

  const int natural_h = uh - 2;
  const int natural_w = uw - 2;
  DeviceTensor y = workspace.AllocateTensor({static_cast<std::int64_t>(N), kWidth,
                                             static_cast<std::int64_t>(natural_h),
                                             static_cast<std::int64_t>(natural_w)});
  {
    Conv2dParams p;
    p.in_channels  = 6;
    p.out_channels = kWidth;
    p.kH = p.kW = 3;
    p.sH = p.sW = 1;
    p.weight    = post_w_.get();
    p.bias      = post_b_.get();
    Conv2dBiasRelu(cat, y, p, stream, &workspace);
  }

  if (out_h == natural_h && out_w == natural_w) {
    Conv2dParams p;
    p.in_channels  = kWidth;
    p.out_channels = 3;
    p.kH = p.kW = 1;
    p.sH = p.sW = 1;
    p.weight    = output_w_.get();
    p.bias      = output_b_.get();
    Conv2d(y, output, p, stream, &workspace);
  } else {
    DeviceTensor natural_rgb = workspace.AllocateTensor(
        {static_cast<std::int64_t>(N), 3, static_cast<std::int64_t>(natural_h),
         static_cast<std::int64_t>(natural_w)});
    {
      Conv2dParams p;
      p.in_channels  = kWidth;
      p.out_channels = 3;
      p.kH = p.kW = 1;
      p.sH = p.sW = 1;
      p.weight    = output_w_.get();
      p.bias      = output_b_.get();
      Conv2d(y, natural_rgb, p, stream, &workspace);
    }
    CenterCropSpatial(natural_rgb, output, out_h, out_w, stream);
  }
}

}  // namespace alcedo
