//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/nn/demosaicnet_xtrans.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "cuda/nn/common.hpp"
#include "cuda/nn/concat.hpp"
#include "cuda/nn/conv2d.hpp"
#include "cuda/nn/crop.hpp"

namespace alcedo {
namespace {

using cuda::nn::CenterCropLike;
using cuda::nn::ConcatChannels;
using cuda::nn::Conv2d;
using cuda::nn::Conv2dBiasRelu;
using cuda::nn::Conv2dParams;
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

}  // namespace

void XTransDemosaicNet::LoadWeightsImpl(const cuda::nn::SafetensorsTensorMap& tensors,
                                        cudaStream_t stream) {
  RequireUpload(tensors, "conv1.weight", {64, 3, 3, 3}, conv_w_[0], stream);
  RequireUpload(tensors, "conv1.bias", {64}, conv_b_[0], stream);
  for (int i = 2; i <= 11; ++i) {
    const std::string wk = "conv" + std::to_string(i) + ".weight";
    const std::string bk = "conv" + std::to_string(i) + ".bias";
    RequireUpload(tensors, wk, {64, 64, 3, 3}, conv_w_[i - 1], stream);
    RequireUpload(tensors, bk, {64}, conv_b_[i - 1], stream);
  }
  RequireUpload(tensors, "post_conv1.weight", {64, 67, 3, 3}, post_w_, stream);
  RequireUpload(tensors, "post_conv1.bias", {64}, post_b_, stream);
  RequireUpload(tensors, "output.weight", {3, 64, 1, 1}, output_w_, stream);
  RequireUpload(tensors, "output.bias", {3}, output_b_, stream);

  if (stream != nullptr) {
    cuda::nn::CheckCuda(::cudaStreamSynchronize(stream), "XTransDemosaicNet::LoadWeightsImpl sync");
  }
}

auto XTransDemosaicNet::ResidentWeightBytes() const -> std::size_t {
  std::size_t total = 0;
  auto add = [&](const cuda::nn::DeviceBufferF32& b) { total += b.bytes(); };
  for (int i = 0; i < kDepth; ++i) {
    add(conv_w_[i]);
    add(conv_b_[i]);
  }
  add(post_w_);
  add(post_b_);
  add(output_w_);
  add(output_b_);
  return total;
}

auto XTransDemosaicNet::EstimateWorkspaceBytes(int input_h, int input_w, int batch)
    -> std::size_t {
  if (batch < 1 || input_h < kMinSpatial || input_w < kMinSpatial) {
    return 0;
  }

  // Match current Forward: no mid-graph rewind; sum all intermediate tensors.
  const std::int64_t N     = batch;
  std::size_t        total = 0;

  std::int64_t ch = input_h;
  std::int64_t cw = input_w;
  for (int i = 0; i < kDepth; ++i) {
    const std::int64_t oh = ch - 2;
    const std::int64_t ow = cw - 2;
    total += TensorBytes(N, 64, oh, ow);
    ch = oh;
    cw = ow;
  }
  total += TensorBytes(N, 3, ch, cw);    // cropped
  total += TensorBytes(N, 67, ch, cw);   // cat
  total += TensorBytes(N, 64, ch - 2, cw - 2);  // post

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
  if (H < kMinSpatial || W < kMinSpatial) {
    throw std::runtime_error("XTransDemosaicNet::Forward: spatial size below minimum");
  }

  const int out_h = OutputHeight(H);
  const int out_w = OutputWidth(W);
  if (output.shape[0] != N || output.shape[2] != out_h || output.shape[3] != out_w) {
    throw std::runtime_error("XTransDemosaicNet::Forward: output shape mismatch");
  }

  workspace.Reset();
  const std::size_t need = EstimateWorkspaceBytes(H, W, N);
  if (workspace.capacity_bytes() < need) {
    workspace.Reserve(need);
  }

  DeviceTensor cur = input;
  for (int i = 0; i < kDepth; ++i) {
    const int cin = static_cast<int>(cur.shape[1]);
    const int oh  = static_cast<int>(cur.shape[2]) - 2;
    const int ow  = static_cast<int>(cur.shape[3]) - 2;
    if (oh < 1 || ow < 1) {
      throw std::runtime_error("XTransDemosaicNet::Forward: spatial collapsed");
    }
    DeviceTensor next = workspace.AllocateTensor({static_cast<std::int64_t>(N), 64,
                                                  static_cast<std::int64_t>(oh),
                                                  static_cast<std::int64_t>(ow)});
    Conv2dParams p;
    p.in_channels  = cin;
    p.out_channels = 64;
    p.kH = p.kW = 3;
    p.sH = p.sW = 1;
    p.weight    = conv_w_[i].get();
    p.bias      = conv_b_[i].get();
    Conv2dBiasRelu(cur, next, p, stream, &workspace);
    cur = next;
  }

  const int mh = static_cast<int>(cur.shape[2]);
  const int mw = static_cast<int>(cur.shape[3]);

  DeviceTensor cropped = workspace.AllocateTensor({static_cast<std::int64_t>(N), 3,
                                                   static_cast<std::int64_t>(mh),
                                                   static_cast<std::int64_t>(mw)});
  CenterCropLike(input, cur, cropped, stream);

  DeviceTensor cat = workspace.AllocateTensor({static_cast<std::int64_t>(N), 67,
                                               static_cast<std::int64_t>(mh),
                                               static_cast<std::int64_t>(mw)});
  ConcatChannels(cropped, cur, cat, stream);

  DeviceTensor y = workspace.AllocateTensor({static_cast<std::int64_t>(N), 64,
                                             static_cast<std::int64_t>(out_h),
                                             static_cast<std::int64_t>(out_w)});
  {
    Conv2dParams p;
    p.in_channels  = 67;
    p.out_channels = 64;
    p.kH = p.kW = 3;
    p.sH = p.sW = 1;
    p.weight    = post_w_.get();
    p.bias      = post_b_.get();
    Conv2dBiasRelu(cat, y, p, stream, &workspace);
  }

  {
    Conv2dParams p;
    p.in_channels  = 64;
    p.out_channels = 3;
    p.kH = p.kW = 1;
    p.sH = p.sW = 1;
    p.weight    = output_w_.get();
    p.bias      = output_b_.get();
    Conv2d(y, output, p, stream, &workspace);
  }
}

}  // namespace alcedo
