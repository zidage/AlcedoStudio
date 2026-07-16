//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "decoders/processor/nn/metal_demosaicnet_tiled.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <MetalPerformanceShadersGraph/MetalPerformanceShadersGraph.h>

#include <alcedo/metal/Metal.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

#include "decoders/processor/neural_tile_jobs.hpp"
#include "decoders/processor/nn/demosaicnet_preprocess_common.hpp"
#include "metal/compute_pipeline_cache.hpp"
#include "metal/metal_context.hpp"

namespace alcedo {
namespace {

std::atomic<std::uint64_t> g_host_wait_count{0};

auto ToObjcQueue(MTL::CommandQueue* queue) -> id<MTLCommandQueue> {
  return (__bridge id<MTLCommandQueue>)(reinterpret_cast<void*>(queue));
}

auto ToObjcBuffer(MTL::Buffer* buffer) -> id<MTLBuffer> {
  return (__bridge id<MTLBuffer>)(reinterpret_cast<void*>(buffer));
}

auto ToObjcTexture(MTL::Texture* texture) -> id<MTLTexture> {
  return (__bridge id<MTLTexture>)(reinterpret_cast<void*>(texture));
}

auto NSErrorMessage(NSError* error) -> std::string {
  if (error == nil) {
    return {};
  }
  const char* description = error.localizedDescription.UTF8String;
  return description != nullptr ? description : "Objective-C error without a description";
}

auto NSExceptionMessage(NSException* exception) -> std::string {
  if (exception == nil) {
    return "Objective-C exception without an object";
  }
  const char* name   = exception.name.UTF8String;
  const char* reason = exception.reason.UTF8String;
  std::string message = name != nullptr ? name : "Objective-C exception";
  if (reason != nullptr && reason[0] != '\0') {
    message += ": ";
    message += reason;
  }
  return message;
}

void RunObjc(const char* stage, const std::function<void()>& body) {
  @try {
    body();
  } @catch (NSException* exception) {
    throw std::runtime_error(std::string("Metal Neural Engine failed (stage=") + stage +
                             "): " + NSExceptionMessage(exception));
  }
}

[[noreturn]] void ThrowStage(const char* stage, const std::string& detail,
                             const char* variant = nullptr) {
  std::string message = "Metal Neural Engine failed (stage=";
  message += stage;
  if (variant != nullptr) {
    message += ", variant=";
    message += variant;
  }
  message += "): ";
  message += detail;
  throw std::runtime_error(message);
}

auto GetIoPipeline(const char* function_name) -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_DEMOSAICNET_IO_METALLIB_PATH
  (void)function_name;
  ThrowStage("prepare", "demosaicnet_io metallib path is not configured");
#else
  return metal::ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_DEMOSAICNET_IO_METALLIB_PATH, function_name, "Metal DemosaicNet IO");
#endif
}

void Dispatch2D(id<MTLComputeCommandEncoder> encoder, id<MTLComputePipelineState> pipeline,
                int width, int height) {
  const NSUInteger thread_width =
      std::max<NSUInteger>(1, pipeline.threadExecutionWidth);
  const NSUInteger thread_height =
      std::max<NSUInteger>(1, pipeline.maxTotalThreadsPerThreadgroup / thread_width);
  const MTLSize threads_per_threadgroup = MTLSizeMake(thread_width, thread_height, 1);
  const MTLSize threads_per_grid =
      MTLSizeMake(static_cast<NSUInteger>(std::max(width, 1)),
                  static_cast<NSUInteger>(std::max(height, 1)), 1);
  [encoder dispatchThreads:threads_per_grid threadsPerThreadgroup:threads_per_threadgroup];
}

void EncodeTileInputImpl(id<MTLCommandBuffer> command_buffer, id<MTLTexture> cfa,
                         id<MTLBuffer> tile_nhwc, const DemosaicNetTileInputParams& params) {
  if (command_buffer == nil || cfa == nil || tile_nhwc == nil) {
    ThrowStage("tile_input", "null command buffer, texture, or tile buffer");
  }
  if (params.tile_w <= 0 || params.tile_h <= 0 || params.aligned_w <= 0 || params.aligned_h <= 0 ||
      params.period <= 0 || params.period > 36) {
    ThrowStage("tile_input", "invalid tile input geometry");
  }

  auto pipeline_cpp = GetIoPipeline("demosaicnet_tile_input_nhwc");
  id<MTLComputePipelineState> pipeline =
      (__bridge id<MTLComputePipelineState>)(reinterpret_cast<void*>(pipeline_cpp.get()));

  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) {
    ThrowStage("tile_input", "failed to create compute encoder");
  }
  encoder.label = @"DemosaicNet Tile Input";
  [encoder setComputePipelineState:pipeline];
  [encoder setTexture:cfa atIndex:0];
  [encoder setBuffer:tile_nhwc offset:0 atIndex:0];
  [encoder setBytes:&params length:sizeof(params) atIndex:1];
  Dispatch2D(encoder, pipeline, params.tile_w, params.tile_h);
  [encoder endEncoding];
}

void EncodeTileOutputImpl(id<MTLCommandBuffer> command_buffer, id<MTLBuffer> tile_nhwc,
                          id<MTLTexture> output_rgba, const DemosaicNetTileOutputParams& params) {
  if (command_buffer == nil || tile_nhwc == nil || output_rgba == nil) {
    ThrowStage("tile_output", "null command buffer, tile buffer, or output texture");
  }
  if (params.owned_w <= 0 || params.owned_h <= 0 || params.tile_w <= 0 || params.tile_h <= 0 ||
      params.crop_w <= 0 || params.crop_h <= 0) {
    ThrowStage("tile_output", "invalid tile output geometry");
  }

  auto pipeline_cpp = GetIoPipeline("demosaicnet_tile_output_rgba");
  id<MTLComputePipelineState> pipeline =
      (__bridge id<MTLComputePipelineState>)(reinterpret_cast<void*>(pipeline_cpp.get()));

  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) {
    ThrowStage("tile_output", "failed to create compute encoder");
  }
  encoder.label = @"DemosaicNet Tile Output";
  [encoder setComputePipelineState:pipeline];
  [encoder setBuffer:tile_nhwc offset:0 atIndex:0];
  [encoder setTexture:output_rgba atIndex:0];
  [encoder setBytes:&params length:sizeof(params) atIndex:1];
  Dispatch2D(encoder, pipeline, params.owned_w, params.owned_h);
  [encoder endEncoding];
}

void ValidateDispatch(const MetalDemosaicNetTiledDispatch& dispatch, int cfa_period,
                      const char* variant) {
  if (dispatch.cfa_image == nullptr || dispatch.cfa_image->Empty()) {
    ThrowStage("prepare", "CFA image is null or empty", variant);
  }
  if (dispatch.cfa_image->Format() != metal::PixelFormat::R32FLOAT) {
    ThrowStage("prepare", "CFA image must be R32FLOAT", variant);
  }
  if (dispatch.output_rgba == nullptr || dispatch.output_rgba->Empty()) {
    ThrowStage("prepare", "output RGBA image is null or empty", variant);
  }
  if (dispatch.output_rgba->Format() != metal::PixelFormat::RGBA32FLOAT) {
    ThrowStage("prepare", "output image must be RGBA32FLOAT", variant);
  }
  if (dispatch.aligned_width <= 0 || dispatch.aligned_height <= 0) {
    ThrowStage("prepare", "aligned dimensions must be positive", variant);
  }
  if ((dispatch.aligned_width % cfa_period) != 0 || (dispatch.aligned_height % cfa_period) != 0) {
    ThrowStage("prepare", "aligned dimensions violate CFA period", variant);
  }
  if (dispatch.product_crop.width <= 0 || dispatch.product_crop.height <= 0) {
    ThrowStage("prepare", "product crop is empty", variant);
  }
  if (static_cast<int>(dispatch.output_rgba->Width()) != dispatch.product_crop.width ||
      static_cast<int>(dispatch.output_rgba->Height()) != dispatch.product_crop.height) {
    ThrowStage("prepare", "output texture size must match product crop", variant);
  }
  const int full_w = static_cast<int>(dispatch.cfa_image->Width());
  const int full_h = static_cast<int>(dispatch.cfa_image->Height());
  if (dispatch.shift_sx < 0 || dispatch.shift_sy < 0 ||
      dispatch.shift_sx + dispatch.aligned_width > full_w ||
      dispatch.shift_sy + dispatch.aligned_height > full_h) {
    ThrowStage("prepare", "phase shift + aligned size exceeds CFA texture", variant);
  }
}

template <typename Module>
auto EnqueueTiles(const Module& module, const MetalDemosaicNetTiledDispatch& dispatch,
                  const detail::NeuralTilePolicy& policy, bool is_xtrans, const char* variant)
    -> MetalDemosaicNetTiledResult {
  if (!module.ready()) {
    ThrowStage("prepare", "module is not ready", variant);
  }
  ValidateDispatch(dispatch, Module::kCfaPeriod, variant);

  MTL::Buffer* input_buffer  = module.InputBuffer();
  MTL::Buffer* output_buffer = module.OutputBuffer();
  if (input_buffer == nullptr || output_buffer == nullptr) {
    ThrowStage("prepare", "module tile buffers are null", variant);
  }

  // Warm ComputePipelineCache before the timed tile loop.
  (void)GetIoPipeline("demosaicnet_tile_input_nhwc");
  (void)GetIoPipeline("demosaicnet_tile_output_rgba");

  const auto jobs =
      detail::BuildTileJobs(cv::Rect(0, 0, dispatch.aligned_width, dispatch.aligned_height),
                            cv::Size(dispatch.aligned_width, dispatch.aligned_height), policy);

  DemosaicNetTileInputParams input_params;
  input_params.tile_w    = Module::kTileInput;
  input_params.tile_h    = Module::kTileInput;
  input_params.aligned_w = dispatch.aligned_width;
  input_params.aligned_h = dispatch.aligned_height;
  input_params.shift_sx  = dispatch.shift_sx;
  input_params.shift_sy  = dispatch.shift_sy;
  input_params.full_w    = static_cast<int>(dispatch.cfa_image->Width());
  input_params.full_h    = static_cast<int>(dispatch.cfa_image->Height());
  FillDemosaicNetTrainingRgbFc(is_xtrans, input_params);

  DemosaicNetTileOutputParams output_params;
  output_params.tile_w = Module::kTileOutput;
  output_params.tile_h = Module::kTileOutput;
  output_params.crop_x = dispatch.product_crop.x;
  output_params.crop_y = dispatch.product_crop.y;
  output_params.crop_w = dispatch.product_crop.width;
  output_params.crop_h = dispatch.product_crop.height;

  MetalDemosaicNetTiledResult result;
  result.tile_count = jobs.size();

  module.ClearLastEncodeError();

  auto& ctx = MetalContext::Instance();
  if (ctx.Device() == nullptr || ctx.Queue() == nullptr) {
    ThrowStage("prepare", "MetalContext device or queue is null", variant);
  }

  RunObjc("graph_encode", [&] {
    id<MTLCommandQueue> queue = ToObjcQueue(ctx.Queue());
    MPSCommandBuffer* mps_cb  = [MPSCommandBuffer commandBufferFromCommandQueue:queue];
    if (mps_cb == nil) {
      ThrowStage("prepare", "failed to create MPSCommandBuffer", variant);
    }

    id<MTLTexture> cfa_tex = ToObjcTexture(dispatch.cfa_image->Texture());
    id<MTLTexture> out_tex = ToObjcTexture(dispatch.output_rgba->Texture());
    id<MTLBuffer>  in_buf  = ToObjcBuffer(input_buffer);
    id<MTLBuffer>  out_buf = ToObjcBuffer(output_buffer);

    // Queue ordering makes one input/output buffer pair safe across tiles.
    // There is deliberately no host wait inside this loop.
    for (const auto& job : jobs) {
      if (job.input_w != Module::kTileInput || job.input_h != Module::kTileInput ||
          job.owned_w != Module::kTileOutput || job.owned_h != Module::kTileOutput ||
          (job.input_origin.x % Module::kCfaPeriod) != 0 ||
          (job.input_origin.y % Module::kCfaPeriod) != 0) {
        ThrowStage("prepare", "invalid shared tile job geometry", variant);
      }

      input_params.origin_x = job.input_origin.x;
      input_params.origin_y = job.input_origin.y;
      EncodeTileInputImpl(mps_cb, cfa_tex, in_buf, input_params);

      module.EncodeOnMpsCommandBuffer((__bridge void*)mps_cb);

      output_params.src_x0  = job.model_output_roi.x;
      output_params.src_y0  = job.model_output_roi.y;
      output_params.owned_w = job.model_output_roi.width;
      output_params.owned_h = job.model_output_roi.height;
      output_params.dst_x   = job.destination_roi.x;
      output_params.dst_y   = job.destination_roi.y;
      EncodeTileOutputImpl(mps_cb, out_buf, out_tex, output_params);

      ++result.tile_encode_count;
    }

    [mps_cb commit];
    if (dispatch.commit_and_wait) {
      [mps_cb waitUntilCompleted];
      ++result.host_wait_count;
      g_host_wait_count.fetch_add(1, std::memory_order_relaxed);

      if (module.HasLastEncodeError()) {
        ThrowStage("graph_execute", module.LastEncodeErrorMessage(), variant);
      }
      if (mps_cb.status != MTLCommandBufferStatusCompleted || mps_cb.error != nil) {
        ThrowStage("graph_execute",
                   mps_cb.error != nil ? NSErrorMessage(mps_cb.error)
                                       : std::string("command did not complete"),
                   variant);
      }
    }
  });

  return result;
}

}  // namespace

void FillDemosaicNetTrainingRgbFc(bool is_xtrans, DemosaicNetTileInputParams& params) {
  if (is_xtrans) {
    params.period = 6;
    for (int i = 0; i < 36; ++i) {
      params.rgb_fc[i] = kDemosaicNetXTransTargetRgb[i];
    }
  } else {
    params.period = 2;
    for (int i = 0; i < 4; ++i) {
      params.rgb_fc[i] = kDemosaicNetBayerTargetRgb[i];
    }
    for (int i = 4; i < 36; ++i) {
      params.rgb_fc[i] = 0;
    }
  }
}

void EncodeDemosaicNetTileInput(void* mtl_command_buffer, MTL::Texture* cfa_texture,
                                MTL::Buffer* tile_nhwc, const DemosaicNetTileInputParams& params) {
  RunObjc("tile_input", [&] {
    EncodeTileInputImpl((__bridge id<MTLCommandBuffer>)mtl_command_buffer,
                        ToObjcTexture(cfa_texture), ToObjcBuffer(tile_nhwc), params);
  });
}

void EncodeDemosaicNetTileOutput(void* mtl_command_buffer, MTL::Buffer* tile_nhwc,
                                 MTL::Texture* output_rgba,
                                 const DemosaicNetTileOutputParams& params) {
  RunObjc("tile_output", [&] {
    EncodeTileOutputImpl((__bridge id<MTLCommandBuffer>)mtl_command_buffer,
                         ToObjcBuffer(tile_nhwc), ToObjcTexture(output_rgba), params);
  });
}

void ResetMetalDemosaicNetHostWaitCountForTest() {
  g_host_wait_count.store(0, std::memory_order_relaxed);
}

auto MetalDemosaicNetHostWaitCountForTest() noexcept -> std::uint64_t {
  return g_host_wait_count.load(std::memory_order_relaxed);
}

struct MetalDemosaicNetTiledExecutor::Impl {};

MetalDemosaicNetTiledExecutor::MetalDemosaicNetTiledExecutor()
    : impl_(std::make_unique<Impl>()) {}
MetalDemosaicNetTiledExecutor::~MetalDemosaicNetTiledExecutor() = default;
MetalDemosaicNetTiledExecutor::MetalDemosaicNetTiledExecutor(
    MetalDemosaicNetTiledExecutor&&) noexcept = default;
MetalDemosaicNetTiledExecutor& MetalDemosaicNetTiledExecutor::operator=(
    MetalDemosaicNetTiledExecutor&&) noexcept = default;

auto MetalDemosaicNetTiledExecutor::EnqueueBayer(const MetalBayerDemosaicNet& module,
                                                 const MetalDemosaicNetTiledDispatch& dispatch)
    -> MetalDemosaicNetTiledResult {
  (void)impl_;
  return EnqueueTiles(module, dispatch, detail::MakeBayerStudentTilePolicy(), /*is_xtrans=*/false,
                      MetalBayerDemosaicNet::kArchitecture);
}

auto MetalDemosaicNetTiledExecutor::EnqueueXTrans(const MetalXTransDemosaicNet& module,
                                                  const MetalDemosaicNetTiledDispatch& dispatch)
    -> MetalDemosaicNetTiledResult {
  (void)impl_;
  return EnqueueTiles(module, dispatch, detail::MakeXTransStudentTilePolicy(), /*is_xtrans=*/true,
                      MetalXTransDemosaicNet::kArchitecture);
}

}  // namespace alcedo

#endif  // HAVE_METAL
