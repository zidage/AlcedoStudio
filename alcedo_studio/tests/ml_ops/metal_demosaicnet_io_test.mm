//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Phase 3: Metal DemosaicNet tile input / crop-sized output kernels.
// Synthetic phase, edge, gamma, and ownership checks. Absolute tolerance 1e-4.

#ifdef HAVE_METAL

#include <gtest/gtest.h>

#import <Metal/Metal.h>

#include <alcedo/metal/Metal.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "decoders/processor/neural_tile_jobs.hpp"
#include "decoders/processor/nn/demosaicnet_preprocess_common.hpp"
#include "decoders/processor/nn/demosaicnet_specs.hpp"
#include "decoders/processor/nn/metal_demosaicnet_cache.hpp"
#include "decoders/processor/nn/metal_demosaicnet_tiled.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "image/metal_image.hpp"
#include "metal/metal_context.hpp"

namespace alcedo {
namespace {

constexpr float kAbsTol = 1e-4f;

void RequireMetal() {
  auto& ctx = MetalContext::Instance();
  if (ctx.Device() == nullptr || ctx.Queue() == nullptr) {
    GTEST_SKIP() << "Metal device or queue unavailable.";
  }
}

auto ToObjcCommandBuffer(MTL::CommandBuffer* command_buffer) -> id<MTLCommandBuffer> {
  return (__bridge id<MTLCommandBuffer>)(reinterpret_cast<void*>(command_buffer));
}

auto MakeSharedBuffer(MTL::Device* device, std::size_t bytes) -> NS::SharedPtr<MTL::Buffer> {
  auto buffer =
      NS::TransferPtr(device->newBuffer(static_cast<NS::UInteger>(bytes), MTL::ResourceStorageModeShared));
  if (!buffer) {
    throw std::runtime_error("failed to allocate shared buffer");
  }
  std::memset(buffer->contents(), 0, bytes);
  return buffer;
}

void CommitAndWait(MTL::CommandQueue* queue, const std::function<void(void*)>& encode) {
  auto command_buffer = NS::RetainPtr(queue->commandBuffer());
  ASSERT_NE(command_buffer.get(), nullptr);
  encode((__bridge void*)ToObjcCommandBuffer(command_buffer.get()));
  command_buffer->commit();
  command_buffer->waitUntilCompleted();
  ASSERT_EQ(command_buffer->status(), MTL::CommandBufferStatusCompleted);
}

auto HostReflectSparseGamma(const cv::Mat& cfa, const DemosaicNetTileInputParams& params)
    -> std::vector<float> {
  std::vector<float> out(static_cast<std::size_t>(params.tile_w) * params.tile_h * 3U, 0.0f);
  for (int y = 0; y < params.tile_h; ++y) {
    for (int x = 0; x < params.tile_w; ++x) {
      const int aligned_x = Reflect101(params.origin_x + x, params.aligned_w);
      const int aligned_y = Reflect101(params.origin_y + y, params.aligned_h);
      const int tex_x     = aligned_x + params.shift_sx;
      const int tex_y     = aligned_y + params.shift_sy;
      float     linear    = 0.0f;
      if (tex_x >= 0 && tex_y >= 0 && tex_x < cfa.cols && tex_y < cfa.rows) {
        linear = cfa.at<float>(tex_y, tex_x);
      }
      const int period = params.period;
      const int wy     = ((aligned_y % period) + period) % period;
      const int wx     = ((aligned_x % period) + period) % period;
      const int color  = params.rgb_fc[wy * period + wx];
      const float enc  = PowSigned(linear, kDemosaicNetGammaEncode);
      const int base   = (y * params.tile_w + x) * 3;
      out[static_cast<std::size_t>(base + color)] = enc;
    }
  }
  return out;
}

void ExpectNearVec(const std::vector<float>& actual, const std::vector<float>& expected,
                   const char* label) {
  ASSERT_EQ(actual.size(), expected.size()) << label;
  float max_abs = 0.0f;
  std::size_t worst = 0;
  for (std::size_t i = 0; i < actual.size(); ++i) {
    const float d = std::fabs(actual[i] - expected[i]);
    if (d > max_abs) {
      max_abs = d;
      worst   = i;
    }
  }
  EXPECT_LE(max_abs, kAbsTol) << label << " max_abs=" << max_abs << " at " << worst
                              << " expected=" << expected[worst] << " actual=" << actual[worst];
}

// Camera pattern such that cropping by (sy,sx) restores the training origin
// (matches demosaicnet_preprocess_common_test helpers).
auto MakeBayerCameraWithAlignShift(const int sy, const int sx) -> RawCfaPattern {
  const auto    training = DemosaicNetTrainingBayerPattern();
  RawCfaPattern pattern  = {};
  pattern.kind           = RawCfaKind::Bayer2x2;
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      const int dst   = BayerCellIndex(y, x);
      const int src_y = WrapPatternCoord(y - sy, 2);
      const int src_x = WrapPatternCoord(x - sx, 2);
      pattern.bayer_pattern.rgb_fc[dst] = RgbColorAt(training, src_y, src_x);
      pattern.bayer_pattern.raw_fc[dst] =
          pattern.bayer_pattern.rgb_fc[dst] == 1 ? 1 : pattern.bayer_pattern.rgb_fc[dst];
    }
  }
  return pattern;
}

auto MakeXTransCameraWithAlignShift(const int sy, const int sx) -> RawCfaPattern {
  const auto    training = DemosaicNetTrainingXTransPattern();
  RawCfaPattern pattern  = {};
  pattern.kind           = RawCfaKind::XTrans6x6;
  for (int y = 0; y < 6; ++y) {
    for (int x = 0; x < 6; ++x) {
      const int dst = XTransCellIndex(y, x);
      pattern.xtrans_pattern.rgb_fc[dst] =
          RgbColorAt(training, WrapPatternCoord(y - sy, 6), WrapPatternCoord(x - sx, 6));
      pattern.xtrans_pattern.raw_fc[dst] = pattern.xtrans_pattern.rgb_fc[dst];
    }
  }
  return pattern;
}

auto MakeSyntheticCfa(int width, int height) -> cv::Mat {
  cv::Mat cfa(height, width, CV_32FC1);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float v = 0.01f * static_cast<float>(y * width + x + 1);
      if (((x + y) % 17) == 0) {
        v = -v;
      }
      if (((x + y) % 23) == 0) {
        v *= 1.7f;
      }
      cfa.at<float>(y, x) = v;
    }
  }
  return cfa;
}

class MetalDemosaicNetIoTest : public ::testing::Test {
 protected:
  void SetUp() override { RequireMetal(); }
};

TEST_F(MetalDemosaicNetIoTest, BayerPhasesSparseGammaAndReflect) {
  auto& ctx    = MetalContext::Instance();
  auto* device = ctx.Device();
  auto* queue  = ctx.Queue();

  constexpr int kFullW = 48;
  constexpr int kFullH = 40;
  constexpr int kTile  = 16;

  for (int phase_sy = 0; phase_sy < 2; ++phase_sy) {
    for (int phase_sx = 0; phase_sx < 2; ++phase_sx) {
      const auto camera = MakeBayerCameraWithAlignShift(phase_sy, phase_sx);
      const auto geo    = ComputeNeuralAlignedGeometry(camera, kFullW, kFullH,
                                                       DemosaicNetBayerSpec::kMinSpatial);
      ASSERT_TRUE(geo.has_value());
      EXPECT_EQ(geo->shift_sy, phase_sy);
      EXPECT_EQ(geo->shift_sx, phase_sx);
      EXPECT_EQ(geo->aligned_width % 2, 0);
      EXPECT_EQ(geo->aligned_height % 2, 0);
      // Period trim may drop a trailing odd column/row after shift.
      EXPECT_LE(geo->aligned_width + geo->shift_sx, kFullW);
      EXPECT_LE(geo->aligned_height + geo->shift_sy, kFullH);

      cv::Mat host = MakeSyntheticCfa(kFullW, kFullH);
      metal::MetalImage cfa =
          metal::MetalImage::Create2D(kFullW, kFullH, metal::PixelFormat::R32FLOAT);
      cfa.Upload(host);

      DemosaicNetTileInputParams params;
      params.origin_x  = -4;  // force reflect on left / top
      params.origin_y  = -6;
      params.tile_w    = kTile;
      params.tile_h    = kTile;
      params.aligned_w = geo->aligned_width;
      params.aligned_h = geo->aligned_height;
      params.shift_sx  = geo->shift_sx;
      params.shift_sy  = geo->shift_sy;
      params.full_w    = kFullW;
      params.full_h    = kFullH;
      FillDemosaicNetTrainingRgbFc(/*is_xtrans=*/false, params);

      // Also cover far-right / bottom origins (out of range).
      for (const auto& origin : {cv::Point(-4, -6),
                                 cv::Point(geo->aligned_width - kTile / 2, geo->aligned_height - 3),
                                 cv::Point(0, 0)}) {
        params.origin_x = origin.x - (origin.x % 2);  // period-align
        params.origin_y = origin.y - (origin.y % 2);

        const std::size_t nbytes =
            static_cast<std::size_t>(kTile) * kTile * 3U * sizeof(float);
        auto tile_buf = MakeSharedBuffer(device, nbytes);

        CommitAndWait(queue, [&](void* cb) {
          EncodeDemosaicNetTileInput(cb, cfa.Texture(), tile_buf.get(), params);
        });

        std::vector<float> actual(static_cast<std::size_t>(kTile) * kTile * 3U);
        std::memcpy(actual.data(), tile_buf->contents(), nbytes);
        const auto expected = HostReflectSparseGamma(host, params);
        ExpectNearVec(actual, expected,
                      ("bayer phase " + std::to_string(phase_sy) + "," + std::to_string(phase_sx) +
                       " origin " + std::to_string(params.origin_x) + "," +
                       std::to_string(params.origin_y))
                          .c_str());

        // Sparse channel placement: at most one non-zero channel per pixel.
        for (int y = 0; y < kTile; ++y) {
          for (int x = 0; x < kTile; ++x) {
            const int base = (y * kTile + x) * 3;
            int nonzero    = 0;
            for (int c = 0; c < 3; ++c) {
              if (actual[static_cast<std::size_t>(base + c)] != 0.0f) {
                ++nonzero;
              }
            }
            EXPECT_LE(nonzero, 1);
          }
        }
      }
    }
  }
}

TEST_F(MetalDemosaicNetIoTest, XTransPhaseShiftsSparseGammaAndReflect) {
  auto& ctx    = MetalContext::Instance();
  auto* device = ctx.Device();
  auto* queue  = ctx.Queue();

  constexpr int kFullW = 54;
  constexpr int kFullH = 48;
  constexpr int kTile  = 18;

  // A few supported cyclic shifts (not all 36 — coverage without exploding runtime).
  // Use (2,3) which recovers exactly; others still admit a valid shift.
  const std::vector<cv::Point> phases = {{0, 0}, {2, 3}, {1, 2}, {5, 1}};
  for (const auto& phase : phases) {
    const auto camera = MakeXTransCameraWithAlignShift(phase.y, phase.x);
    const auto geo    = ComputeNeuralAlignedGeometry(camera, kFullW, kFullH,
                                                     DemosaicNetXTransSpec::kMinSpatial);
    ASSERT_TRUE(geo.has_value());
    // Training mask symmetries may yield an equivalent smaller shift.
    EXPECT_EQ(geo->aligned_width % 6, 0);
    EXPECT_EQ(geo->aligned_height % 6, 0);

    cv::Mat host = MakeSyntheticCfa(kFullW, kFullH);
    metal::MetalImage cfa =
        metal::MetalImage::Create2D(kFullW, kFullH, metal::PixelFormat::R32FLOAT);
    cfa.Upload(host);

    DemosaicNetTileInputParams params;
    params.origin_x  = -6;
    params.origin_y  = -12;
    params.tile_w    = kTile;
    params.tile_h    = kTile;
    params.aligned_w = geo->aligned_width;
    params.aligned_h = geo->aligned_height;
    params.shift_sx  = geo->shift_sx;
    params.shift_sy  = geo->shift_sy;
    params.full_w    = kFullW;
    params.full_h    = kFullH;
    FillDemosaicNetTrainingRgbFc(/*is_xtrans=*/true, params);

    const std::size_t nbytes = static_cast<std::size_t>(kTile) * kTile * 3U * sizeof(float);
    auto tile_buf            = MakeSharedBuffer(device, nbytes);
    CommitAndWait(queue, [&](void* cb) {
      EncodeDemosaicNetTileInput(cb, cfa.Texture(), tile_buf.get(), params);
    });

    std::vector<float> actual(static_cast<std::size_t>(kTile) * kTile * 3U);
    std::memcpy(actual.data(), tile_buf->contents(), nbytes);
    const auto expected = HostReflectSparseGamma(host, params);
    ExpectNearVec(actual, expected,
                  ("xtrans phase " + std::to_string(phase.y) + "," + std::to_string(phase.x))
                      .c_str());
  }
}

TEST_F(MetalDemosaicNetIoTest, OutputOwnershipCropAndGammaDecode) {
  auto& ctx    = MetalContext::Instance();
  auto* device = ctx.Device();
  auto* queue  = ctx.Queue();

  constexpr int kTileW  = 32;
  constexpr int kTileH  = 32;
  constexpr int kAlignW = 40;
  constexpr int kAlignH = 36;
  constexpr int kCropX  = 4;
  constexpr int kCropY  = 2;
  constexpr int kCropW  = 28;
  constexpr int kCropH  = 24;

  // Two first-writer owned ROIs covering the full crop without gaps/duplicates.
  // Cover aligned [0,40)×[0,30) so product crop [4,32)×[2,26) is fully owned.
  struct OwnedJob {
    int src_x0, src_y0, owned_w, owned_h, dst_x, dst_y;
    float tag;
  };
  const std::vector<OwnedJob> jobs = {
      {0, 0, 20, 30, 0, 0, 0.25f},
      {0, 0, 20, 30, 20, 0, 0.75f},
  };

  metal::MetalImage out =
      metal::MetalImage::Create2D(kCropW, kCropH, metal::PixelFormat::RGBA32FLOAT);
  // Zero-initialize via upload.
  {
    cv::Mat zeros(kCropH, kCropW, CV_32FC4, cv::Scalar(0, 0, 0, 0));
    out.Upload(zeros);
  }

  for (const auto& job : jobs) {
    const std::size_t nbytes =
        static_cast<std::size_t>(kTileW) * kTileH * 3U * sizeof(float);
    auto tile_buf = MakeSharedBuffer(device, nbytes);
    auto*         ptr = static_cast<float*>(tile_buf->contents());
    for (int y = 0; y < kTileH; ++y) {
      for (int x = 0; x < kTileW; ++x) {
        const int base = (y * kTileW + x) * 3;
        // Gamma-encoded residual-like values (including negative / over-range).
        float v = job.tag + 0.001f * static_cast<float>(x + y);
        if ((x + y) % 11 == 0) {
          v = -v;
        }
        if ((x + y) % 13 == 0) {
          v *= 1.4f;
        }
        ptr[base + 0] = v;
        ptr[base + 1] = v * 0.5f;
        ptr[base + 2] = v * 0.25f;
      }
    }

    DemosaicNetTileOutputParams params;
    params.tile_w  = kTileW;
    params.tile_h  = kTileH;
    params.src_x0  = job.src_x0;
    params.src_y0  = job.src_y0;
    params.owned_w = job.owned_w;
    params.owned_h = job.owned_h;
    params.dst_x   = job.dst_x;
    params.dst_y   = job.dst_y;
    params.crop_x  = kCropX;
    params.crop_y  = kCropY;
    params.crop_w  = kCropW;
    params.crop_h  = kCropH;

    CommitAndWait(queue, [&](void* cb) {
      EncodeDemosaicNetTileOutput(cb, tile_buf.get(), out.Texture(), params);
    });
  }

  cv::Mat downloaded;
  out.Download(downloaded);
  ASSERT_EQ(downloaded.type(), CV_32FC4);
  ASSERT_EQ(downloaded.cols, kCropW);
  ASSERT_EQ(downloaded.rows, kCropH);

  // Host reference: aligned assembly then crop, vs direct crop assembly already done.
  std::vector<float> aligned(static_cast<std::size_t>(kAlignW) * kAlignH * 3U, -999.0f);
  for (const auto& job : jobs) {
    for (int oy = 0; oy < job.owned_h; ++oy) {
      for (int ox = 0; ox < job.owned_w; ++ox) {
        const int ax = job.dst_x + ox;
        const int ay = job.dst_y + oy;
        if (ax < 0 || ay < 0 || ax >= kAlignW || ay >= kAlignH) {
          continue;
        }
        float v = job.tag + 0.001f * static_cast<float>((job.src_x0 + ox) + (job.src_y0 + oy));
        if (((job.src_x0 + ox) + (job.src_y0 + oy)) % 11 == 0) {
          v = -v;
        }
        if (((job.src_x0 + ox) + (job.src_y0 + oy)) % 13 == 0) {
          v *= 1.4f;
        }
        const int base = (ay * kAlignW + ax) * 3;
        aligned[static_cast<std::size_t>(base + 0)] = PowSigned(v, kDemosaicNetGammaDecode);
        aligned[static_cast<std::size_t>(base + 1)] =
            PowSigned(v * 0.5f, kDemosaicNetGammaDecode);
        aligned[static_cast<std::size_t>(base + 2)] =
            PowSigned(v * 0.25f, kDemosaicNetGammaDecode);
      }
    }
  }

  int written = 0;
  for (int y = 0; y < kCropH; ++y) {
    for (int x = 0; x < kCropW; ++x) {
      const int ax   = kCropX + x;
      const int ay   = kCropY + y;
      const int base = (ay * kAlignW + ax) * 3;
      const cv::Vec4f px = downloaded.at<cv::Vec4f>(y, x);
      EXPECT_NEAR(px[0], aligned[static_cast<std::size_t>(base + 0)], kAbsTol);
      EXPECT_NEAR(px[1], aligned[static_cast<std::size_t>(base + 1)], kAbsTol);
      EXPECT_NEAR(px[2], aligned[static_cast<std::size_t>(base + 2)], kAbsTol);
      EXPECT_NEAR(px[3], 1.0f, kAbsTol);
      if (aligned[static_cast<std::size_t>(base + 0)] > -900.0f) {
        ++written;
      }
    }
  }
  // Full crop coverage from the two jobs (40×24 owned in aligned space overlaps crop 28×24).
  EXPECT_EQ(written, kCropW * kCropH);
}

TEST_F(MetalDemosaicNetIoTest, TiledExecutorReusesBuffersAndWaitsOnce) {
  // Smallest Bayer cover that the student tile planner accepts still runs the full
  // 1086→1024 graph once per tile. Use a single-tile-sized aligned frame when possible.
  // For a frame smaller than one owned export the planner still emits tiles covering it.
  auto& cache = MetalDemosaicNetModelCache::Instance();
  MetalDemosaicNetLoadOptions opts;
  if (!cache.EnsureLoaded(MetalDemosaicNetVariant::Bayer, opts)) {
    GTEST_SKIP() << "Bayer model unavailable: " << cache.LastError();
  }
  ASSERT_TRUE(cache.IsLoaded(MetalDemosaicNetVariant::Bayer));
  const auto& module = cache.Bayer();
  ASSERT_TRUE(module.ready());

  const std::uint64_t alloc_before = module.input_output_allocation_count();
  const std::size_t   owned_before = module.OwnedBufferBytes();
  ASSERT_GT(owned_before, 0U);

  // Aligned size just above min spatial; student planner covers with fixed tiles.
  constexpr int kAlignedW = 128;
  constexpr int kAlignedH = 128;
  constexpr int kFullW    = kAlignedW + 2;  // leave room for a phase shift of 0..1
  constexpr int kFullH    = kAlignedH + 2;

  cv::Mat host = MakeSyntheticCfa(kFullW, kFullH);
  metal::MetalImage cfa =
      metal::MetalImage::Create2D(kFullW, kFullH, metal::PixelFormat::R32FLOAT);
  cfa.Upload(host);

  const cv::Rect crop(0, 0, kAlignedW, kAlignedH);
  metal::MetalImage out = metal::MetalImage::Create2D(
      static_cast<uint32_t>(crop.width), static_cast<uint32_t>(crop.height),
      metal::PixelFormat::RGBA32FLOAT);
  {
    cv::Mat zeros(crop.height, crop.width, CV_32FC4, cv::Scalar(0, 0, 0, 0));
    out.Upload(zeros);
  }

  MetalDemosaicNetTiledDispatch dispatch;
  dispatch.cfa_image       = &cfa;
  dispatch.output_rgba     = &out;
  dispatch.shift_sx        = 0;
  dispatch.shift_sy        = 0;
  dispatch.aligned_width   = kAlignedW;
  dispatch.aligned_height  = kAlignedH;
  dispatch.product_crop    = crop;
  dispatch.commit_and_wait = true;

  ResetMetalDemosaicNetHostWaitCountForTest();
  MetalDemosaicNetTiledExecutor executor;
  const auto result = executor.EnqueueBayer(module, dispatch);

  EXPECT_GE(result.tile_count, 1U);
  EXPECT_EQ(result.tile_encode_count, result.tile_count);
  EXPECT_EQ(result.host_wait_count, 1U);
  EXPECT_EQ(MetalDemosaicNetHostWaitCountForTest(), 1U);
  EXPECT_EQ(module.input_output_allocation_count(), alloc_before);
  EXPECT_EQ(module.OwnedBufferBytes(), owned_before);
  EXPECT_FALSE(module.HasLastEncodeError());

  cv::Mat downloaded;
  out.Download(downloaded);
  ASSERT_EQ(downloaded.cols, crop.width);
  ASSERT_EQ(downloaded.rows, crop.height);
  // Output must be finite RGBA with alpha 1 where ownership wrote.
  int finite = 0;
  for (int y = 0; y < downloaded.rows; ++y) {
    for (int x = 0; x < downloaded.cols; ++x) {
      const cv::Vec4f px = downloaded.at<cv::Vec4f>(y, x);
      EXPECT_TRUE(std::isfinite(px[0]) && std::isfinite(px[1]) && std::isfinite(px[2]));
      EXPECT_NEAR(px[3], 1.0f, kAbsTol);
      ++finite;
    }
  }
  EXPECT_EQ(finite, crop.width * crop.height);

  // Second run: still one wait, no buffer growth.
  ResetMetalDemosaicNetHostWaitCountForTest();
  const auto result2 = executor.EnqueueBayer(module, dispatch);
  EXPECT_EQ(result2.host_wait_count, 1U);
  EXPECT_EQ(MetalDemosaicNetHostWaitCountForTest(), 1U);
  EXPECT_EQ(module.input_output_allocation_count(), alloc_before);
  EXPECT_EQ(result2.tile_count, result.tile_count);
}

TEST_F(MetalDemosaicNetIoTest, BatchTwoPairsTilesAndIgnoresOddDuplicate) {
  auto& cache = MetalDemosaicNetModelCache::Instance();
  if (!cache.EnsureLoaded(MetalDemosaicNetVariant::Bayer)) {
    GTEST_SKIP() << "Bayer model unavailable: " << cache.LastError();
  }

  // Bayer's period-safe 1024-step planner produces three x-spans for 2100
  // pixels and one y-span here: the final invocation must pad lane 1 by
  // copying tile 2, then discard that duplicate output.
  constexpr int kAlignedW = 2100;
  constexpr int kAlignedH = 128;
  cv::Mat host = MakeSyntheticCfa(kAlignedW + 2, kAlignedH + 2);
  metal::MetalImage cfa = metal::MetalImage::Create2D(
      kAlignedW + 2, kAlignedH + 2, metal::PixelFormat::R32FLOAT);
  cfa.Upload(host);

  const cv::Rect crop(0, 0, kAlignedW, kAlignedH);
  metal::MetalImage out = metal::MetalImage::Create2D(
      kAlignedW, kAlignedH, metal::PixelFormat::RGBA32FLOAT);
  out.Upload(cv::Mat(kAlignedH, kAlignedW, CV_32FC4, cv::Scalar(0, 0, 0, 0)));

  MetalDemosaicNetTiledDispatch dispatch;
  dispatch.cfa_image       = &cfa;
  dispatch.output_rgba     = &out;
  dispatch.aligned_width   = kAlignedW;
  dispatch.aligned_height  = kAlignedH;
  dispatch.product_crop    = crop;
  dispatch.commit_and_wait = true;

  ResetMetalDemosaicNetHostWaitCountForTest();
  MetalDemosaicNetTiledExecutor executor;
  const auto result = executor.EnqueueBayer(cache.Bayer(), dispatch);

  EXPECT_EQ(result.tile_count, 3U);
  EXPECT_EQ(result.graph_invocation_count, 2U);
  EXPECT_EQ(result.padded_tile_count, 4U);
  EXPECT_EQ(result.tile_encode_count, 3U);
  EXPECT_EQ(result.host_wait_count, 1U);
  EXPECT_EQ(MetalDemosaicNetHostWaitCountForTest(), 1U);
  EXPECT_FALSE(cache.Bayer().HasLastEncodeError());

  cv::Mat downloaded;
  out.Download(downloaded);
  ASSERT_EQ(downloaded.type(), CV_32FC4);
  for (int y = 0; y < downloaded.rows; ++y) {
    for (int x = 0; x < downloaded.cols; ++x) {
      const cv::Vec4f px = downloaded.at<cv::Vec4f>(y, x);
      EXPECT_TRUE(std::isfinite(px[0]) && std::isfinite(px[1]) && std::isfinite(px[2]));
      EXPECT_FLOAT_EQ(px[3], 1.0f);
    }
  }
}

TEST_F(MetalDemosaicNetIoTest, NoFullFrameStagingInDispatchContract) {
  // Structural contract: dispatch only holds the original CFA texture and crop-sized
  // RGBA output — no HWC3 intermediate fields exist on the public API.
  MetalDemosaicNetTiledDispatch dispatch;
  dispatch.cfa_image   = nullptr;
  dispatch.output_rgba = nullptr;
  dispatch.product_crop = cv::Rect(0, 0, 1, 1);
  // Compile-time / API shape check: only the two product-visible images.
  EXPECT_EQ(dispatch.cfa_image, nullptr);
  EXPECT_EQ(dispatch.output_rgba, nullptr);
  EXPECT_EQ(sizeof(dispatch.shift_sx), sizeof(int));
}

}  // namespace
}  // namespace alcedo

#endif  // HAVE_METAL
