//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Phase 2: Metal fixed Bayer/X-Trans MPSGraph modules vs exported student
// references. Absolute tolerance 1e-4. Cache reuse and invalid-metadata rejection.

#ifdef HAVE_METAL

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <alcedo/metal/Metal.hpp>

#include "decoders/processor/nn/metal_demosaicnet_cache.hpp"
#include "decoders/processor/nn/metal_demosaicnet_module.hpp"
#include "metal/metal_context.hpp"
#include "nn/safetensors.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;

constexpr float kAbsTol = 1e-4f;

void RequireMetal() {
  auto& ctx = MetalContext::Instance();
  if (ctx.Device() == nullptr || ctx.Queue() == nullptr) {
    GTEST_SKIP() << "Metal device or queue unavailable.";
  }
}

auto FindPath(const char* filename, std::initializer_list<const char*> prefixes) -> fs::path {
  for (const char* prefix : prefixes) {
    fs::path        path = fs::path(prefix) / filename;
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
      return path;
    }
  }
  return {};
}

auto FindModelDir() -> fs::path {
#ifdef ALCEDO_DEMOASICNET_MODEL_DIR
  {
    fs::path        p{ALCEDO_DEMOASICNET_MODEL_DIR};
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      return p;
    }
  }
#endif
  return FindPath("bayer.safetensors",
                  {"alcedo_studio/src/config/models/", "../alcedo_studio/src/config/models/",
                   "../../alcedo_studio/src/config/models/",
                   "../../../alcedo_studio/src/config/models/", "src/config/models/",
                   "../src/config/models/"})
      .parent_path();
}

auto FindGolden(const char* filename) -> fs::path {
  return FindPath(filename,
                  {"alcedo_studio/tests/ml_ops/goldens/", "../alcedo_studio/tests/ml_ops/goldens/",
                   "../../alcedo_studio/tests/ml_ops/goldens/",
                   "../../../alcedo_studio/tests/ml_ops/goldens/", "tests/ml_ops/goldens/",
                   "../tests/ml_ops/goldens/"});
}

auto LoadFloatBin(const fs::path& path, std::size_t expected_count) -> std::vector<float> {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open " + path.string());
  }
  in.seekg(0, std::ios::end);
  const auto nbytes = static_cast<std::size_t>(in.tellg());
  in.seekg(0, std::ios::beg);
  if (nbytes != expected_count * sizeof(float)) {
    throw std::runtime_error("unexpected size for " + path.string());
  }
  std::vector<float> data(expected_count);
  in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(nbytes));
  return data;
}

struct MaxAbsReport {
  float       max_abs  = 0.0f;
  std::size_t index    = 0;
  float       expected = 0.0f;
  float       actual   = 0.0f;
  int         y        = 0;
  int         x        = 0;
  int         channel  = 0;
};

auto ComputeMaxAbsNchw(const std::vector<float>& actual, const std::vector<float>& expected, int h,
                       int w) -> MaxAbsReport {
  MaxAbsReport      report;
  const std::size_t plane = static_cast<std::size_t>(h) * w;
  for (std::size_t i = 0; i < actual.size(); ++i) {
    const float err = std::fabs(actual[i] - expected[i]);
    if (err > report.max_abs) {
      report.max_abs        = err;
      report.index          = i;
      report.expected       = expected[i];
      report.actual         = actual[i];
      report.channel        = static_cast<int>(i / plane);
      const std::size_t pix = i % plane;
      report.y              = static_cast<int>(pix / static_cast<std::size_t>(w));
      report.x              = static_cast<int>(pix % static_cast<std::size_t>(w));
    }
  }
  return report;
}

void ExpectNchwNear(const std::vector<float>& actual, const std::vector<float>& expected, int h,
                    int w, float abs_tol) {
  ASSERT_EQ(actual.size(), expected.size());
  for (float v : actual) {
    ASSERT_TRUE(std::isfinite(v));
  }
  const auto report = ComputeMaxAbsNchw(actual, expected, h, w);
  EXPECT_LE(report.max_abs, abs_tol)
      << "max_abs=" << report.max_abs << " at nchw index=" << report.index << " y=" << report.y
      << " x=" << report.x << " channel=" << report.channel << " expected=" << report.expected
      << " actual=" << report.actual;
  if (report.max_abs > abs_tol) {
    std::cerr << "first failing coordinate: y=" << report.y << " x=" << report.x
              << " c=" << report.channel << "\n";
  }
}

// ---------------------------------------------------------------------------
// Host FP32 reference of the fixed product graph (NCHW). Used when exported
// student .bin goldens are absent (they are gitignored via *.bin).
// ---------------------------------------------------------------------------

struct HostTensor {
  int                c = 0;
  int                h = 0;
  int                w = 0;
  std::vector<float> data;

  HostTensor() = default;
  HostTensor(int channels, int height, int width)
      : c(channels), h(height), w(width), data(static_cast<std::size_t>(channels) * height * width) {}

  [[nodiscard]] auto At(int channel, int y, int x) -> float& {
    return data[(static_cast<std::size_t>(channel) * h + y) * w + x];
  }
  [[nodiscard]] auto At(int channel, int y, int x) const -> float {
    return data[(static_cast<std::size_t>(channel) * h + y) * w + x];
  }
};

auto Conv2dValid(const HostTensor& input, const float* weight_oihw, const float* bias, int out_c,
                 int kh, int kw, int stride, bool relu) -> HostTensor {
  const int oh = (input.h - kh) / stride + 1;
  const int ow = (input.w - kw) / stride + 1;
  HostTensor out(out_c, oh, ow);
  for (int oc = 0; oc < out_c; ++oc) {
    for (int y = 0; y < oh; ++y) {
      for (int x = 0; x < ow; ++x) {
        float sum = 0.0f;
        for (int ic = 0; ic < input.c; ++ic) {
          for (int ky = 0; ky < kh; ++ky) {
            for (int kx = 0; kx < kw; ++kx) {
              const int iy = y * stride + ky;
              const int ix = x * stride + kx;
              const float w =
                  weight_oihw[(((oc * input.c + ic) * kh) + ky) * kw + kx];
              sum += input.At(ic, iy, ix) * w;
            }
          }
        }
        if (bias != nullptr) {
          sum += bias[oc];
        }
        if (relu && sum < 0.0f) {
          sum = 0.0f;
        }
        out.At(oc, y, x) = sum;
      }
    }
  }
  return out;
}

auto ResidualUnpack(const HostTensor& residual) -> HostTensor {
  // residual channels: rgb group g, then row-major subpixels (py, px).
  HostTensor out(3, residual.h * 2, residual.w * 2);
  for (int y = 0; y < residual.h; ++y) {
    for (int x = 0; x < residual.w; ++x) {
      for (int g = 0; g < 3; ++g) {
        for (int py = 0; py < 2; ++py) {
          for (int px = 0; px < 2; ++px) {
            const int ch = g * 4 + py * 2 + px;
            out.At(g, y * 2 + py, x * 2 + px) = residual.At(ch, y, x);
          }
        }
      }
    }
  }
  return out;
}

auto CenterCrop(const HostTensor& input, int out_h, int out_w) -> HostTensor {
  const int top  = (input.h - out_h) / 2;
  const int left = (input.w - out_w) / 2;
  HostTensor out(input.c, out_h, out_w);
  for (int c = 0; c < input.c; ++c) {
    for (int y = 0; y < out_h; ++y) {
      for (int x = 0; x < out_w; ++x) {
        out.At(c, y, x) = input.At(c, top + y, left + x);
      }
    }
  }
  return out;
}

auto ConcatChannels(const HostTensor& a, const HostTensor& b) -> HostTensor {
  HostTensor out(a.c + b.c, a.h, a.w);
  for (int c = 0; c < a.c; ++c) {
    for (int y = 0; y < a.h; ++y) {
      for (int x = 0; x < a.w; ++x) {
        out.At(c, y, x) = a.At(c, y, x);
      }
    }
  }
  for (int c = 0; c < b.c; ++c) {
    for (int y = 0; y < b.h; ++y) {
      for (int x = 0; x < b.w; ++x) {
        out.At(a.c + c, y, x) = b.At(c, y, x);
      }
    }
  }
  return out;
}

auto HostForward(const nn::SafetensorsTensorMap& tensors, const std::vector<float>& input_nchw,
                 int tile_input, int tile_output, int depth, int width, int pack_out_ch,
                 int residual_ch) -> std::vector<float> {
  HostTensor original(3, tile_input, tile_input);
  if (input_nchw.size() != original.data.size()) {
    throw std::runtime_error("HostForward: unexpected input size");
  }
  original.data = input_nchw;
  HostTensor x  = original;

  const auto& pack_w = nn::RequireF32Tensor(tensors, "pack.weight", {pack_out_ch, 3, 2, 2});
  x = Conv2dValid(x, pack_w.data.data(), nullptr, pack_out_ch, 2, 2, 2, /*relu=*/false);

  for (int i = 0; i < depth; ++i) {
    const int in_c = (i == 0) ? pack_out_ch : width;
    const auto& w =
        nn::RequireF32Tensor(tensors, "trunk." + std::to_string(i) + ".weight", {width, in_c, 3, 3});
    const auto& b = nn::RequireF32Tensor(tensors, "trunk." + std::to_string(i) + ".bias", {width});
    x = Conv2dValid(x, w.data.data(), b.data.data(), width, 3, 3, 1, /*relu=*/true);
  }

  {
    const auto& w =
        nn::RequireF32Tensor(tensors, "residual.weight", {residual_ch, width, 1, 1});
    const auto& b = nn::RequireF32Tensor(tensors, "residual.bias", {residual_ch});
    x = Conv2dValid(x, w.data.data(), b.data.data(), residual_ch, 1, 1, 1, /*relu=*/false);
  }

  HostTensor residual_rgb = ResidualUnpack(x);
  HostTensor skip         = CenterCrop(original, residual_rgb.h, residual_rgb.w);
  // DemosaicNet post_conv consumes sparse mosaic RGB first, then residual RGB.
  // This is the same C6 ABI used by CUDA/OpenCL product kernels.
  x                       = ConcatChannels(skip, residual_rgb);

  {
    const auto& w = nn::RequireF32Tensor(tensors, "post_conv.weight", {width, 6, 3, 3});
    const auto& b = nn::RequireF32Tensor(tensors, "post_conv.bias", {width});
    x = Conv2dValid(x, w.data.data(), b.data.data(), width, 3, 3, 1, /*relu=*/true);
  }
  {
    const auto& w = nn::RequireF32Tensor(tensors, "output.weight", {3, width, 1, 1});
    const auto& b = nn::RequireF32Tensor(tensors, "output.bias", {3});
    x = Conv2dValid(x, w.data.data(), b.data.data(), 3, 1, 1, 1, /*relu=*/false);
  }

  x = CenterCrop(x, tile_output, tile_output);
  return x.data;
}

auto MakeDeterministicInput(int channels, int height, int width, std::uint32_t seed)
    -> std::vector<float> {
  std::vector<float> data(static_cast<std::size_t>(channels) * height * width);
  std::uint32_t      state = seed;
  for (float& v : data) {
    state = state * 1664525u + 1013904223u;
    // Signed values in roughly [-0.5, 1.5] to exercise negatives and >1.
    v = static_cast<float>(state % 2001u) / 1000.0f - 0.5f;
  }
  return data;
}

class MetalDemosaicNetModuleTest : public ::testing::Test {
 protected:
  void SetUp() override { RequireMetal(); }
};

}  // namespace

// ---------------------------------------------------------------------------
// Load / metadata validation
// ---------------------------------------------------------------------------

TEST_F(MetalDemosaicNetModuleTest, BayerLoadRejectsWrongArchitectureMetadata) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }
  auto map             = nn::LoadSafetensors(model_dir / "bayer.safetensors");
  auto meta            = map.metadata();
  meta["architecture"] = "teacher_bayer_d15";
  map.SetMetadata(std::move(meta));

  MetalBayerDemosaicNet net;
  EXPECT_THROW(net.LoadAndCompile(map), std::runtime_error);
  EXPECT_FALSE(net.ready());
}

TEST_F(MetalDemosaicNetModuleTest, BayerLoadRejectsWrongTensorShapeWithoutPublishing) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }
  auto map      = nn::LoadSafetensors(model_dir / "bayer.safetensors");
  auto mutated  = map.at("trunk.0.weight");
  mutated.shape = {24, 5, 3, 3};
  mutated.data.assign(mutated.numel(), 0.0f);
  map.InsertOrAssign(std::move(mutated));

  MetalBayerDemosaicNet net;
  EXPECT_THROW(net.LoadAndCompile(map), std::runtime_error);
  EXPECT_FALSE(net.ready());
}

TEST_F(MetalDemosaicNetModuleTest, XTransLoadAndCompileProducesResidentBuffers) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }
  auto                   map = nn::LoadSafetensors(model_dir / "xtrans.safetensors");
  MetalXTransDemosaicNet net;
  ASSERT_NO_THROW(net.LoadAndCompile(map));
  EXPECT_TRUE(net.ready());
  EXPECT_GT(net.ResidentWeightBytes(), 0u);
  EXPECT_GT(net.OwnedBufferBytes(), 0u);
  EXPECT_EQ(net.compile_count(), 1u);
  EXPECT_EQ(net.input_output_allocation_count(), 1u);
  EXPECT_NE(net.InputBuffer(), nullptr);
  EXPECT_NE(net.OutputBuffer(), nullptr);
  EXPECT_EQ(net.InputBuffer()->length(),
            static_cast<NS::UInteger>(MetalXTransDemosaicNet::kBatchSize * 1048u * 1048u * 3u *
                                     sizeof(float)));
  // Graph product is skip/residual concat [N, unpacked_h, unpacked_h, 6].
  // X-Trans: packed=524, residual=516, unpacked=1032.
  EXPECT_EQ(net.OutputBuffer()->length(),
            static_cast<NS::UInteger>(MetalXTransDemosaicNet::kBatchSize * 1032u * 1032u * 6u *
                                     sizeof(float)));
  EXPECT_EQ(net.CatHeight(), 1032);
  EXPECT_GE(net.FinalCrop(), 0);
}

// ---------------------------------------------------------------------------
// Lazy cache
// ---------------------------------------------------------------------------

TEST_F(MetalDemosaicNetModuleTest, CacheRejectsMissingModelWithoutPublishingEntry) {
  MetalDemosaicNetModelCache  cache;
  MetalDemosaicNetLoadOptions opts;
  opts.model_dir = fs::path("does_not_exist_subdir");
  EXPECT_FALSE(cache.EnsureLoaded(MetalDemosaicNetVariant::Bayer, opts));
  EXPECT_FALSE(cache.IsLoaded(MetalDemosaicNetVariant::Bayer));
  EXPECT_FALSE(cache.LastError().empty());
}

TEST_F(MetalDemosaicNetModuleTest, CacheSecondEnsureLoadedPerformsNoParseOrCompile) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }
  MetalDemosaicNetLoadOptions opts;
  opts.model_dir = model_dir;

  MetalDemosaicNetModelCache cache;
  ASSERT_TRUE(cache.EnsureLoaded(MetalDemosaicNetVariant::Bayer, opts)) << cache.LastError();
  const auto parse_after_first   = cache.parse_count();
  const auto compile_after_first = cache.compile_count();
  const auto alloc_after_first   = cache.input_output_allocation_count();
  ASSERT_GE(parse_after_first, 1u);
  ASSERT_GE(compile_after_first, 1u);
  ASSERT_GE(alloc_after_first, 1u);

  MTL::Buffer* input0  = cache.Bayer().InputBuffer();
  MTL::Buffer* output0 = cache.Bayer().OutputBuffer();
  ASSERT_NE(input0, nullptr);
  ASSERT_NE(output0, nullptr);

  ASSERT_TRUE(cache.EnsureLoaded(MetalDemosaicNetVariant::Bayer, opts));
  EXPECT_EQ(cache.parse_count(), parse_after_first);
  EXPECT_EQ(cache.compile_count(), compile_after_first);
  EXPECT_EQ(cache.input_output_allocation_count(), alloc_after_first);
  EXPECT_EQ(cache.Bayer().InputBuffer(), input0);
  EXPECT_EQ(cache.Bayer().OutputBuffer(), output0);
}

TEST_F(MetalDemosaicNetModuleTest, CacheContainsNoAlternateImplementation) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }
  MetalDemosaicNetLoadOptions opts;
  opts.model_dir = model_dir;
  MetalDemosaicNetModelCache cache;
  ASSERT_TRUE(cache.EnsureLoaded(MetalDemosaicNetVariant::Bayer, opts)) << cache.LastError();
  ASSERT_TRUE(cache.EnsureLoaded(MetalDemosaicNetVariant::XTrans, opts)) << cache.LastError();
  EXPECT_TRUE(cache.IsLoaded(MetalDemosaicNetVariant::Bayer));
  EXPECT_TRUE(cache.IsLoaded(MetalDemosaicNetVariant::XTrans));
  EXPECT_NE(cache.Bayer().InputBuffer(), cache.XTrans().InputBuffer());
  EXPECT_EQ(std::string(MetalBayerDemosaicNet::kArchitecture), "bayer_s24_d8");
  EXPECT_EQ(std::string(MetalXTransDemosaicNet::kArchitecture), "xtrans_p2_s32_d4");
}

// ---------------------------------------------------------------------------
// Exported student reference pairs (absolute 1e-4)
// ---------------------------------------------------------------------------

TEST_F(MetalDemosaicNetModuleTest, BayerForwardMatchesHostReferenceWithin1eMinus4) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  constexpr int H  = MetalBayerDemosaicNet::kTileInput;
  constexpr int W  = MetalBayerDemosaicNet::kTileInput;
  constexpr int Oh = MetalBayerDemosaicNet::kTileOutput;
  constexpr int Ow = MetalBayerDemosaicNet::kTileOutput;
  auto tensors     = nn::LoadSafetensors(model_dir / "bayer.safetensors");
  const auto input = MakeDeterministicInput(3, H, W, /*seed=*/0xBA7E0001u);

  MetalBayerDemosaicNet net;
  net.LoadAndCompile(tensors);
  std::vector<float> actual(static_cast<std::size_t>(3) * Oh * Ow);
  net.ForwardNchwReference(input.data(), actual.data());

  const auto expected =
      HostForward(tensors, input, H, Oh, MetalBayerDemosaicNet::kDepth,
                  MetalBayerDemosaicNet::kWidth, MetalBayerDemosaicNet::kPackOutCh,
                  MetalBayerDemosaicNet::kResidualCh);
  ExpectNchwNear(actual, expected, Oh, Ow, kAbsTol);

  // Second run reuses compiled executable and owned tile buffers.
  const auto compile_before = net.compile_count();
  const auto alloc_before   = net.input_output_allocation_count();
  MTL::Buffer* in_buf       = net.InputBuffer();
  MTL::Buffer* out_buf      = net.OutputBuffer();
  net.ForwardNchwReference(input.data(), actual.data());
  EXPECT_EQ(net.compile_count(), compile_before);
  EXPECT_EQ(net.input_output_allocation_count(), alloc_before);
  EXPECT_EQ(net.InputBuffer(), in_buf);
  EXPECT_EQ(net.OutputBuffer(), out_buf);
  ExpectNchwNear(actual, expected, Oh, Ow, kAbsTol);
}

TEST_F(MetalDemosaicNetModuleTest, XTransForwardMatchesHostReferenceWithin1eMinus4) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  constexpr int H  = MetalXTransDemosaicNet::kTileInput;
  constexpr int W  = MetalXTransDemosaicNet::kTileInput;
  constexpr int Oh = MetalXTransDemosaicNet::kTileOutput;
  constexpr int Ow = MetalXTransDemosaicNet::kTileOutput;
  auto tensors     = nn::LoadSafetensors(model_dir / "xtrans.safetensors");
  const auto input = MakeDeterministicInput(3, H, W, /*seed=*/0xC7A50001u);

  MetalXTransDemosaicNet net;
  net.LoadAndCompile(tensors);
  std::vector<float> actual(static_cast<std::size_t>(3) * Oh * Ow);
  net.ForwardNchwReference(input.data(), actual.data());

  const auto expected =
      HostForward(tensors, input, H, Oh, MetalXTransDemosaicNet::kDepth,
                  MetalXTransDemosaicNet::kWidth, MetalXTransDemosaicNet::kPackOutCh,
                  MetalXTransDemosaicNet::kResidualCh);
  ExpectNchwNear(actual, expected, Oh, Ow, kAbsTol);
}

TEST_F(MetalDemosaicNetModuleTest, BayerStudentForwardMatchesExportedReference00) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_input_00.bin");
  const auto out_path  = FindGolden("bayer_output_00.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student references not found";
  }

  constexpr int N  = 1;
  constexpr int H  = MetalBayerDemosaicNet::kTileInput;
  constexpr int W  = MetalBayerDemosaicNet::kTileInput;
  constexpr int Oh = MetalBayerDemosaicNet::kTileOutput;
  constexpr int Ow = MetalBayerDemosaicNet::kTileOutput;
  const auto    hin =
      LoadFloatBin(in_path, static_cast<std::size_t>(N) * 3U * H * W);
  const auto expected =
      LoadFloatBin(out_path, static_cast<std::size_t>(N) * 3U * Oh * Ow);

  MetalBayerDemosaicNet net;
  net.LoadAndCompile(nn::LoadSafetensors(model_dir / "bayer.safetensors"));
  std::vector<float> actual(static_cast<std::size_t>(N) * 3U * Oh * Ow);
  net.ForwardNchwReference(hin.data(), actual.data());
  ExpectNchwNear(actual, expected, Oh, Ow, kAbsTol);

  // Second run reuses the same compiled executable and tile buffers.
  const auto compile_before = net.compile_count();
  const auto alloc_before   = net.input_output_allocation_count();
  MTL::Buffer* in_buf       = net.InputBuffer();
  MTL::Buffer* out_buf      = net.OutputBuffer();
  net.ForwardNchwReference(hin.data(), actual.data());
  EXPECT_EQ(net.compile_count(), compile_before);
  EXPECT_EQ(net.input_output_allocation_count(), alloc_before);
  EXPECT_EQ(net.InputBuffer(), in_buf);
  EXPECT_EQ(net.OutputBuffer(), out_buf);
  ExpectNchwNear(actual, expected, Oh, Ow, kAbsTol);
}

TEST_F(MetalDemosaicNetModuleTest, BayerStudentForwardMatchesExportedReference01) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_input_01.bin");
  const auto out_path  = FindGolden("bayer_output_01.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student references not found";
  }

  constexpr int N  = 1;
  constexpr int H  = MetalBayerDemosaicNet::kTileInput;
  constexpr int W  = MetalBayerDemosaicNet::kTileInput;
  constexpr int Oh = MetalBayerDemosaicNet::kTileOutput;
  constexpr int Ow = MetalBayerDemosaicNet::kTileOutput;
  const auto    hin =
      LoadFloatBin(in_path, static_cast<std::size_t>(N) * 3U * H * W);
  const auto expected =
      LoadFloatBin(out_path, static_cast<std::size_t>(N) * 3U * Oh * Ow);

  MetalBayerDemosaicNet net;
  net.LoadAndCompile(nn::LoadSafetensors(model_dir / "bayer.safetensors"));
  std::vector<float> actual(static_cast<std::size_t>(N) * 3U * Oh * Ow);
  net.ForwardNchwReference(hin.data(), actual.data());
  ExpectNchwNear(actual, expected, Oh, Ow, kAbsTol);
}

TEST_F(MetalDemosaicNetModuleTest, XTransStudentForwardMatchesExportedReference00) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("xtrans_input_00.bin");
  const auto out_path  = FindGolden("xtrans_output_00.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student references not found";
  }

  constexpr int N  = 1;
  constexpr int H  = MetalXTransDemosaicNet::kTileInput;
  constexpr int W  = MetalXTransDemosaicNet::kTileInput;
  constexpr int Oh = MetalXTransDemosaicNet::kTileOutput;
  constexpr int Ow = MetalXTransDemosaicNet::kTileOutput;
  const auto    hin =
      LoadFloatBin(in_path, static_cast<std::size_t>(N) * 3U * H * W);
  const auto expected =
      LoadFloatBin(out_path, static_cast<std::size_t>(N) * 3U * Oh * Ow);

  MetalXTransDemosaicNet net;
  net.LoadAndCompile(nn::LoadSafetensors(model_dir / "xtrans.safetensors"));
  std::vector<float> actual(static_cast<std::size_t>(N) * 3U * Oh * Ow);
  net.ForwardNchwReference(hin.data(), actual.data());
  ExpectNchwNear(actual, expected, Oh, Ow, kAbsTol);
}

TEST_F(MetalDemosaicNetModuleTest, XTransStudentForwardMatchesExportedReference01) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("xtrans_input_01.bin");
  const auto out_path  = FindGolden("xtrans_output_01.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student references not found";
  }

  constexpr int N  = 1;
  constexpr int H  = MetalXTransDemosaicNet::kTileInput;
  constexpr int W  = MetalXTransDemosaicNet::kTileInput;
  constexpr int Oh = MetalXTransDemosaicNet::kTileOutput;
  constexpr int Ow = MetalXTransDemosaicNet::kTileOutput;
  const auto    hin =
      LoadFloatBin(in_path, static_cast<std::size_t>(N) * 3U * H * W);
  const auto expected =
      LoadFloatBin(out_path, static_cast<std::size_t>(N) * 3U * Oh * Ow);

  MetalXTransDemosaicNet net;
  net.LoadAndCompile(nn::LoadSafetensors(model_dir / "xtrans.safetensors"));
  std::vector<float> actual(static_cast<std::size_t>(N) * 3U * Oh * Ow);
  net.ForwardNchwReference(hin.data(), actual.data());
  ExpectNchwNear(actual, expected, Oh, Ow, kAbsTol);
}

}  // namespace alcedo

#endif  // HAVE_METAL
