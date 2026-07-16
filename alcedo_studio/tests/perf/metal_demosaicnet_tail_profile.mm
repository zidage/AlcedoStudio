//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Development-only profile of the fixed post/output tail. This deliberately
// keeps the tail in a separate MPSGraph executable so its cost can be compared
// with the complete tiled Neural interval without changing the product graph.

#ifdef HAVE_METAL

#include <Metal/Metal.h>
#include <MetalPerformanceShadersGraph/MetalPerformanceShadersGraph.h>

#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoders/processor/nn/metal_demosaicnet_module.hpp"
#include "metal/metal_context.hpp"
#include "nn/safetensors.hpp"

namespace alcedo {
namespace {

using Clock = std::chrono::steady_clock;

struct VariantConfig {
  const char* name;
  const char* model;
  int         depth;
  int         width;
  int         tile_input;
  int         tile_output;
  int         graph_invocations;
};

constexpr VariantConfig kBayer  = {"bayer_s24_d8", "bayer.safetensors", 8, 24, 1086, 1024, 12};
constexpr VariantConfig kXTrans = {"xtrans_p2_s32_d4", "xtrans.safetensors", 4, 32, 1048, 1024, 24};

auto                    ToObjcDevice(MTL::Device* device) -> id<MTLDevice> {
  return (__bridge id<MTLDevice>)(reinterpret_cast<void*>(device));
}

auto NSErrorMessage(NSError* error) -> std::string {
  if (error == nil) {
    return {};
  }
  const char* description = error.localizedDescription.UTF8String;
  return description != nullptr ? description : "Objective-C error without a description";
}

[[nodiscard]] auto RequireTensor(const nn::SafetensorsTensorMap& tensors, const char* name,
                                 std::initializer_list<std::int64_t> shape)
    -> const nn::SafetensorsTensor& {
  return nn::RequireF32Tensor(tensors, name, shape);
}

[[nodiscard]] auto TransposeOihwToHwio(const nn::SafetensorsTensor& oihw, int out_c, int in_c,
                                       int kh, int kw) -> std::vector<float> {
  std::vector<float> hwio(oihw.data.size());
  for (int o = 0; o < out_c; ++o) {
    for (int i = 0; i < in_c; ++i) {
      for (int h = 0; h < kh; ++h) {
        for (int w = 0; w < kw; ++w) {
          const auto src = ((((static_cast<std::size_t>(o) * in_c + i) * kh + h) * kw) + w);
          const auto dst = ((((static_cast<std::size_t>(h) * kw + w) * in_c + i) * out_c) + o);
          hwio[dst]      = oihw.data[src];
        }
      }
    }
  }
  return hwio;
}

auto ConstantHwio(MPSGraph* graph, const nn::SafetensorsTensor& oihw, int out_c, int in_c, int kh,
                  int kw) -> MPSGraphTensor* {
  const std::vector<float> hwio = TransposeOihwToHwio(oihw, out_c, in_c, kh, kw);
  NSData* data = [NSData dataWithBytes:hwio.data() length:hwio.size() * sizeof(float)];
  return [graph constantWithData:data
                           shape:@[ @(kh), @(kw), @(in_c), @(out_c) ]
                        dataType:MPSDataTypeFloat32];
}

auto ConvDescriptor() -> MPSGraphConvolution2DOpDescriptor* {
  MPSGraphConvolution2DOpDescriptor* descriptor =
      [MPSGraphConvolution2DOpDescriptor descriptorWithStrideInX:1
                                                       strideInY:1
                                                 dilationRateInX:1
                                                 dilationRateInY:1
                                                          groups:1
                                                    paddingStyle:MPSGraphPaddingStyleExplicit
                                                      dataLayout:MPSGraphTensorNamedDataLayoutNHWC
                                                   weightsLayout:MPSGraphTensorNamedDataLayoutHWIO];
  descriptor.paddingLeft   = 0;
  descriptor.paddingRight  = 0;
  descriptor.paddingTop    = 0;
  descriptor.paddingBottom = 0;
  return descriptor;
}

auto AddBiasRelu(MPSGraph* graph, MPSGraphTensor* source, const nn::SafetensorsTensor& bias,
                 int channels, NSString* prefix) -> MPSGraphTensor* {
  MPSGraphTensor* b =
      [graph constantWithData:[NSData dataWithBytes:bias.data.data()
                                             length:bias.data.size() * sizeof(float)]
                        shape:@[ @(channels) ]
                     dataType:MPSDataTypeFloat32];
  MPSGraphTensor* b4 = [graph reshapeTensor:b
                                  withShape:@[ @1, @1, @1, @(channels) ]
                                       name:[prefix stringByAppendingString:@"_bias"]];
  MPSGraphTensor* added =
      [graph additionWithPrimaryTensor:source
                       secondaryTensor:b4
                                  name:[prefix stringByAppendingString:@"_add"]];
  return [graph reLUWithTensor:added name:[prefix stringByAppendingString:@"_relu"]];
}

struct TailGraph {
  id<MTLBuffer>                          input_buffer      = nil;
  id<MTLBuffer>                          output_buffer     = nil;
  MPSGraphExecutable*                    executable        = nil;
  MPSGraphTensorData*                    input_data        = nil;
  MPSGraphTensorData*                    output_data       = nil;
  MPSGraphExecutableExecutionDescriptor* execution         = nil;
  int                                    graph_invocations = 0;
};

[[nodiscard]] auto MakeTailGraph(const VariantConfig&            config,
                                 const nn::SafetensorsTensorMap& tensors, id<MTLDevice> device)
    -> TailGraph {
  const int packed_h   = config.tile_input / 2;
  const int residual_h = packed_h - 2 * config.depth;
  const int unpacked_h = residual_h * 2;
  const int post_h     = unpacked_h - 2;
  const int final_top  = (post_h - config.tile_output) / 2;
  if (final_top < 0 || post_h < config.tile_output) {
    throw std::runtime_error("invalid fixed tail geometry");
  }

  MPSGraph*       graph        = [MPSGraph new];
  MPSShape*       input_shape  = @[ @2, @(unpacked_h), @(unpacked_h), @6 ];
  MPSShape*       output_shape = @[ @2, @(config.tile_output), @(config.tile_output), @3 ];
  MPSGraphTensor* input        = [graph placeholderWithShape:input_shape
                                             dataType:MPSDataTypeFloat32
                                                 name:@"tail_input"];

  const auto&     post_weight = RequireTensor(tensors, "post_conv.weight", {config.width, 6, 3, 3});
  const auto&     post_bias   = RequireTensor(tensors, "post_conv.bias", {config.width});
  MPSGraphTensor* post =
      [graph convolution2DWithSourceTensor:input
                             weightsTensor:ConstantHwio(graph, post_weight, config.width, 6, 3, 3)
                                descriptor:ConvDescriptor()
                                      name:@"tail_post_conv"];
  post                          = AddBiasRelu(graph, post, post_bias, config.width, @"tail_post");

  const auto&     output_weight = RequireTensor(tensors, "output.weight", {3, config.width, 1, 1});
  const auto&     output_bias   = RequireTensor(tensors, "output.bias", {3});
  MPSGraphTensor* output =
      [graph convolution2DWithSourceTensor:post
                             weightsTensor:ConstantHwio(graph, output_weight, 3, config.width, 1, 1)
                                descriptor:ConvDescriptor()
                                      name:@"tail_output_conv"];
  MPSGraphTensor* output_bias_tensor =
      [graph constantWithData:[NSData dataWithBytes:output_bias.data.data()
                                             length:output_bias.data.size() * sizeof(float)]
                        shape:@[ @3 ]
                     dataType:MPSDataTypeFloat32];
  output                                     = [graph additionWithPrimaryTensor:output
                            secondaryTensor:[graph reshapeTensor:output_bias_tensor
                                                       withShape:@[ @1, @1, @1, @3 ]
                                                            name:@"tail_output_bias"]
                                       name:@"tail_output_add"];
  output                                     = [graph
      sliceTensor:output
           starts:@[ @0, @(final_top), @(final_top), @0 ]
             ends:@[ @2, @(final_top + config.tile_output), @(final_top + config.tile_output), @3 ]
          strides:@[ @1, @1, @1, @1 ]
             name:@"tail_crop"];

  MPSGraphShapedType*            feed_type   = [[MPSGraphShapedType alloc] initWithShape:input_shape
                                                                   dataType:MPSDataTypeFloat32];
  MPSGraphCompilationDescriptor* compilation = [MPSGraphCompilationDescriptor new];
  compilation.waitForCompilationCompletion   = YES;
  MPSGraphExecutable* executable =
      [graph compileWithDevice:[MPSGraphDevice deviceWithMTLDevice:device]
                          feeds:@{input : feed_type}
                  targetTensors:@[ output ]
               targetOperations:nil
          compilationDescriptor:compilation];
  if (executable == nil) {
    throw std::runtime_error("tail graph compilation returned nil");
  }

  const std::size_t input_bytes =
      static_cast<std::size_t>(2) * unpacked_h * unpacked_h * 6 * sizeof(float);
  const std::size_t output_bytes =
      static_cast<std::size_t>(2) * config.tile_output * config.tile_output * 3 * sizeof(float);
  TailGraph result;
  result.input_buffer  = [device newBufferWithLength:input_bytes
                                            options:MTLResourceStorageModePrivate];
  result.output_buffer = [device newBufferWithLength:output_bytes
                                             options:MTLResourceStorageModePrivate];
  if (result.input_buffer == nil || result.output_buffer == nil) {
    throw std::runtime_error("tail graph buffer allocation returned nil");
  }
  result.executable        = executable;
  result.input_data        = [[MPSGraphTensorData alloc] initWithMTLBuffer:result.input_buffer
                                                              shape:input_shape
                                                           dataType:MPSDataTypeFloat32];
  result.output_data       = [[MPSGraphTensorData alloc] initWithMTLBuffer:result.output_buffer
                                                               shape:output_shape
                                                            dataType:MPSDataTypeFloat32];
  result.execution         = [MPSGraphExecutableExecutionDescriptor new];
  result.graph_invocations = config.graph_invocations;
  return result;
}

double EncodeAndWait(const TailGraph& graph, id<MTLCommandQueue> queue) {
  __block NSError* graph_error      = nil;
  graph.execution.completionHandler = ^(NSArray<MPSGraphTensorData*>*, NSError* error) {
    if (error != nil && graph_error == nil) {
      graph_error = error;
    }
  };
  id<MTLCommandBuffer> backing = [queue commandBuffer];
  if (backing == nil) {
    throw std::runtime_error("tail profile command buffer allocation returned nil");
  }
  MPSCommandBuffer* command_buffer = [MPSCommandBuffer commandBufferWithCommandBuffer:backing];
  if (command_buffer == nil) {
    throw std::runtime_error("tail profile MPSCommandBuffer creation returned nil");
  }
  for (int i = 0; i < graph.graph_invocations; ++i) {
    NSArray<MPSGraphTensorData*>* result =
        [graph.executable encodeToCommandBuffer:command_buffer
                                    inputsArray:@[ graph.input_data ]
                                   resultsArray:@[ graph.output_data ]
                            executionDescriptor:graph.execution];
    if (result == nil) {
      throw std::runtime_error("tail profile graph encode returned nil");
    }
  }
  const auto start = Clock::now();
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  if (graph_error != nil) {
    throw std::runtime_error("tail profile graph execution failed: " + NSErrorMessage(graph_error));
  }
  if (command_buffer.status != MTLCommandBufferStatusCompleted || command_buffer.error != nil) {
    throw std::runtime_error("tail profile Metal command failed: " +
                             NSErrorMessage(command_buffer.error));
  }
  return elapsed;
}

auto FindModelDir() -> std::filesystem::path {
#ifdef ALCEDO_DEMOASICNET_MODEL_DIR
  return std::filesystem::path(ALCEDO_DEMOASICNET_MODEL_DIR);
#else
  return std::filesystem::path("alcedo_studio/src/config/models");
#endif
}

}  // namespace
}  // namespace alcedo

int main(int argc, char** argv) {
  try {
    std::string variant    = "bayer";
    int         iterations = 3;
    double      full_ms    = 0.0;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--variant" && i + 1 < argc) {
        variant = argv[++i];
      } else if (arg == "--iterations" && i + 1 < argc) {
        iterations = std::stoi(argv[++i]);
      } else if (arg == "--full-ms" && i + 1 < argc) {
        full_ms = std::stod(argv[++i]);
      } else {
        throw std::runtime_error("usage: MetalDemosaicNetTailProfile [--variant bayer|xtrans] "
                                 "[--iterations N] [--full-ms N]");
      }
    }

    const auto& config = variant == "bayer" ? alcedo::kBayer : alcedo::kXTrans;
    if (variant != "bayer" && variant != "xtrans") {
      throw std::runtime_error("--variant must be bayer or xtrans");
    }
    auto& context = alcedo::MetalContext::Instance();
    if (context.Device() == nullptr || context.Queue() == nullptr) {
      throw std::runtime_error("Metal device or queue unavailable");
    }
    const auto        model = alcedo::nn::LoadSafetensors(alcedo::FindModelDir() / config.model);
    alcedo::TailGraph tail =
        alcedo::MakeTailGraph(config, model, alcedo::ToObjcDevice(context.Device()));
    id<MTLCommandQueue> queue =
        (__bridge id<MTLCommandQueue>)(reinterpret_cast<void*>(context.Queue()));

    (void)alcedo::EncodeAndWait(tail, queue);
    std::vector<double> samples;
    for (int i = 0; i < iterations; ++i) {
      samples.push_back(alcedo::EncodeAndWait(tail, queue));
    }
    const double mean =
        std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
    const double frame_tail = mean;
    const double ratio      = full_ms > 0.0 ? frame_tail / full_ms * 100.0 : 0.0;
    std::cout << std::fixed << std::setprecision(3) << "variant=" << config.name
              << " invocations=" << config.graph_invocations << " tail_ms=[";
    for (std::size_t i = 0; i < samples.size(); ++i) {
      if (i != 0) std::cout << ", ";
      std::cout << samples[i];
    }
    std::cout << "] mean=" << mean << " ms"
              << " tail_frame=" << frame_tail << " ms";
    if (full_ms > 0.0) {
      std::cout << " full_neural=" << full_ms << " ms ratio=" << ratio << "%";
    }
    std::cout << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}

#else

int main() { return 0; }

#endif
