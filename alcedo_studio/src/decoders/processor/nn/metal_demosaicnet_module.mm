//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "decoders/processor/nn/metal_demosaicnet_module.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <MetalPerformanceShadersGraph/MetalPerformanceShadersGraph.h>

#include <alcedo/metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "metal/metal_context.hpp"

namespace alcedo {
namespace {

// ---------------------------------------------------------------------------
// ObjC / metal-cpp bridges
// ---------------------------------------------------------------------------

auto ToObjcDevice(MTL::Device* device) -> id<MTLDevice> {
  return (__bridge id<MTLDevice>)(reinterpret_cast<void*>(device));
}

auto ToObjcBuffer(MTL::Buffer* buffer) -> id<MTLBuffer> {
  return (__bridge id<MTLBuffer>)(reinterpret_cast<void*>(buffer));
}

auto ToObjcCommandBuffer(MTL::CommandBuffer* command_buffer) -> id<MTLCommandBuffer> {
  return (__bridge id<MTLCommandBuffer>)(reinterpret_cast<void*>(command_buffer));
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

// ---------------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------------

void RequireMetadata(const nn::SafetensorsTensorMap& tensors, std::string_view key,
                     std::string_view expected, const char* module) {
  const auto actual = tensors.metadata(key);
  if (actual != expected) {
    throw std::runtime_error(std::string(module) + ": metadata '" + std::string(key) +
                             "' expected '" + std::string(expected) + "', got '" +
                             std::string(actual) + "'");
  }
}

void RequireExactHostWeight(const nn::SafetensorsTensor& host, const std::vector<float>& expected,
                            std::string_view key, const char* module) {
  if (host.data.size() != expected.size()) {
    throw std::runtime_error(std::string(module) + ": fixed weight size mismatch for " +
                             std::string(key));
  }
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (std::fabs(host.data[i] - expected[i]) > 0.0f) {
      throw std::runtime_error(std::string(module) + ": fixed one-hot mismatch for " +
                               std::string(key) + " at index " + std::to_string(i));
    }
  }
}

void RequireAllF32(const nn::SafetensorsTensorMap& tensors, const char* module) {
  for (const auto& [name, tensor] : tensors) {
    if (tensor.dtype != nn::SafetensorsTensor::Dtype::F32) {
      throw std::runtime_error(std::string(module) + ": non-FP32 tensor '" + name + "'");
    }
  }
}

[[nodiscard]] auto ExpectedBayerPackWeight() -> std::vector<float> {
  std::vector<float> w(4 * 3 * 2 * 2, 0.0f);
  for (int py = 0; py < 2; ++py) {
    for (int px = 0; px < 2; ++px) {
      const int out_i = py * 2 + px;
      for (int c = 0; c < 3; ++c) {
        w[((out_i * 3 + c) * 2 + py) * 2 + px] = 1.0f;
      }
    }
  }
  return w;
}

[[nodiscard]] auto ExpectedXTransPackWeight() -> std::vector<float> {
  std::vector<float> w(12 * 3 * 2 * 2, 0.0f);
  for (int c = 0; c < 3; ++c) {
    for (int py = 0; py < 2; ++py) {
      for (int px = 0; px < 2; ++px) {
        const int out_i                        = c * 4 + py * 2 + px;
        w[((out_i * 3 + c) * 2 + py) * 2 + px] = 1.0f;
      }
    }
  }
  return w;
}

[[nodiscard]] auto ExpectedUnpackWeight() -> std::vector<float> {
  std::vector<float> w(12 * 1 * 2 * 2, 0.0f);
  for (int g = 0; g < 3; ++g) {
    for (int py = 0; py < 2; ++py) {
      for (int px = 0; px < 2; ++px) {
        const int in_i              = g * 4 + py * 2 + px;
        w[(in_i * 2 + py) * 2 + px] = 1.0f;
      }
    }
  }
  return w;
}

[[nodiscard]] auto ResolveDevice(MTL::Device* device) -> MTL::Device* {
  if (device != nullptr) {
    return device;
  }
  auto& ctx = MetalContext::Instance();
  if (ctx.Device() == nullptr) {
    throw std::runtime_error("Metal Neural Engine failed (stage=load): MetalContext device is null");
  }
  return ctx.Device();
}

[[nodiscard]] auto ResolveQueue() -> MTL::CommandQueue* {
  auto& ctx = MetalContext::Instance();
  if (ctx.Queue() == nullptr) {
    throw std::runtime_error(
        "Metal Neural Engine failed (stage=graph_execute): MetalContext queue is null");
  }
  return ctx.Queue();
}

// ---------------------------------------------------------------------------
// Graph geometry for fixed product tiles
// ---------------------------------------------------------------------------

struct GraphGeometry {
  int tile_input  = 0;
  int tile_output = 0;
  int depth       = 0;
  int width       = 0;
  int pack_out_ch = 0;
  int residual_ch = 0;
  int pack_factor = 2;

  int packed_h    = 0;
  int residual_h  = 0;
  int unpacked_h  = 0;
  int post_h      = 0;

  int skip_top    = 0;
  int final_top   = 0;
};

[[nodiscard]] auto MakeGeometry(int tile_input, int tile_output, int depth, int width, int pack_out_ch,
                                int residual_ch, int pack_factor) -> GraphGeometry {
  GraphGeometry g;
  g.tile_input  = tile_input;
  g.tile_output = tile_output;
  g.depth       = depth;
  g.width       = width;
  g.pack_out_ch = pack_out_ch;
  g.residual_ch = residual_ch;
  g.pack_factor = pack_factor;
  g.packed_h    = tile_input / pack_factor;
  g.residual_h  = g.packed_h - 2 * depth;
  g.unpacked_h  = g.residual_h * pack_factor;
  g.post_h      = g.unpacked_h - 2;
  g.skip_top    = (tile_input - g.unpacked_h) / 2;
  g.final_top   = (g.post_h - tile_output) / 2;
  if ((tile_input % pack_factor) != 0 || g.residual_h < 1 || g.post_h < tile_output ||
      ((tile_input - g.unpacked_h) % 2) != 0 || ((g.post_h - tile_output) % 2) != 0) {
    throw std::runtime_error("Metal Neural Engine failed (stage=compile): invalid fixed geometry");
  }
  return g;
}

// ---------------------------------------------------------------------------
// Shared MPSGraph module state
// ---------------------------------------------------------------------------

struct GraphModule {
  bool ready = false;

  GraphGeometry geometry;

  NS::SharedPtr<MTL::Buffer> input_buffer;
  NS::SharedPtr<MTL::Buffer> output_buffer;

  // ARC-retained graph objects. Kept alive for the life of the executable.
  // Move transfers ownership by nulling the source so the moved-from destructor
  // does not release the live objects.
  MPSGraph*                              graph         = nil;
  MPSGraphTensor*                        input_tensor  = nil;
  MPSGraphTensor*                        output_tensor = nil;
  MPSGraphExecutable*                    executable    = nil;
  MPSGraphTensorData*                    input_data    = nil;
  MPSGraphTensorData*                    output_data   = nil;
  MPSGraphExecutableExecutionDescriptor* execution     = nil;

  std::size_t   resident_weight_bytes         = 0;
  std::uint64_t compile_count                 = 0;
  std::uint64_t input_output_allocation_count = 0;

  // First encode error retained until the host inspects it after wait.
  mutable NSError* last_encode_error = nil;

  GraphModule() = default;
  ~GraphModule() { Reset(); }

  GraphModule(const GraphModule&)            = delete;
  GraphModule& operator=(const GraphModule&) = delete;

  GraphModule(GraphModule&& other) noexcept { *this = std::move(other); }

  GraphModule& operator=(GraphModule&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    Reset();
    ready                         = other.ready;
    geometry                      = other.geometry;
    input_buffer                  = std::move(other.input_buffer);
    output_buffer                 = std::move(other.output_buffer);
    graph                         = other.graph;
    input_tensor                  = other.input_tensor;
    output_tensor                 = other.output_tensor;
    executable                    = other.executable;
    input_data                    = other.input_data;
    output_data                   = other.output_data;
    execution                     = other.execution;
    resident_weight_bytes         = other.resident_weight_bytes;
    compile_count                 = other.compile_count;
    input_output_allocation_count = other.input_output_allocation_count;
    last_encode_error             = other.last_encode_error;

    other.ready                         = false;
    other.geometry                      = {};
    other.graph                         = nil;
    other.input_tensor                  = nil;
    other.output_tensor                 = nil;
    other.executable                    = nil;
    other.input_data                    = nil;
    other.output_data                   = nil;
    other.execution                     = nil;
    other.resident_weight_bytes         = 0;
    other.compile_count                 = 0;
    other.input_output_allocation_count = 0;
    other.last_encode_error             = nil;
    return *this;
  }

  void Reset() {
    ready                         = false;
    geometry                      = {};
    input_buffer.reset();
    output_buffer.reset();
    graph                         = nil;
    input_tensor                  = nil;
    output_tensor                 = nil;
    executable                    = nil;
    input_data                    = nil;
    output_data                   = nil;
    execution                     = nil;
    resident_weight_bytes         = 0;
    compile_count                 = 0;
    input_output_allocation_count = 0;
    last_encode_error             = nil;
  }
};

auto MakeShape(int n, int h, int w, int c) -> MPSShape* {
  return @[ @(n), @(h), @(w), @(c) ];
}

auto ConstantFromTensor(MPSGraph* graph, const nn::SafetensorsTensor& host, MPSShape* shape)
    -> MPSGraphTensor* {
  NSData* data = [NSData dataWithBytes:host.data.data()
                                length:host.data.size() * sizeof(float)];
  return [graph constantWithData:data shape:shape dataType:MPSDataTypeFloat32];
}

auto ValidConvDescriptor(NSUInteger stride_x, NSUInteger stride_y)
    -> MPSGraphConvolution2DOpDescriptor* {
  MPSGraphConvolution2DOpDescriptor* desc = [MPSGraphConvolution2DOpDescriptor
      descriptorWithStrideInX:stride_x
                    strideInY:stride_y
              dilationRateInX:1
              dilationRateInY:1
                       groups:1
                 paddingStyle:MPSGraphPaddingStyleExplicit
                   dataLayout:MPSGraphTensorNamedDataLayoutNHWC
                weightsLayout:MPSGraphTensorNamedDataLayoutOIHW];
  desc.paddingLeft   = 0;
  desc.paddingRight  = 0;
  desc.paddingTop    = 0;
  desc.paddingBottom = 0;
  return desc;
}

auto ConvBiasRelu(MPSGraph* graph, MPSGraphTensor* source, const nn::SafetensorsTensor& weight,
                  const nn::SafetensorsTensor& bias, int out_c, int in_c, int kh, int kw,
                  NSUInteger stride, bool apply_relu, NSString* name_prefix) -> MPSGraphTensor* {
  MPSShape* weight_shape = @[ @(out_c), @(in_c), @(kh), @(kw) ];
  MPSGraphTensor* w      = ConstantFromTensor(graph, weight, weight_shape);
  MPSGraphTensor* conv =
      [graph convolution2DWithSourceTensor:source
                             weightsTensor:w
                                descriptor:ValidConvDescriptor(stride, stride)
                                      name:[name_prefix stringByAppendingString:@"_conv"]];
  MPSGraphTensor* b = ConstantFromTensor(graph, bias, @[ @(out_c) ]);
  MPSGraphTensor* b4 =
      [graph reshapeTensor:b
                 withShape:@[ @1, @1, @1, @(out_c) ]
                      name:[name_prefix stringByAppendingString:@"_b4"]];
  MPSGraphTensor* added =
      [graph additionWithPrimaryTensor:conv
                       secondaryTensor:b4
                                  name:[name_prefix stringByAppendingString:@"_add"]];
  if (!apply_relu) {
    return added;
  }
  return [graph reLUWithTensor:added name:[name_prefix stringByAppendingString:@"_relu"]];
}

auto ExplicitResidualUnpack(MPSGraph* graph, MPSGraphTensor* residual_nhwc12, int residual_h,
                            NSString* name_prefix) -> MPSGraphTensor* {
  // residual: [1, H, W, 12] → [1, H, W, 3, 2, 2] → [1, H, 2, W, 2, 3] → [1, 2H, 2W, 3]
  MPSGraphTensor* packed =
      [graph reshapeTensor:residual_nhwc12
                 withShape:@[ @1, @(residual_h), @(residual_h), @3, @2, @2 ]
                      name:[name_prefix stringByAppendingString:@"_pack"]];
  MPSGraphTensor* transposed =
      [graph transposeTensor:packed
                 permutation:@[ @0, @1, @4, @2, @5, @3 ]
                        name:[name_prefix stringByAppendingString:@"_tr"]];
  const int up = residual_h * 2;
  return [graph reshapeTensor:transposed
                    withShape:@[ @1, @(up), @(up), @3 ]
                         name:[name_prefix stringByAppendingString:@"_up"]];
}

auto CenterSliceNHWC(MPSGraph* graph, MPSGraphTensor* source, int source_h, int out_h, int top,
                     NSString* name) -> MPSGraphTensor* {
  (void)source_h;
  return [graph sliceTensor:source
                     starts:@[ @0, @(top), @(top), @0 ]
                       ends:@[ @1, @(top + out_h), @(top + out_h), @3 ]
                    strides:@[ @1, @1, @1, @1 ]
                       name:name];
}

void BuildGraph(GraphModule& module, const nn::SafetensorsTensorMap& tensors, bool is_bayer,
                const char* module_name) {
  const int depth       = is_bayer ? MetalBayerDemosaicNet::kDepth : MetalXTransDemosaicNet::kDepth;
  const int width       = is_bayer ? MetalBayerDemosaicNet::kWidth : MetalXTransDemosaicNet::kWidth;
  const int pack_out_ch =
      is_bayer ? MetalBayerDemosaicNet::kPackOutCh : MetalXTransDemosaicNet::kPackOutCh;
  const int residual_ch =
      is_bayer ? MetalBayerDemosaicNet::kResidualCh : MetalXTransDemosaicNet::kResidualCh;
  const int tile_input =
      is_bayer ? MetalBayerDemosaicNet::kTileInput : MetalXTransDemosaicNet::kTileInput;
  const int tile_output =
      is_bayer ? MetalBayerDemosaicNet::kTileOutput : MetalXTransDemosaicNet::kTileOutput;
  const int pack_factor = 2;

  module.geometry =
      MakeGeometry(tile_input, tile_output, depth, width, pack_out_ch, residual_ch, pack_factor);

  // Validate fixed pack / unpack arrays (pack is used as a real conv; unpack is structural only).
  {
    const auto& pack = nn::RequireF32Tensor(tensors, "pack.weight", {pack_out_ch, 3, 2, 2});
    RequireExactHostWeight(pack, is_bayer ? ExpectedBayerPackWeight() : ExpectedXTransPackWeight(),
                           "pack.weight", module_name);
  }
  {
    const auto& unpack = nn::RequireF32Tensor(tensors, "unpack.weight", {residual_ch, 1, 2, 2});
    RequireExactHostWeight(unpack, ExpectedUnpackWeight(), "unpack.weight", module_name);
  }

  std::size_t weight_bytes = 0;
  auto        add_bytes    = [&](const nn::SafetensorsTensor& t) {
    weight_bytes += t.data.size() * sizeof(float);
  };

  MPSGraph* graph = [MPSGraph new];
  graph.options   = MPSGraphOptionsNone;
  module.graph    = graph;

  MPSShape* input_shape = MakeShape(1, tile_input, tile_input, 3);
  module.input_tensor =
      [graph placeholderWithShape:input_shape dataType:MPSDataTypeFloat32 name:@"input"];

  // Pack: 2×2 stride-2 valid convolution, no bias.
  const auto& pack_w = nn::RequireF32Tensor(tensors, "pack.weight", {pack_out_ch, 3, 2, 2});
  add_bytes(pack_w);
  MPSGraphTensor* pack_weights = ConstantFromTensor(graph, pack_w, @[ @(pack_out_ch), @3, @2, @2 ]);
  MPSGraphTensor* x =
      [graph convolution2DWithSourceTensor:module.input_tensor
                             weightsTensor:pack_weights
                                descriptor:ValidConvDescriptor(2, 2)
                                      name:@"pack"];

  // Trunk: depth × (3×3 valid + bias + ReLU).
  for (int i = 0; i < depth; ++i) {
    const int         in_c = (i == 0) ? pack_out_ch : width;
    const std::string wk   = "trunk." + std::to_string(i) + ".weight";
    const std::string bk   = "trunk." + std::to_string(i) + ".bias";
    const auto&       weight = nn::RequireF32Tensor(tensors, wk, {width, in_c, 3, 3});
    const auto&       bias   = nn::RequireF32Tensor(tensors, bk, {width});
    add_bytes(weight);
    add_bytes(bias);
    NSString* prefix = [NSString stringWithFormat:@"trunk_%d", i];
    x = ConvBiasRelu(graph, x, weight, bias, width, in_c, 3, 3, 1, /*apply_relu=*/true, prefix);
  }

  // Residual 1×1 + bias, no ReLU.
  {
    const auto& weight =
        nn::RequireF32Tensor(tensors, "residual.weight", {residual_ch, width, 1, 1});
    const auto& bias = nn::RequireF32Tensor(tensors, "residual.bias", {residual_ch});
    add_bytes(weight);
    add_bytes(bias);
    x = ConvBiasRelu(graph, x, weight, bias, residual_ch, width, 1, 1, 1, /*apply_relu=*/false,
                     @"residual");
  }

  // Explicit residual unpack (reshape / transpose / reshape).
  MPSGraphTensor* residual_rgb =
      ExplicitResidualUnpack(graph, x, module.geometry.residual_h, @"unpack");

  // Center-crop the original graph input and concatenate on the channel axis.
  MPSGraphTensor* skip =
      CenterSliceNHWC(graph, module.input_tensor, tile_input, module.geometry.unpacked_h,
                      module.geometry.skip_top, @"skip_crop");
  x = [graph concatTensors:@[ residual_rgb, skip ] dimension:3 name:@"concat"];

  // Post 3×3 + bias + ReLU, then 1×1 output + bias.
  {
    const auto& weight = nn::RequireF32Tensor(tensors, "post_conv.weight", {width, 6, 3, 3});
    const auto& bias   = nn::RequireF32Tensor(tensors, "post_conv.bias", {width});
    add_bytes(weight);
    add_bytes(bias);
    x = ConvBiasRelu(graph, x, weight, bias, width, 6, 3, 3, 1, /*apply_relu=*/true, @"post");
  }
  {
    const auto& weight = nn::RequireF32Tensor(tensors, "output.weight", {3, width, 1, 1});
    const auto& bias   = nn::RequireF32Tensor(tensors, "output.bias", {3});
    add_bytes(weight);
    add_bytes(bias);
    x = ConvBiasRelu(graph, x, weight, bias, 3, width, 1, 1, 1, /*apply_relu=*/false, @"output");
  }

  // Final fixed center crop to 1024×1024.
  module.output_tensor =
      CenterSliceNHWC(graph, x, module.geometry.post_h, tile_output, module.geometry.final_top,
                      @"export_crop");
  // unpack.weight is validated only; count its bytes for resident accounting.
  add_bytes(nn::RequireF32Tensor(tensors, "unpack.weight", {residual_ch, 1, 2, 2}));
  module.resident_weight_bytes = weight_bytes;
}

void CompileAndBind(GraphModule& module, MTL::Device* device, const char* module_name) {
  id<MTLDevice> objc_device = ToObjcDevice(device);
  if (objc_device == nil) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=compile): null device");
  }

  const int tile_input  = module.geometry.tile_input;
  const int tile_output = module.geometry.tile_output;
  MPSShape* input_shape  = MakeShape(1, tile_input, tile_input, 3);
  MPSShape* output_shape = MakeShape(1, tile_output, tile_output, 3);

  MPSGraphShapedType* feed_type =
      [[MPSGraphShapedType alloc] initWithShape:input_shape dataType:MPSDataTypeFloat32];
  MPSGraphCompilationDescriptor* compilation = [MPSGraphCompilationDescriptor new];
  compilation.waitForCompilationCompletion   = YES;
  __block NSError* compile_error             = nil;
  compilation.compilationCompletionHandler   = ^(MPSGraphExecutable*, NSError* error) {
    if (error != nil && compile_error == nil) {
      compile_error = error;
    }
  };

  MPSGraphDevice* graph_device = [MPSGraphDevice deviceWithMTLDevice:objc_device];
  if (graph_device == nil || graph_device.metalDevice != objc_device) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=compile): graph device mismatch");
  }

  module.executable = [module.graph compileWithDevice:graph_device
                                                feeds:@{module.input_tensor : feed_type}
                                        targetTensors:@[ module.output_tensor ]
                                     targetOperations:nil
                                compilationDescriptor:compilation];
  if (compile_error != nil) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=compile): " +
                             NSErrorMessage(compile_error));
  }
  if (module.executable == nil) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=compile): null executable");
  }
  ++module.compile_count;

  const std::size_t input_bytes =
      static_cast<std::size_t>(1) * tile_input * tile_input * 3U * sizeof(float);
  const std::size_t output_bytes =
      static_cast<std::size_t>(1) * tile_output * tile_output * 3U * sizeof(float);

  module.input_buffer =
      NS::TransferPtr(device->newBuffer(static_cast<NS::UInteger>(input_bytes),
                                        MTL::ResourceStorageModePrivate));
  module.output_buffer =
      NS::TransferPtr(device->newBuffer(static_cast<NS::UInteger>(output_bytes),
                                        MTL::ResourceStorageModePrivate));
  if (!module.input_buffer || !module.output_buffer) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=prepare): tile buffer allocation");
  }
  ++module.input_output_allocation_count;

  module.input_data =
      [[MPSGraphTensorData alloc] initWithMTLBuffer:ToObjcBuffer(module.input_buffer.get())
                                              shape:input_shape
                                           dataType:MPSDataTypeFloat32];
  module.output_data =
      [[MPSGraphTensorData alloc] initWithMTLBuffer:ToObjcBuffer(module.output_buffer.get())
                                              shape:output_shape
                                           dataType:MPSDataTypeFloat32];
  if (module.input_data == nil || module.output_data == nil) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=prepare): tensor data bind");
  }

  module.execution                 = [MPSGraphExecutableExecutionDescriptor new];
  module.execution.completionHandler = ^(NSArray<MPSGraphTensorData*>*, NSError* error) {
    if (error != nil && module.last_encode_error == nil) {
      module.last_encode_error = error;
    }
  };

  module.ready = true;
}

void LoadModule(GraphModule& module, const nn::SafetensorsTensorMap& tensors, bool is_bayer,
                const char* module_name, MTL::Device* device_in) {
  GraphModule staging;
  RunObjc("load", [&] {
    RequireAllF32(tensors, module_name);
    if (is_bayer) {
      RequireMetadata(tensors, "format", "demosaicnet-pytorch-state_dict", module_name);
      RequireMetadata(tensors, "architecture", MetalBayerDemosaicNet::kArchitecture, module_name);
      RequireMetadata(tensors, "architecture_version", "1", module_name);
      RequireMetadata(tensors, "variant", "bayer", module_name);
      RequireMetadata(tensors, "cfa_period", "2", module_name);
      RequireMetadata(tensors, "pack_factor", "2", module_name);
      RequireMetadata(tensors, "tile_input", "1086", module_name);
      RequireMetadata(tensors, "tile_output", "1024", module_name);
      RequireMetadata(tensors, "tile_border", "31", module_name);
      RequireMetadata(tensors, "tile_pad", "32", module_name);
      RequireMetadata(tensors, "tile_step", "1024", module_name);
      RequireMetadata(tensors, "checkpoint_sha256",
                      "f00fb0e4f4a49e32344ffb0add583bee98c7d5dbfda6c593b5b066d08f9de69f",
                      module_name);
    } else {
      RequireMetadata(tensors, "format", "demosaicnet-pytorch-state_dict", module_name);
      RequireMetadata(tensors, "architecture", MetalXTransDemosaicNet::kArchitecture, module_name);
      RequireMetadata(tensors, "architecture_version", "1", module_name);
      RequireMetadata(tensors, "variant", "xtrans", module_name);
      RequireMetadata(tensors, "cfa_period", "6", module_name);
      RequireMetadata(tensors, "pack_factor", "2", module_name);
      RequireMetadata(tensors, "tile_input", "1048", module_name);
      RequireMetadata(tensors, "tile_output", "1024", module_name);
      RequireMetadata(tensors, "tile_border", "12", module_name);
      RequireMetadata(tensors, "tile_pad", "12", module_name);
      RequireMetadata(tensors, "tile_step", "1020", module_name);
      RequireMetadata(tensors, "checkpoint_sha256",
                      "f985ba64404a4ef9e4662d4f556d184de1e47127ab046f7140fa4b614f4c7546",
                      module_name);
    }

    BuildGraph(staging, tensors, is_bayer, module_name);
  });

  MTL::Device* device = ResolveDevice(device_in);
  RunObjc("compile", [&] { CompileAndBind(staging, device, module_name); });

  // Publish only after full success.
  module = std::move(staging);
}

void EncodeGraph(const GraphModule& module, void* mps_command_buffer, const char* module_name) {
  if (!module.ready || module.executable == nil) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=graph_encode): not ready");
  }
  if (mps_command_buffer == nullptr) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=graph_encode): null command buffer");
  }
  MPSCommandBuffer* command_buffer = (__bridge MPSCommandBuffer*)mps_command_buffer;
  module.last_encode_error         = nil;
  NSArray<MPSGraphTensorData*>* results =
      [module.executable encodeToCommandBuffer:command_buffer
                                   inputsArray:@[ module.input_data ]
                                  resultsArray:@[ module.output_data ]
                           executionDescriptor:module.execution];
  if (results == nil) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=graph_encode): encode returned nil");
  }
}

void NchwToNhwc(const float* nchw, float* nhwc, int h, int w) {
  const std::size_t plane = static_cast<std::size_t>(h) * static_cast<std::size_t>(w);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t pix = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                              static_cast<std::size_t>(x);
      nhwc[pix * 3U + 0U] = nchw[0U * plane + pix];
      nhwc[pix * 3U + 1U] = nchw[1U * plane + pix];
      nhwc[pix * 3U + 2U] = nchw[2U * plane + pix];
    }
  }
}

void NhwcToNchw(const float* nhwc, float* nchw, int h, int w) {
  const std::size_t plane = static_cast<std::size_t>(h) * static_cast<std::size_t>(w);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t pix = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                              static_cast<std::size_t>(x);
      nchw[0U * plane + pix] = nhwc[pix * 3U + 0U];
      nchw[1U * plane + pix] = nhwc[pix * 3U + 1U];
      nchw[2U * plane + pix] = nhwc[pix * 3U + 2U];
    }
  }
}

void ForwardNchw(const GraphModule& module, const float* input_nchw, float* output_nchw,
                 const char* module_name) {
  if (!module.ready) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=graph_execute): not ready");
  }
  if (input_nchw == nullptr || output_nchw == nullptr) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=prepare): null host buffer");
  }

  const int tile_input  = module.geometry.tile_input;
  const int tile_output = module.geometry.tile_output;
  const std::size_t input_elems =
      static_cast<std::size_t>(1) * 3U * tile_input * tile_input;
  const std::size_t output_elems =
      static_cast<std::size_t>(1) * 3U * tile_output * tile_output;

  MTL::Device*       device = ResolveDevice(nullptr);
  MTL::CommandQueue* queue  = ResolveQueue();

  auto host_in = NS::TransferPtr(device->newBuffer(static_cast<NS::UInteger>(input_elems * sizeof(float)),
                                                   MTL::ResourceStorageModeShared));
  auto host_out =
      NS::TransferPtr(device->newBuffer(static_cast<NS::UInteger>(output_elems * sizeof(float)),
                                        MTL::ResourceStorageModeShared));
  if (!host_in || !host_out) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=prepare): staging allocation");
  }

  NchwToNhwc(input_nchw, static_cast<float*>(ToObjcBuffer(host_in.get()).contents), tile_input,
             tile_input);

  RunObjc("graph_execute", [&] {
    NS::SharedPtr<MTL::CommandBuffer> backing = NS::RetainPtr(queue->commandBuffer());
    if (!backing) {
      throw std::runtime_error(std::string(module_name) +
                               ": Metal Neural Engine failed (stage=graph_execute): command buffer");
    }
    MPSCommandBuffer* mps_cb =
        [MPSCommandBuffer commandBufferWithCommandBuffer:ToObjcCommandBuffer(backing.get())];
    if (mps_cb == nil) {
      throw std::runtime_error(
          std::string(module_name) +
          ": Metal Neural Engine failed (stage=graph_execute): MPSCommandBuffer");
    }

    id<MTLBlitCommandEncoder> upload = [mps_cb blitCommandEncoder];
    if (upload == nil) {
      throw std::runtime_error(std::string(module_name) +
                               ": Metal Neural Engine failed (stage=prepare): upload blit encoder");
    }
    [upload copyFromBuffer:ToObjcBuffer(host_in.get())
              sourceOffset:0
                  toBuffer:ToObjcBuffer(module.input_buffer.get())
         destinationOffset:0
                      size:input_elems * sizeof(float)];
    [upload endEncoding];

    EncodeGraph(module, (__bridge void*)mps_cb, module_name);

    id<MTLBlitCommandEncoder> download = [mps_cb blitCommandEncoder];
    if (download == nil) {
      throw std::runtime_error(
          std::string(module_name) +
          ": Metal Neural Engine failed (stage=graph_execute): download blit encoder");
    }
    [download copyFromBuffer:ToObjcBuffer(module.output_buffer.get())
                sourceOffset:0
                    toBuffer:ToObjcBuffer(host_out.get())
           destinationOffset:0
                        size:output_elems * sizeof(float)];
    [download endEncoding];

    [mps_cb commit];
    [mps_cb waitUntilCompleted];

    if (module.last_encode_error != nil) {
      throw std::runtime_error(std::string(module_name) +
                               ": Metal Neural Engine failed (stage=graph_execute): " +
                               NSErrorMessage(module.last_encode_error));
    }
    if (mps_cb.status != MTLCommandBufferStatusCompleted || mps_cb.error != nil) {
      throw std::runtime_error(
          std::string(module_name) +
          ": Metal Neural Engine failed (stage=graph_execute): " +
          (mps_cb.error != nil ? NSErrorMessage(mps_cb.error)
                               : std::string("command did not complete")));
    }
  });

  NhwcToNchw(static_cast<const float*>(ToObjcBuffer(host_out.get()).contents), output_nchw,
             tile_output, tile_output);
}

}  // namespace

// ===========================================================================
// Bayer
// ===========================================================================

struct MetalBayerDemosaicNet::Impl {
  GraphModule module;
};

MetalBayerDemosaicNet::MetalBayerDemosaicNet() : impl_(std::make_unique<Impl>()) {}
MetalBayerDemosaicNet::~MetalBayerDemosaicNet()                                   = default;
MetalBayerDemosaicNet::MetalBayerDemosaicNet(MetalBayerDemosaicNet&&) noexcept    = default;
MetalBayerDemosaicNet& MetalBayerDemosaicNet::operator=(MetalBayerDemosaicNet&&) noexcept =
    default;

void MetalBayerDemosaicNet::LoadAndCompile(const nn::SafetensorsTensorMap& tensors,
                                           MTL::Device*                    device) {
  // Stage into a temporary module so failures never publish partial state.
  GraphModule staging;
  LoadModule(staging, tensors, /*is_bayer=*/true, "MetalBayerDemosaicNet", device);
  impl_->module = std::move(staging);
}

auto MetalBayerDemosaicNet::ready() const -> bool {
  return impl_ != nullptr && impl_->module.ready;
}

auto MetalBayerDemosaicNet::ResidentWeightBytes() const -> std::size_t {
  return impl_ != nullptr ? impl_->module.resident_weight_bytes : 0;
}

auto MetalBayerDemosaicNet::OwnedBufferBytes() const -> std::size_t {
  if (impl_ == nullptr || !impl_->module.ready) {
    return 0;
  }
  const auto& g = impl_->module.geometry;
  const std::size_t in =
      static_cast<std::size_t>(1) * g.tile_input * g.tile_input * 3U * sizeof(float);
  const std::size_t out =
      static_cast<std::size_t>(1) * g.tile_output * g.tile_output * 3U * sizeof(float);
  return in + out;
}

auto MetalBayerDemosaicNet::compile_count() const -> std::uint64_t {
  return impl_ != nullptr ? impl_->module.compile_count : 0;
}

auto MetalBayerDemosaicNet::input_output_allocation_count() const -> std::uint64_t {
  return impl_ != nullptr ? impl_->module.input_output_allocation_count : 0;
}

auto MetalBayerDemosaicNet::InputBuffer() const -> MTL::Buffer* {
  return impl_ != nullptr ? impl_->module.input_buffer.get() : nullptr;
}

auto MetalBayerDemosaicNet::OutputBuffer() const -> MTL::Buffer* {
  return impl_ != nullptr ? impl_->module.output_buffer.get() : nullptr;
}

void MetalBayerDemosaicNet::EncodeOnMpsCommandBuffer(void* mps_command_buffer) const {
  RunObjc("graph_encode",
          [&] { EncodeGraph(impl_->module, mps_command_buffer, "MetalBayerDemosaicNet"); });
}

void MetalBayerDemosaicNet::ClearLastEncodeError() const {
  if (impl_ != nullptr) {
    impl_->module.last_encode_error = nil;
  }
}

auto MetalBayerDemosaicNet::HasLastEncodeError() const -> bool {
  return impl_ != nullptr && impl_->module.last_encode_error != nil;
}

auto MetalBayerDemosaicNet::LastEncodeErrorMessage() const -> std::string {
  if (impl_ == nullptr || impl_->module.last_encode_error == nil) {
    return {};
  }
  return NSErrorMessage(impl_->module.last_encode_error);
}

void MetalBayerDemosaicNet::ForwardNchwReference(const float* input_nchw,
                                                 float*       output_nchw) const {
  ForwardNchw(impl_->module, input_nchw, output_nchw, "MetalBayerDemosaicNet");
}

// ===========================================================================
// X-Trans
// ===========================================================================

struct MetalXTransDemosaicNet::Impl {
  GraphModule module;
};

MetalXTransDemosaicNet::MetalXTransDemosaicNet() : impl_(std::make_unique<Impl>()) {}
MetalXTransDemosaicNet::~MetalXTransDemosaicNet()                                    = default;
MetalXTransDemosaicNet::MetalXTransDemosaicNet(MetalXTransDemosaicNet&&) noexcept    = default;
MetalXTransDemosaicNet& MetalXTransDemosaicNet::operator=(MetalXTransDemosaicNet&&) noexcept =
    default;

void MetalXTransDemosaicNet::LoadAndCompile(const nn::SafetensorsTensorMap& tensors,
                                            MTL::Device*                    device) {
  GraphModule staging;
  LoadModule(staging, tensors, /*is_bayer=*/false, "MetalXTransDemosaicNet", device);
  impl_->module = std::move(staging);
}

auto MetalXTransDemosaicNet::ready() const -> bool {
  return impl_ != nullptr && impl_->module.ready;
}

auto MetalXTransDemosaicNet::ResidentWeightBytes() const -> std::size_t {
  return impl_ != nullptr ? impl_->module.resident_weight_bytes : 0;
}

auto MetalXTransDemosaicNet::OwnedBufferBytes() const -> std::size_t {
  if (impl_ == nullptr || !impl_->module.ready) {
    return 0;
  }
  const auto& g = impl_->module.geometry;
  const std::size_t in =
      static_cast<std::size_t>(1) * g.tile_input * g.tile_input * 3U * sizeof(float);
  const std::size_t out =
      static_cast<std::size_t>(1) * g.tile_output * g.tile_output * 3U * sizeof(float);
  return in + out;
}

auto MetalXTransDemosaicNet::compile_count() const -> std::uint64_t {
  return impl_ != nullptr ? impl_->module.compile_count : 0;
}

auto MetalXTransDemosaicNet::input_output_allocation_count() const -> std::uint64_t {
  return impl_ != nullptr ? impl_->module.input_output_allocation_count : 0;
}

auto MetalXTransDemosaicNet::InputBuffer() const -> MTL::Buffer* {
  return impl_ != nullptr ? impl_->module.input_buffer.get() : nullptr;
}

auto MetalXTransDemosaicNet::OutputBuffer() const -> MTL::Buffer* {
  return impl_ != nullptr ? impl_->module.output_buffer.get() : nullptr;
}

void MetalXTransDemosaicNet::EncodeOnMpsCommandBuffer(void* mps_command_buffer) const {
  RunObjc("graph_encode",
          [&] { EncodeGraph(impl_->module, mps_command_buffer, "MetalXTransDemosaicNet"); });
}

void MetalXTransDemosaicNet::ClearLastEncodeError() const {
  if (impl_ != nullptr) {
    impl_->module.last_encode_error = nil;
  }
}

auto MetalXTransDemosaicNet::HasLastEncodeError() const -> bool {
  return impl_ != nullptr && impl_->module.last_encode_error != nil;
}

auto MetalXTransDemosaicNet::LastEncodeErrorMessage() const -> std::string {
  if (impl_ == nullptr || impl_->module.last_encode_error == nil) {
    return {};
  }
  return NSErrorMessage(impl_->module.last_encode_error);
}

void MetalXTransDemosaicNet::ForwardNchwReference(const float* input_nchw,
                                                  float*       output_nchw) const {
  ForwardNchw(impl_->module, input_nchw, output_nchw, "MetalXTransDemosaicNet");
}

}  // namespace alcedo

#endif  // HAVE_METAL
