//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/nn/demosaicnet_bayer.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "cuda/nn/common.hpp"
#include "cuda/nn/concat.hpp"
#include "cuda/nn/conv2d.hpp"
#include "cuda/nn/conv_transpose2d.hpp"
#include "cuda/nn/crop.hpp"
#include "cuda/nn/mul.hpp"
#include "cuda/nn/slice.hpp"

namespace alcedo {
namespace {

using cuda::nn::ConcatChannels;
using cuda::nn::Conv2d;
using cuda::nn::Conv2dBiasRelu;
using cuda::nn::Conv2dParams;
using cuda::nn::ConvTranspose2d;
using cuda::nn::ConvTranspose2dParams;
using cuda::nn::CenterCropLike;
using cuda::nn::DeviceTensor;
using cuda::nn::Mul;
using cuda::nn::SplitChannelsView;
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

void BayerDemosaicNet::LoadWeightsImpl(const cuda::nn::SafetensorsTensorMap& tensors,
                                       cudaStream_t stream) {
  // Fixed key → slot map (§2.1). No dynamic layer list.
  RequireUpload(tensors, "pack_mosaick.weight", {4, 3, 2, 2}, pack_w_, stream);
  RequireUpload(tensors, "pack_mosaick.bias", {4}, pack_b_, stream);

  RequireUpload(tensors, "conv1.weight", {64, 4, 3, 3}, conv_w_[0], stream);
  RequireUpload(tensors, "conv1.bias", {64}, conv_b_[0], stream);
  for (int i = 2; i <= 14; ++i) {
    const std::string wk = "conv" + std::to_string(i) + ".weight";
    const std::string bk = "conv" + std::to_string(i) + ".bias";
    RequireUpload(tensors, wk, {64, 64, 3, 3}, conv_w_[i - 1], stream);
    RequireUpload(tensors, bk, {64}, conv_b_[i - 1], stream);
  }
  RequireUpload(tensors, "conv15.weight", {128, 64, 3, 3}, conv_w_[14], stream);
  RequireUpload(tensors, "conv15.bias", {128}, conv_b_[14], stream);

  RequireUpload(tensors, "residual.weight", {12, 64, 1, 1}, residual_w_, stream);
  RequireUpload(tensors, "residual.bias", {12}, residual_b_, stream);
  RequireUpload(tensors, "unpack_mosaick.weight", {12, 1, 2, 2}, unpack_w_, stream);
  RequireUpload(tensors, "unpack_mosaick.bias", {3}, unpack_b_, stream);
  RequireUpload(tensors, "post_conv1.weight", {64, 6, 3, 3}, post_w_, stream);
  RequireUpload(tensors, "post_conv1.bias", {64}, post_b_, stream);
  RequireUpload(tensors, "output.weight", {3, 64, 1, 1}, output_w_, stream);
  RequireUpload(tensors, "output.bias", {3}, output_b_, stream);

  if (stream != nullptr) {
    cuda::nn::CheckCuda(::cudaStreamSynchronize(stream), "BayerDemosaicNet::LoadWeightsImpl sync");
  }
}

auto BayerDemosaicNet::ResidentWeightBytes() const -> std::size_t {
  std::size_t total = 0;
  auto add = [&](const cuda::nn::DeviceBufferF32& b) { total += b.bytes(); };
  add(pack_w_);
  add(pack_b_);
  for (int i = 0; i < kDepth; ++i) {
    add(conv_w_[i]);
    add(conv_b_[i]);
  }
  add(residual_w_);
  add(residual_b_);
  add(unpack_w_);
  add(unpack_b_);
  add(post_w_);
  add(post_b_);
  add(output_w_);
  add(output_b_);
  return total;
}

auto BayerDemosaicNet::EstimateWorkspaceBytes(int input_h, int input_w, int batch) -> std::size_t {
  if (batch < 1 || input_h < kMinSpatial || input_w < kMinSpatial) {
    return 0;
  }
  if ((input_h % 2) != 0 || (input_w % 2) != 0) {
    return 0;
  }

  // Match current Forward: bump allocations are not rewound mid-graph, so peak
  // is the sum of all intermediate tensors (output is caller-owned).
  const std::int64_t N  = batch;
  std::size_t        total = 0;

  const std::int64_t ph = input_h / 2;
  const std::int64_t pw = input_w / 2;
  total += TensorBytes(N, 4, ph, pw);  // pack

  std::int64_t ch = ph;
  std::int64_t cw = pw;
  for (int i = 0; i < kDepth; ++i) {
    const std::int64_t oh = ch - 2;
    const std::int64_t ow = cw - 2;
    const std::int64_t oc = (i == kDepth - 1) ? 128 : 64;
    total += TensorBytes(N, oc, oh, ow);
    ch = oh;
    cw = ow;
  }
  total += TensorBytes(N, 64, ch, cw);  // filtered
  total += TensorBytes(N, 12, ch, cw);  // residual
  const std::int64_t uh = ch * 2;
  const std::int64_t uw = cw * 2;
  total += TensorBytes(N, 3, uh, uw);   // up
  total += TensorBytes(N, 3, uh, uw);   // cropped
  total += TensorBytes(N, 6, uh, uw);   // cat
  total += TensorBytes(N, 64, uh - 2, uw - 2);  // post

  // Headroom for alignment fragmentation across many bump allocs.
  return total + (256 * 1024);
}

void BayerDemosaicNet::Forward(const DeviceTensor& input, DeviceTensor& output,
                               WorkspacePool& workspace, cudaStream_t stream) const {
  if (!weights_loaded()) {
    throw std::runtime_error("BayerDemosaicNet::Forward: weights not loaded");
  }
  RequireContiguousNchw3(input, "BayerDemosaicNet::Forward input");
  RequireContiguousNchw3(output, "BayerDemosaicNet::Forward output");

  const int N = static_cast<int>(input.shape[0]);
  const int H = static_cast<int>(input.shape[2]);
  const int W = static_cast<int>(input.shape[3]);
  if (N < 1) {
    throw std::runtime_error("BayerDemosaicNet::Forward: invalid batch");
  }
  if ((H % 2) != 0 || (W % 2) != 0) {
    throw std::runtime_error("BayerDemosaicNet::Forward: H and W must be even");
  }
  if (H < kMinSpatial || W < kMinSpatial) {
    throw std::runtime_error("BayerDemosaicNet::Forward: spatial size below minimum (64)");
  }

  const int out_h = OutputHeight(H);
  const int out_w = OutputWidth(W);
  if (output.shape[0] != N || output.shape[2] != out_h || output.shape[3] != out_w) {
    throw std::runtime_error("BayerDemosaicNet::Forward: output shape mismatch");
  }

  workspace.Reset();
  const std::size_t need = EstimateWorkspaceBytes(H, W, N);
  if (workspace.capacity_bytes() < need) {
    workspace.Reserve(need);
  }

  // --- low-res branch: pack_mosaick ---
  const int ph = H / 2;
  const int pw = W / 2;
  DeviceTensor cur = workspace.AllocateTensor(
      {static_cast<std::int64_t>(N), 4, static_cast<std::int64_t>(ph),
       static_cast<std::int64_t>(pw)});
  {
    Conv2dParams p;
    p.in_channels  = 3;
    p.out_channels = 4;
    p.kH = p.kW = 2;
    p.sH = p.sW = 2;
    p.weight    = pack_w_.get();
    p.bias      = pack_b_.get();
    Conv2d(input, cur, p, stream, &workspace);
  }

  // conv1..conv15 with fused ReLU (valid 3×3)
  for (int i = 0; i < kDepth; ++i) {
    const int cin  = static_cast<int>(cur.shape[1]);
    const int cout = (i == kDepth - 1) ? 128 : 64;
    const int oh   = static_cast<int>(cur.shape[2]) - 2;
    const int ow   = static_cast<int>(cur.shape[3]) - 2;
    if (oh < 1 || ow < 1) {
      throw std::runtime_error("BayerDemosaicNet::Forward: spatial collapsed in main processor");
    }

    // Rewind previous activation after next is produced: allocate next first.
    const std::size_t mark = workspace.used_bytes();
    DeviceTensor next = workspace.AllocateTensor({static_cast<std::int64_t>(N),
                                                  static_cast<std::int64_t>(cout),
                                                  static_cast<std::int64_t>(oh),
                                                  static_cast<std::int64_t>(ow)});
    Conv2dParams p;
    p.in_channels  = cin;
    p.out_channels = cout;
    p.kH = p.kW = 3;
    p.sH = p.sW = 1;
    p.weight    = conv_w_[i].get();
    p.bias      = conv_b_[i].get();
    Conv2dBiasRelu(cur, next, p, stream, &workspace);

    // Compact: copy next to the mark (overwrite previous cur storage region).
    // Simpler and correct for peak: leave next live and drop conceptual prev
    // by only tracking next as cur (bump pool still holds both until Reset).
    // For steady-state peak control we rewind to mark and re-allocate next
    // size then memcpy — but that adds a D2D copy every layer. Prefer keeping
    // both in the bump allocator; EstimateWorkspaceBytes accounts for it.
    cur = next;
    (void)mark;
  }

  // filters ⊙ masks
  auto [filters, masks] = SplitChannelsView(cur, 64);
  const int mh = static_cast<int>(cur.shape[2]);
  const int mw = static_cast<int>(cur.shape[3]);
  DeviceTensor filtered = workspace.AllocateTensor(
      {static_cast<std::int64_t>(N), 64, static_cast<std::int64_t>(mh),
       static_cast<std::int64_t>(mw)});
  Mul(filters, masks, filtered, stream);

  DeviceTensor residual = workspace.AllocateTensor(
      {static_cast<std::int64_t>(N), 12, static_cast<std::int64_t>(mh),
       static_cast<std::int64_t>(mw)});
  {
    Conv2dParams p;
    p.in_channels  = 64;
    p.out_channels = 12;
    p.kH = p.kW = 1;
    p.sH = p.sW = 1;
    p.weight    = residual_w_.get();
    p.bias      = residual_b_.get();
    Conv2d(filtered, residual, p, stream, &workspace);
  }

  const int uh = mh * 2;
  const int uw = mw * 2;
  DeviceTensor up = workspace.AllocateTensor({static_cast<std::int64_t>(N), 3,
                                              static_cast<std::int64_t>(uh),
                                              static_cast<std::int64_t>(uw)});
  {
    ConvTranspose2dParams p;
    p.in_channels  = 12;
    p.out_channels = 3;
    p.kH = p.kW = 2;
    p.sH = p.sW = 2;
    p.groups    = 3;
    p.weight    = unpack_w_.get();
    p.bias      = unpack_b_.get();
    ConvTranspose2d(residual, up, p, stream, &workspace);
  }

  // --- full-res branch ---
  DeviceTensor cropped = workspace.AllocateTensor({static_cast<std::int64_t>(N), 3,
                                                   static_cast<std::int64_t>(uh),
                                                   static_cast<std::int64_t>(uw)});
  CenterCropLike(input, up, cropped, stream);

  DeviceTensor cat = workspace.AllocateTensor({static_cast<std::int64_t>(N), 6,
                                               static_cast<std::int64_t>(uh),
                                               static_cast<std::int64_t>(uw)});
  ConcatChannels(cropped, up, cat, stream);

  DeviceTensor y = workspace.AllocateTensor({static_cast<std::int64_t>(N), 64,
                                             static_cast<std::int64_t>(out_h),
                                             static_cast<std::int64_t>(out_w)});
  {
    Conv2dParams p;
    p.in_channels  = 6;
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
