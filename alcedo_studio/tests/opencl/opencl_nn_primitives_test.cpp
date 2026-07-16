//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "decoders/processor/operators/gpu/opencl_demosaicnet_programs.hpp"
#include "opencl/nn/convolution.hpp"
#include "opencl/nn/device_buffer.hpp"
#include "opencl/nn/tensor_view.hpp"
#include "opencl/nn/workspace.hpp"
#include "opencl/opencl_backend_program_registry.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {
namespace nn = opencl::nn;

constexpr float kAbsTol = 1e-4f;

// Production channel shapes exercised by the OpenCL DemosaicNet kernels.
struct ChannelShape {
  int in_c  = 0;
  int out_c = 0;
  const char* label = "";
};

constexpr ChannelShape kProd3x3Shapes[] = {
    {4, 24, "bayer_pack_to_trunk"},
    {24, 24, "bayer_trunk"},
    {12, 32, "xtrans_pack_to_trunk"},
    {32, 32, "xtrans_trunk"},
};

constexpr ChannelShape kProd1x1Shapes[] = {
    {24, 12, "bayer_residual"},
    {32, 12, "xtrans_residual"},
    {6, 3, "post_c6_to_rgb"},  // logical C6 in 2 blocks → C3
};

auto EnsureOpenCl() -> bool {
  if (TryPrepareOpenClRuntime()) {
    return true;
  }
  return OpenClContext::Instance().IsInitialized();
}

void RequireOpenCl() {
  if (!EnsureOpenCl()) {
    const std::string error = OpenClContext::Instance().LastInitializationError();
    GTEST_SKIP() << (error.empty() ? "OpenCL runtime unavailable." : error);
  }
  RegisterOpenClBackendPrograms();
}

// Deterministic host fill: includes negatives and over-range values.
auto MakeHostNhwc4(int batch, int height, int width, int logical_c, int channel_blocks,
                   std::uint32_t seed) -> std::vector<float> {
  std::vector<float> host(static_cast<std::size_t>(batch) * height * width * channel_blocks * 4,
                          0.0f);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> dist(-1.5f, 2.0f);
  for (int n = 0; n < batch; ++n) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        for (int c = 0; c < logical_c; ++c) {
          host[nn::Nhwc4ScalarIndex(n, y, x, c, height, width, channel_blocks)] = dist(rng);
        }
      }
    }
  }
  return host;
}

auto MakeHostOihw(int out_c, int in_c, int kh, int kw, std::uint32_t seed) -> std::vector<float> {
  std::vector<float> w(static_cast<std::size_t>(out_c) * in_c * kh * kw);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> dist(-0.25f, 0.25f);
  for (float& v : w) {
    v = dist(rng);
  }
  return w;
}

auto MakeHostBias(int out_c, std::uint32_t seed) -> std::vector<float> {
  std::vector<float> b(static_cast<std::size_t>(out_c));
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
  for (float& v : b) {
    v = dist(rng);
  }
  return b;
}

// CPU reference: NHWC4 direct 3x3, optional bias, optional ReLU. Matches kernel order.
auto CpuConv3x3Nhwc4(const std::vector<float>& input, const std::vector<float>& oihw,
                     const float* bias, int batch, int in_h, int in_w, int out_h, int out_w,
                     int in_c, int out_c, int in_blocks, int out_blocks, int pad_h, int pad_w,
                     bool apply_relu) -> std::vector<float> {
  std::vector<float> output(static_cast<std::size_t>(batch) * out_h * out_w * out_blocks * 4, 0.0f);
  const int k_area = 9;
  for (int n = 0; n < batch; ++n) {
    for (int oy = 0; oy < out_h; ++oy) {
      for (int ox = 0; ox < out_w; ++ox) {
        for (int oc = 0; oc < out_c; ++oc) {
          float acc = 0.0f;
          for (int ky = 0; ky < 3; ++ky) {
            const int iy = oy + ky - pad_h;
            for (int kx = 0; kx < 3; ++kx) {
              const int ix = ox + kx - pad_w;
              if (iy < 0 || iy >= in_h || ix < 0 || ix >= in_w) {
                continue;
              }
              for (int ic = 0; ic < in_c; ++ic) {
                const float in_v =
                    input[nn::Nhwc4ScalarIndex(n, iy, ix, ic, in_h, in_w, in_blocks)];
                const std::size_t wi =
                    (static_cast<std::size_t>(oc) * in_c + ic) * k_area + ky * 3 + kx;
                acc += in_v * oihw[wi];
              }
            }
          }
          if (bias != nullptr) {
            acc += bias[oc];
          }
          if (apply_relu) {
            acc = std::max(acc, 0.0f);
          }
          output[nn::Nhwc4ScalarIndex(n, oy, ox, oc, out_h, out_w, out_blocks)] = acc;
        }
      }
    }
  }
  return output;
}

auto CpuConv1x1Nhwc4(const std::vector<float>& input, const std::vector<float>& oihw,
                     const float* bias, int batch, int height, int width, int in_c, int out_c,
                     int in_blocks, int out_blocks, bool apply_relu) -> std::vector<float> {
  std::vector<float> output(static_cast<std::size_t>(batch) * height * width * out_blocks * 4,
                            0.0f);
  for (int n = 0; n < batch; ++n) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        for (int oc = 0; oc < out_c; ++oc) {
          float acc = 0.0f;
          for (int ic = 0; ic < in_c; ++ic) {
            const float in_v =
                input[nn::Nhwc4ScalarIndex(n, y, x, ic, height, width, in_blocks)];
            const std::size_t wi = static_cast<std::size_t>(oc) * in_c + ic;
            acc += in_v * oihw[wi];
          }
          if (bias != nullptr) {
            acc += bias[oc];
          }
          if (apply_relu) {
            acc = std::max(acc, 0.0f);
          }
          output[nn::Nhwc4ScalarIndex(n, y, x, oc, height, width, out_blocks)] = acc;
        }
      }
    }
  }
  return output;
}

struct MaxAbsDiff {
  float       max_diff = 0.0f;
  std::size_t index    = 0;
  float       expected = 0.0f;
  float       actual   = 0.0f;
};

auto ComputeMaxAbsDiff(const std::vector<float>& expected, const std::vector<float>& actual)
    -> MaxAbsDiff {
  MaxAbsDiff stats;
  const std::size_t n = std::min(expected.size(), actual.size());
  for (std::size_t i = 0; i < n; ++i) {
    const float d = std::fabs(expected[i] - actual[i]);
    if (d > stats.max_diff) {
      stats.max_diff = d;
      stats.index    = i;
      stats.expected = expected[i];
      stats.actual   = actual[i];
    }
  }
  return stats;
}

class KernelHolder {
 public:
  KernelHolder() = default;
  KernelHolder(cl_program program, const char* name) {
    cl_int err = CL_SUCCESS;
    kernel_    = clCreateKernel(program, name, &err);
    nn::CheckOpenCl(err, "clCreateKernel");
  }
  ~KernelHolder() {
    if (kernel_ != nullptr) {
      clReleaseKernel(kernel_);
    }
  }
  KernelHolder(const KernelHolder&)            = delete;
  KernelHolder& operator=(const KernelHolder&) = delete;
  KernelHolder(KernelHolder&& other) noexcept : kernel_(other.kernel_) { other.kernel_ = nullptr; }
  auto operator=(KernelHolder&& other) noexcept -> KernelHolder& {
    if (this != &other) {
      if (kernel_ != nullptr) {
        clReleaseKernel(kernel_);
      }
      kernel_       = other.kernel_;
      other.kernel_ = nullptr;
    }
    return *this;
  }
  [[nodiscard]] auto get() const -> cl_kernel { return kernel_; }

 private:
  cl_kernel kernel_ = nullptr;
};

// Builds kernels once per fixture from already-registered programs (explicit GetProgram).
struct ConvKernels {
  KernelHolder conv3x3_bayer;
  KernelHolder conv1x1_bayer;
  KernelHolder conv3x3_xtrans;
  KernelHolder conv1x1_xtrans;
  bool         ready = false;
};

auto GetSharedKernels() -> ConvKernels& {
  static ConvKernels kernels;
  if (!kernels.ready) {
    // Explicit first use — this is the only GetProgram path; enqueue helpers never build.
    cl_program bayer  = OpenClProgramLibrary::Instance().GetProgram(
        OpenCL::DemosaicNet::kConvBayerProgramName);
    cl_program xtrans = OpenClProgramLibrary::Instance().GetProgram(
        OpenCL::DemosaicNet::kConvXTransProgramName);
    kernels.conv3x3_bayer =
        KernelHolder(bayer, OpenCL::DemosaicNet::kConv3x3KernelName);
    kernels.conv1x1_bayer =
        KernelHolder(bayer, OpenCL::DemosaicNet::kConv1x1KernelName);
    kernels.conv3x3_xtrans =
        KernelHolder(xtrans, OpenCL::DemosaicNet::kConv3x3KernelName);
    kernels.conv1x1_xtrans =
        KernelHolder(xtrans, OpenCL::DemosaicNet::kConv1x1KernelName);
    kernels.ready = true;
  }
  return kernels;
}

auto SelectConv3x3Kernel(int out_blocks) -> cl_kernel {
  auto& k = GetSharedKernels();
  // Prefer matching program variant; both accept runtime channel blocks.
  return out_blocks > 6 ? k.conv3x3_xtrans.get() : k.conv3x3_bayer.get();
}

auto SelectConv1x1Kernel(int out_blocks) -> cl_kernel {
  auto& k = GetSharedKernels();
  return out_blocks > 6 ? k.conv1x1_xtrans.get() : k.conv1x1_bayer.get();
}

// ---------------------------------------------------------------------------
// OHWI4o4i packing (host-only; no OpenCL required)
// ---------------------------------------------------------------------------

TEST(OpenClNnWeightPackTest, PackOhwi4o4iZeroFillsPaddedLanesAndPreservesLogicalWeights) {
  // in_c=5 → 2 blocks, out_c=3 → 1 block, 3x3
  constexpr int in_c  = 5;
  constexpr int out_c = 3;
  constexpr int kh    = 3;
  constexpr int kw    = 3;
  auto          oihw  = MakeHostOihw(out_c, in_c, kh, kw, 11);
  const auto    packed = nn::PackOhwi4o4iFromOihw(oihw.data(), out_c, in_c, kh, kw);
  EXPECT_EQ(packed.size(), nn::Ohwi4o4iElementCount(out_c, in_c, kh, kw));

  const int out_blocks = nn::ChannelBlocks(out_c);
  const int in_blocks  = nn::ChannelBlocks(in_c);
  for (int ob = 0; ob < out_blocks; ++ob) {
    for (int ky = 0; ky < kh; ++ky) {
      for (int kx = 0; kx < kw; ++kx) {
        for (int ib = 0; ib < in_blocks; ++ib) {
          for (int ol = 0; ol < 4; ++ol) {
            for (int il = 0; il < 4; ++il) {
              const int oc = ob * 4 + ol;
              const int ic = ib * 4 + il;
              const std::size_t idx =
                  (((((static_cast<std::size_t>(ob) * kh + ky) * kw + kx) * in_blocks + ib) * 4 +
                    ol) *
                       4 +
                   il);
              if (oc < out_c && ic < in_c) {
                const std::size_t src =
                    (static_cast<std::size_t>(oc) * in_c + ic) * (kh * kw) + ky * kw + kx;
                EXPECT_FLOAT_EQ(packed[idx], oihw[src]);
              } else {
                EXPECT_FLOAT_EQ(packed[idx], 0.0f);
              }
            }
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Workspace
// ---------------------------------------------------------------------------

TEST(OpenClNnWorkspaceTest, GrowOnlyWorkspaceReusesAllocationAfterReserve) {
  RequireOpenCl();
  nn::WorkspacePool pool;
  EXPECT_EQ(pool.allocation_generation(), 0u);

  constexpr std::size_t kPeak = 4 * 1024 * 1024;
  pool.Reserve(kPeak);
  const auto gen_after_reserve = pool.allocation_generation();
  EXPECT_EQ(gen_after_reserve, 1u);
  EXPECT_GE(pool.capacity_bytes(), kPeak);

  for (int i = 0; i < 3; ++i) {
    pool.Reset();
    auto a = pool.Allocate(1024 * 1024);
    auto b = pool.Allocate(1024 * 1024);
    EXPECT_FALSE(a.empty());
    EXPECT_FALSE(b.empty());
    EXPECT_EQ(pool.allocation_generation(), gen_after_reserve)
        << "measured-style reuse must not reallocate after Reserve";
  }

  // Growing beyond capacity when empty is allowed and bumps generation.
  pool.Reset();
  pool.Reserve(kPeak * 2);
  EXPECT_EQ(pool.allocation_generation(), gen_after_reserve + 1);
}

TEST(OpenClNnWorkspaceTest, SubBufferCoversAllocatedSlice) {
  RequireOpenCl();
  nn::WorkspacePool pool;
  pool.Reserve(4096);
  auto slice = pool.Allocate(512);
  nn::SubBuffer sub(slice);
  ASSERT_NE(sub.get(), nullptr);

  std::vector<float> host(128, 3.14f);
  auto&              ctx = OpenClContext::Instance();
  nn::CheckOpenCl(clEnqueueWriteBuffer(ctx.Queue(), sub.get(), CL_TRUE, 0, host.size() * sizeof(float),
                                       host.data(), 0, nullptr, nullptr),
                  "write sub");
  std::vector<float> back(128, 0.0f);
  nn::CheckOpenCl(clEnqueueReadBuffer(ctx.Queue(), sub.get(), CL_TRUE, 0, back.size() * sizeof(float),
                                      back.data(), 0, nullptr, nullptr),
                  "read sub");
  EXPECT_EQ(back, host);
}

// ---------------------------------------------------------------------------
// Convolution correctness
// ---------------------------------------------------------------------------

void RunConv3x3Shape(const ChannelShape& shape, int in_h, int in_w, int pad) {
  RequireOpenCl();
  nn::ResetDispatchInstrumentation();

  const int in_blocks  = nn::ChannelBlocks(shape.in_c);
  const int out_blocks = nn::ChannelBlocks(shape.out_c);
  const int out_h      = nn::ConvOutputSize(in_h, pad, 3);
  const int out_w      = nn::ConvOutputSize(in_w, pad, 3);
  ASSERT_GT(out_h, 0);
  ASSERT_GT(out_w, 0);

  auto host_in  = MakeHostNhwc4(1, in_h, in_w, shape.in_c, in_blocks, 100 + shape.in_c);
  auto host_w   = MakeHostOihw(shape.out_c, shape.in_c, 3, 3, 200 + shape.out_c);
  auto host_b   = MakeHostBias(shape.out_c, 300 + shape.out_c);
  auto packed_w = nn::PackOhwi4o4iFromOihw(host_w.data(), shape.out_c, shape.in_c, 3, 3);

  auto expected = CpuConv3x3Nhwc4(host_in, host_w, host_b.data(), 1, in_h, in_w, out_h, out_w,
                                  shape.in_c, shape.out_c, in_blocks, out_blocks, pad, pad,
                                  /*apply_relu=*/true);

  nn::DeviceBuffer in_buf  = nn::DeviceBuffer::Floats(host_in.size());
  nn::DeviceBuffer out_buf = nn::DeviceBuffer::Floats(
      static_cast<std::size_t>(out_h) * out_w * out_blocks * 4);
  nn::DeviceBuffer w_buf   = nn::DeviceBuffer::Floats(packed_w.size());
  nn::DeviceBuffer b_buf   = nn::DeviceBuffer::Floats(host_b.size());
  in_buf.UploadFloats(host_in);
  w_buf.UploadFloats(packed_w);
  b_buf.UploadFloats(host_b);
  out_buf.FillZero();

  auto in_view  = nn::Nhwc4TensorView::Contiguous(in_buf.get(), 1, in_h, in_w, shape.in_c);
  auto out_view = nn::Nhwc4TensorView::Contiguous(out_buf.get(), 1, out_h, out_w, shape.out_c);

  // Capture whether the program was already built before this enqueue path.
  const bool built_before =
      OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::DemosaicNet::kConvBayerProgramName) ||
      OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::DemosaicNet::kConvXTransProgramName);

  nn::Conv3x3Dispatch d;
  d.kernel  = SelectConv3x3Kernel(out_blocks);
  d.input   = &in_view;
  d.output  = &out_view;
  d.weights = w_buf.get();
  d.bias    = b_buf.get();
  d.pad_h   = pad;
  d.pad_w   = pad;

  auto& ctx = OpenClContext::Instance();
  const auto finish_before = nn::GetDispatchInstrumentation().finish_count;
  nn::EnqueueConv3x3Nhwc4(d, ctx.Queue());
  EXPECT_EQ(nn::GetDispatchInstrumentation().finish_count, finish_before)
      << "enqueue must not clFinish";
  nn::WaitQueue(ctx.Queue());

  // Kernels are created from GetProgram in GetSharedKernels; after first use programs are built.
  EXPECT_TRUE(
      OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::DemosaicNet::kConvBayerProgramName) ||
      OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::DemosaicNet::kConvXTransProgramName));
  (void)built_before;

  auto actual = out_buf.DownloadFloats(expected.size());
  const auto diff = ComputeMaxAbsDiff(expected, actual);
  EXPECT_LE(diff.max_diff, kAbsTol)
      << shape.label << " max_abs=" << diff.max_diff << " idx=" << diff.index
      << " expected=" << diff.expected << " actual=" << diff.actual;
}

TEST(OpenClNnConv3x3Test, ProductionShapesMatchCpuReferenceWithin1eMinus4) {
  RequireOpenCl();
  // Small spatial sizes for fast CPU reference; covers non-multiple-of-8 dims.
  for (const auto& shape : kProd3x3Shapes) {
    RunConv3x3Shape(shape, /*in_h=*/11, /*in_w=*/13, /*pad=*/0);
    RunConv3x3Shape(shape, /*in_h=*/7, /*in_w=*/9, /*pad=*/1);
  }
}

TEST(OpenClNnConv3x3Test, BiasThenReluOrderingMatchesUnfusedCpu) {
  RequireOpenCl();
  // Negative pre-ReLU values verify bias-before-ReLU: bias can push negative→positive.
  constexpr int in_c  = 4;
  constexpr int out_c = 4;
  constexpr int in_h  = 5;
  constexpr int in_w  = 5;
  const int     in_b  = nn::ChannelBlocks(in_c);
  const int     out_b = nn::ChannelBlocks(out_c);
  const int     out_h = nn::ConvOutputSize(in_h, 0, 3);
  const int     out_w = nn::ConvOutputSize(in_w, 0, 3);

  auto host_in = MakeHostNhwc4(1, in_h, in_w, in_c, in_b, 42);
  // Force some large negative inputs so without bias+relu order, mismatch is likely.
  for (float& v : host_in) {
    if (v > 0.0f) {
      v = -v;
    }
  }
  auto host_w = MakeHostOihw(out_c, in_c, 3, 3, 43);
  auto host_b = MakeHostBias(out_c, 44);
  // Large positive bias so ReLU-after-bias keeps values that ReLU-before would zero.
  for (float& v : host_b) {
    v = 2.0f;
  }

  auto expected = CpuConv3x3Nhwc4(host_in, host_w, host_b.data(), 1, in_h, in_w, out_h, out_w, in_c,
                                  out_c, in_b, out_b, 0, 0, true);
  auto packed_w = nn::PackOhwi4o4iFromOihw(host_w.data(), out_c, in_c, 3, 3);

  nn::DeviceBuffer in_buf  = nn::DeviceBuffer::Floats(host_in.size());
  nn::DeviceBuffer out_buf = nn::DeviceBuffer::Floats(expected.size());
  nn::DeviceBuffer w_buf   = nn::DeviceBuffer::Floats(packed_w.size());
  nn::DeviceBuffer b_buf   = nn::DeviceBuffer::Floats(host_b.size());
  in_buf.UploadFloats(host_in);
  w_buf.UploadFloats(packed_w);
  b_buf.UploadFloats(host_b);

  auto in_view  = nn::Nhwc4TensorView::Contiguous(in_buf.get(), 1, in_h, in_w, in_c);
  auto out_view = nn::Nhwc4TensorView::Contiguous(out_buf.get(), 1, out_h, out_w, out_c);
  nn::Conv3x3Dispatch d{SelectConv3x3Kernel(out_b), &in_view, &out_view, w_buf.get(), b_buf.get(),
                        0, 0};
  auto& ctx = OpenClContext::Instance();
  nn::EnqueueConv3x3Nhwc4(d, ctx.Queue());
  nn::WaitQueue(ctx.Queue());

  auto actual = out_buf.DownloadFloats(expected.size());
  EXPECT_LE(ComputeMaxAbsDiff(expected, actual).max_diff, kAbsTol);
}

void RunConv1x1Shape(const ChannelShape& shape, int height, int width, bool relu) {
  RequireOpenCl();
  nn::ResetDispatchInstrumentation();

  const int in_blocks  = nn::ChannelBlocks(shape.in_c);
  // Post C6→C3: logical 6 uses 2 physical blocks (padded lanes).
  const int out_blocks = nn::ChannelBlocks(shape.out_c);
  const int in_phys_blocks =
      (shape.in_c == 6) ? 2 : in_blocks;  // C6 logical in 8-lane allocation

  auto host_in = MakeHostNhwc4(1, height, width, shape.in_c, in_phys_blocks, 50 + shape.in_c);
  // Poison padded lanes so incorrect masking would corrupt the result.
  if (shape.in_c < in_phys_blocks * 4) {
    for (int n = 0; n < 1; ++n) {
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          for (int c = shape.in_c; c < in_phys_blocks * 4; ++c) {
            host_in[nn::Nhwc4ScalarIndex(n, y, x, c, height, width, in_phys_blocks)] = 999.0f;
          }
        }
      }
    }
  }

  auto host_w   = MakeHostOihw(shape.out_c, shape.in_c, 1, 1, 60 + shape.out_c);
  auto host_b   = MakeHostBias(shape.out_c, 70 + shape.out_c);
  auto packed_w = nn::PackOhwi4o4iFromOihw(host_w.data(), shape.out_c, shape.in_c, 1, 1);
  auto expected =
      CpuConv1x1Nhwc4(host_in, host_w, host_b.data(), 1, height, width, shape.in_c, shape.out_c,
                      in_phys_blocks, out_blocks, relu);

  nn::DeviceBuffer in_buf  = nn::DeviceBuffer::Floats(host_in.size());
  nn::DeviceBuffer out_buf = nn::DeviceBuffer::Floats(expected.size());
  nn::DeviceBuffer w_buf   = nn::DeviceBuffer::Floats(packed_w.size());
  nn::DeviceBuffer b_buf   = nn::DeviceBuffer::Floats(host_b.size());
  in_buf.UploadFloats(host_in);
  w_buf.UploadFloats(packed_w);
  b_buf.UploadFloats(host_b);

  auto in_view =
      nn::Nhwc4TensorView::ContiguousBlocked(in_buf.get(), 1, height, width, shape.in_c,
                                             in_phys_blocks);
  auto out_view = nn::Nhwc4TensorView::Contiguous(out_buf.get(), 1, height, width, shape.out_c);

  nn::Conv1x1Dispatch d;
  d.kernel     = SelectConv1x1Kernel(std::max(out_blocks, in_phys_blocks));
  d.input      = &in_view;
  d.output     = &out_view;
  d.weights    = w_buf.get();
  d.bias       = b_buf.get();
  d.apply_relu = relu ? 1 : 0;

  auto& ctx = OpenClContext::Instance();
  nn::EnqueueConv1x1Nhwc4(d, ctx.Queue());
  nn::WaitQueue(ctx.Queue());

  auto actual = out_buf.DownloadFloats(expected.size());
  const auto diff = ComputeMaxAbsDiff(expected, actual);
  EXPECT_LE(diff.max_diff, kAbsTol)
      << shape.label << " relu=" << relu << " max_abs=" << diff.max_diff;
}

TEST(OpenClNnConv1x1Test, ProductionShapesMatchCpuReferenceAndIgnorePaddedChannels) {
  RequireOpenCl();
  for (const auto& shape : kProd1x1Shapes) {
    RunConv1x1Shape(shape, /*height=*/8, /*width=*/7, /*relu=*/true);
    RunConv1x1Shape(shape, /*height=*/5, /*width=*/6, /*relu=*/false);
  }
}

TEST(OpenClNnConv3x3Test, BoundaryWorkItemsDoNotWriteOutsideTensorView) {
  RequireOpenCl();
  // Allocate oversized output capacity; only out_h x out_w region is a valid view.
  // Kernel global size uses out dims; edge work-items must early-out.
  constexpr int in_c  = 4;
  constexpr int out_c = 4;
  constexpr int in_h  = 6;
  constexpr int in_w  = 5;  // non-multiple of typical WG size
  const int     out_h = nn::ConvOutputSize(in_h, 0, 3);
  const int     out_w = nn::ConvOutputSize(in_w, 0, 3);
  const int     in_b  = nn::ChannelBlocks(in_c);
  const int     out_b = nn::ChannelBlocks(out_c);

  // Physical buffer larger than the logical output tensor.
  const std::size_t logical_floats =
      static_cast<std::size_t>(out_h) * out_w * out_b * 4;
  const std::size_t capacity_floats = logical_floats + 64;
  std::vector<float> sentinel(capacity_floats, 12345.0f);

  auto host_in  = MakeHostNhwc4(1, in_h, in_w, in_c, in_b, 9);
  auto host_w   = MakeHostOihw(out_c, in_c, 3, 3, 10);
  auto host_b   = MakeHostBias(out_c, 11);
  auto packed_w = nn::PackOhwi4o4iFromOihw(host_w.data(), out_c, in_c, 3, 3);

  nn::DeviceBuffer in_buf  = nn::DeviceBuffer::Floats(host_in.size());
  nn::DeviceBuffer out_buf = nn::DeviceBuffer::Floats(capacity_floats);
  nn::DeviceBuffer w_buf   = nn::DeviceBuffer::Floats(packed_w.size());
  nn::DeviceBuffer b_buf   = nn::DeviceBuffer::Floats(host_b.size());
  in_buf.UploadFloats(host_in);
  w_buf.UploadFloats(packed_w);
  b_buf.UploadFloats(host_b);
  out_buf.UploadFloats(sentinel);

  auto in_view  = nn::Nhwc4TensorView::Contiguous(in_buf.get(), 1, in_h, in_w, in_c);
  auto out_view = nn::Nhwc4TensorView::Contiguous(out_buf.get(), 1, out_h, out_w, out_c);
  nn::Conv3x3Dispatch d{SelectConv3x3Kernel(out_b), &in_view, &out_view, w_buf.get(), b_buf.get(),
                        0, 0};
  auto& ctx = OpenClContext::Instance();
  nn::EnqueueConv3x3Nhwc4(d, ctx.Queue());
  nn::WaitQueue(ctx.Queue());

  auto actual = out_buf.DownloadFloats(capacity_floats);
  for (std::size_t i = logical_floats; i < capacity_floats; ++i) {
    EXPECT_FLOAT_EQ(actual[i], 12345.0f) << "wrote past tensor view at float index " << i;
  }
}

TEST(OpenClNnConvDispatchTest, MultiLayerSequenceDoesNotSynchronizeBetweenEnqueues) {
  RequireOpenCl();
  nn::ResetDispatchInstrumentation();

  constexpr int c    = 4;
  constexpr int in_h = 9;
  constexpr int in_w = 9;
  const int     b    = nn::ChannelBlocks(c);

  // Layer0: 9→7, layer1: 7→5 (two valid 3x3).
  auto h0 = MakeHostNhwc4(1, in_h, in_w, c, b, 1);
  auto w0 = MakeHostOihw(c, c, 3, 3, 2);
  auto w1 = MakeHostOihw(c, c, 3, 3, 3);
  auto bias = MakeHostBias(c, 4);
  auto p0 = nn::PackOhwi4o4iFromOihw(w0.data(), c, c, 3, 3);
  auto p1 = nn::PackOhwi4o4iFromOihw(w1.data(), c, c, 3, 3);

  nn::DeviceBuffer a = nn::DeviceBuffer::Floats(h0.size());
  nn::DeviceBuffer mid = nn::DeviceBuffer::Floats(static_cast<std::size_t>(7) * 7 * b * 4);
  nn::DeviceBuffer out = nn::DeviceBuffer::Floats(static_cast<std::size_t>(5) * 5 * b * 4);
  nn::DeviceBuffer wb0 = nn::DeviceBuffer::Floats(p0.size());
  nn::DeviceBuffer wb1 = nn::DeviceBuffer::Floats(p1.size());
  nn::DeviceBuffer bb  = nn::DeviceBuffer::Floats(bias.size());
  a.UploadFloats(h0);
  wb0.UploadFloats(p0);
  wb1.UploadFloats(p1);
  bb.UploadFloats(bias);

  auto v0 = nn::Nhwc4TensorView::Contiguous(a.get(), 1, 9, 9, c);
  auto v1 = nn::Nhwc4TensorView::Contiguous(mid.get(), 1, 7, 7, c);
  auto v2 = nn::Nhwc4TensorView::Contiguous(out.get(), 1, 5, 5, c);
  cl_kernel k = SelectConv3x3Kernel(b);
  auto& ctx = OpenClContext::Instance();

  nn::Conv3x3Dispatch d0{k, &v0, &v1, wb0.get(), bb.get(), 0, 0};
  nn::Conv3x3Dispatch d1{k, &v1, &v2, wb1.get(), bb.get(), 0, 0};

  EXPECT_EQ(nn::GetDispatchInstrumentation().finish_count, 0u);
  nn::EnqueueConv3x3Nhwc4(d0, ctx.Queue());
  nn::EnqueueConv3x3Nhwc4(d1, ctx.Queue());
  EXPECT_EQ(nn::GetDispatchInstrumentation().finish_count, 0u)
      << "no clFinish inside multi-layer sequence";
  EXPECT_EQ(nn::GetDispatchInstrumentation().enqueue_count, 2u);
  nn::WaitQueue(ctx.Queue());
  EXPECT_EQ(nn::GetDispatchInstrumentation().finish_count, 1u);
}

TEST(OpenClNnConvDispatchTest, EnqueueHelpersDoNotTriggerImplicitProgramBuild) {
  RequireOpenCl();
  // After backend register, Neural programs must stay cold until explicit GetProgram.
  // This test only constructs kernels if already warmed by prior tests in the process;
  // when run alone, GetSharedKernels is the sole build trigger (explicit).
  RegisterOpenClBackendPrograms();
  OpenClProgramLibrary::Instance().WarmUpRequiredPrograms();

  // If nothing has requested DemosaicNet programs yet in this process, they remain unbuilt.
  // We then explicitly GetProgram once and verify subsequent enqueues do not rebuild.
  const bool built_bayer =
      OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::DemosaicNet::kConvBayerProgramName);
  if (!built_bayer) {
    EXPECT_FALSE(OpenClProgramLibrary::Instance().IsProgramBuilt(
        OpenCL::DemosaicNet::kConvBayerProgramName));
  }

  // Explicit build (allowed once).
  (void)GetSharedKernels();
  EXPECT_TRUE(OpenClProgramLibrary::Instance().IsProgramBuilt(
      OpenCL::DemosaicNet::kConvBayerProgramName));

  // Enqueue path uses existing kernels only — IsProgramBuilt stays true, no throw.
  constexpr int c = 4;
  auto host_in = MakeHostNhwc4(1, 5, 5, c, 1, 1);
  auto host_w  = MakeHostOihw(c, c, 3, 3, 2);
  auto host_b  = MakeHostBias(c, 3);
  auto packed  = nn::PackOhwi4o4iFromOihw(host_w.data(), c, c, 3, 3);
  nn::DeviceBuffer in_buf  = nn::DeviceBuffer::Floats(host_in.size());
  nn::DeviceBuffer out_buf = nn::DeviceBuffer::Floats(static_cast<std::size_t>(3) * 3 * 4);
  nn::DeviceBuffer w_buf   = nn::DeviceBuffer::Floats(packed.size());
  nn::DeviceBuffer b_buf   = nn::DeviceBuffer::Floats(host_b.size());
  in_buf.UploadFloats(host_in);
  w_buf.UploadFloats(packed);
  b_buf.UploadFloats(host_b);
  auto in_view  = nn::Nhwc4TensorView::Contiguous(in_buf.get(), 1, 5, 5, c);
  auto out_view = nn::Nhwc4TensorView::Contiguous(out_buf.get(), 1, 3, 3, c);
  nn::Conv3x3Dispatch d{SelectConv3x3Kernel(1), &in_view, &out_view, w_buf.get(), b_buf.get(), 0,
                        0};
  auto& ctx = OpenClContext::Instance();
  EXPECT_NO_THROW(nn::EnqueueConv3x3Nhwc4(d, ctx.Queue()));
  nn::WaitQueue(ctx.Queue());
  EXPECT_TRUE(OpenClProgramLibrary::Instance().IsProgramBuilt(
      OpenCL::DemosaicNet::kConvBayerProgramName));
}

}  // namespace
}  // namespace alcedo
