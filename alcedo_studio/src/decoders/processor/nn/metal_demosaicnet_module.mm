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

#include "metal/compute_pipeline_cache.hpp"
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
  int           post_channels = 0;  // 24 Bayer / 32 X-Trans

  NS::SharedPtr<MTL::Buffer> input_buffer;   // [N,Hin,Win,3]
  NS::SharedPtr<MTL::Buffer> output_buffer;  // graph concat [N,Hcat,Wcat,6]
  // Fused-tail weights (shared, filled once at load; immutable on hot path).
  NS::SharedPtr<MTL::Buffer> post_w_buffer;      // OIHW [Cout,6,3,3] flat
  NS::SharedPtr<MTL::Buffer> post_b_buffer;      // [Cout]
  NS::SharedPtr<MTL::Buffer> out_w_cio_buffer;   // prepacked [Cout,3]
  NS::SharedPtr<MTL::Buffer> out_b_buffer;       // [3]

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
    post_channels                 = other.post_channels;
    input_buffer                  = std::move(other.input_buffer);
    output_buffer                 = std::move(other.output_buffer);
    post_w_buffer                 = std::move(other.post_w_buffer);
    post_b_buffer                 = std::move(other.post_b_buffer);
    out_w_cio_buffer              = std::move(other.out_w_cio_buffer);
    out_b_buffer                  = std::move(other.out_b_buffer);
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
    other.post_channels                 = 0;
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
    post_channels                 = 0;
    input_buffer.reset();
    output_buffer.reset();
    post_w_buffer.reset();
    post_b_buffer.reset();
    out_w_cio_buffer.reset();
    out_b_buffer.reset();
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

// Product path: NHWC activations with HWIO weight constants. Safetensors store
// OIHW (O,I,H,W); transpose once at cold load so the graph compiler can avoid an
// implicit OIHW→HWIO conversion on every tile. Element count is unchanged; math
// is identical when weightsLayout is HWIO.
[[nodiscard]] auto TransposeOihwToHwio(const nn::SafetensorsTensor& oihw, int out_c, int in_c,
                                       int kh, int kw) -> std::vector<float> {
  const std::size_t expected =
      static_cast<std::size_t>(out_c) * static_cast<std::size_t>(in_c) *
      static_cast<std::size_t>(kh) * static_cast<std::size_t>(kw);
  if (oihw.data.size() != expected) {
    throw std::runtime_error(
        "Metal Neural Engine failed (stage=load): OIHW weight size mismatch for HWIO transpose");
  }
  std::vector<float> hwio(expected);
  for (int o = 0; o < out_c; ++o) {
    for (int i = 0; i < in_c; ++i) {
      for (int h = 0; h < kh; ++h) {
        for (int w = 0; w < kw; ++w) {
          const std::size_t src =
              ((((static_cast<std::size_t>(o) * static_cast<std::size_t>(in_c)) +
                 static_cast<std::size_t>(i)) *
                static_cast<std::size_t>(kh)) +
               static_cast<std::size_t>(h)) *
                  static_cast<std::size_t>(kw) +
              static_cast<std::size_t>(w);
          const std::size_t dst =
              ((((static_cast<std::size_t>(h) * static_cast<std::size_t>(kw)) +
                 static_cast<std::size_t>(w)) *
                static_cast<std::size_t>(in_c)) +
               static_cast<std::size_t>(i)) *
                  static_cast<std::size_t>(out_c) +
              static_cast<std::size_t>(o);
          hwio[dst] = oihw.data[src];
        }
      }
    }
  }
  return hwio;
}

auto ConstantHwioFromOihw(MPSGraph* graph, const nn::SafetensorsTensor& oihw, int out_c, int in_c,
                          int kh, int kw) -> MPSGraphTensor* {
  const std::vector<float> hwio = TransposeOihwToHwio(oihw, out_c, in_c, kh, kw);
  NSData* data = [NSData dataWithBytes:hwio.data() length:hwio.size() * sizeof(float)];
  // HWIO shape: [kernelHeight, kernelWidth, inputChannels, outputChannels]
  MPSShape* shape = @[ @(kh), @(kw), @(in_c), @(out_c) ];
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
                weightsLayout:MPSGraphTensorNamedDataLayoutHWIO];
  desc.paddingLeft   = 0;
  desc.paddingRight  = 0;
  desc.paddingTop    = 0;
  desc.paddingBottom = 0;
  return desc;
}

auto ConvBiasRelu(MPSGraph* graph, MPSGraphTensor* source, const nn::SafetensorsTensor& weight,
                  const nn::SafetensorsTensor& bias, int out_c, int in_c, int kh, int kw,
                  NSUInteger stride, bool apply_relu, NSString* name_prefix) -> MPSGraphTensor* {
  MPSGraphTensor* w = ConstantHwioFromOihw(graph, weight, out_c, in_c, kh, kw);
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

auto NativeResidualUnpack(MPSGraph* graph, MPSGraphTensor* residual_nhwc12,
                          NSString* name_prefix) -> MPSGraphTensor* {
  // The safetensors unpack order is [rgb, subpixel_y, subpixel_x]. MPSGraph's
  // pixel-shuffle order stores each spatial block contiguously in depth, which
  // matches that [rgb, y, x] channel order for this model.
  return [graph depthToSpace2DTensor:residual_nhwc12
                           widthAxis:2
                          heightAxis:1
                           depthAxis:3
                           blockSize:2
                usePixelShuffleOrder:YES
                                  name:name_prefix];
}

auto CenterSliceNHWC(MPSGraph* graph, MPSGraphTensor* source, int source_h, int out_h, int top,
                     int batch_size, NSString* name) -> MPSGraphTensor* {
  (void)source_h;
  return [graph sliceTensor:source
                     starts:@[ @0, @(top), @(top), @0 ]
                       ends:@[ @(batch_size), @(top + out_h), @(top + out_h), @3 ]
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
  constexpr int batch_size = 2;
  const int pack_factor = 2;

  module.geometry =
      MakeGeometry(tile_input, tile_output, depth, width, pack_out_ch, residual_ch, pack_factor);
  module.post_channels = width;

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

  MPSShape* input_shape = MakeShape(batch_size, tile_input, tile_input, 3);
  module.input_tensor =
      [graph placeholderWithShape:input_shape dataType:MPSDataTypeFloat32 name:@"input"];

  // Pack: 2×2 stride-2 valid convolution, no bias.
  // Host pack.weight is OIHW and already validated above; transpose to HWIO once.
  const auto& pack_w = nn::RequireF32Tensor(tensors, "pack.weight", {pack_out_ch, 3, 2, 2});
  add_bytes(pack_w);
  MPSGraphTensor* pack_weights = ConstantHwioFromOihw(graph, pack_w, pack_out_ch, 3, 2, 2);
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

  // Native depth-to-space replaces the materialized reshape / transpose / reshape sequence.
  MPSGraphTensor* residual_rgb = NativeResidualUnpack(graph, x, @"unpack");

  // Center-crop the original graph input and concatenate on the channel axis.
  // Graph ends here: the post/output/gamma tail is a fixed Metal kernel (CUDA P4-A style)
  // so the 24/32-channel post activation is never materialized.
  MPSGraphTensor* skip =
      CenterSliceNHWC(graph, module.input_tensor, tile_input, module.geometry.unpacked_h,
                      module.geometry.skip_top, batch_size, @"skip_crop");
  // The post convolution's OIHW weights consume the concat in the same order as
  // CUDA/OpenCL's DemosaicNetUnpackCropConcatNhwc: the cropped sparse mosaic
  // occupies channels 0..2 and the unpacked residual occupies channels 3..5.
  // Reversing these inputs still produces a valid MPSGraph tensor, but applies
  // every post-convolution weight to the wrong feature family.
  module.output_tensor = [graph concatTensors:@[ skip, residual_rgb ] dimension:3 name:@"concat"];

  // Validate and account for fused-tail weights (uploaded as MTLBuffers in CompileAndBind).
  {
    const auto& weight = nn::RequireF32Tensor(tensors, "post_conv.weight", {width, 6, 3, 3});
    const auto& bias   = nn::RequireF32Tensor(tensors, "post_conv.bias", {width});
    add_bytes(weight);
    add_bytes(bias);
  }
  {
    const auto& weight = nn::RequireF32Tensor(tensors, "output.weight", {3, width, 1, 1});
    const auto& bias   = nn::RequireF32Tensor(tensors, "output.bias", {3});
    add_bytes(weight);
    add_bytes(bias);
  }
  // unpack.weight is validated only; count its bytes for resident accounting.
  add_bytes(nn::RequireF32Tensor(tensors, "unpack.weight", {residual_ch, 1, 2, 2}));
  module.resident_weight_bytes = weight_bytes;
}

[[nodiscard]] auto MakeSharedWeightBuffer(MTL::Device* device, const float* data,
                                          std::size_t count, const char* module_name,
                                          const char* label) -> NS::SharedPtr<MTL::Buffer> {
  if (data == nullptr || count == 0) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=load): empty " + label);
  }
  const std::size_t bytes = count * sizeof(float);
  auto buffer = NS::TransferPtr(
      device->newBuffer(static_cast<NS::UInteger>(bytes), MTL::ResourceStorageModeShared));
  if (!buffer) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=load): allocate " + label);
  }
  std::memcpy(buffer->contents(), data, bytes);
  return buffer;
}

// OIHW [3, width, 1, 1] → CIO [width, 3] so each post channel's RGB weights are contiguous.
[[nodiscard]] auto PrepackOutputWeightsCio(const nn::SafetensorsTensor& oihw, int width)
    -> std::vector<float> {
  std::vector<float> cio(static_cast<std::size_t>(width) * 3U);
  for (int co = 0; co < width; ++co) {
    for (int c = 0; c < 3; ++c) {
      cio[static_cast<std::size_t>(co) * 3U + static_cast<std::size_t>(c)] =
          oihw.data[static_cast<std::size_t>(c) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(co)];
    }
  }
  return cio;
}

void CompileAndBind(GraphModule& module, MTL::Device* device,
                    const nn::SafetensorsTensorMap& tensors, const char* module_name) {
  id<MTLDevice> objc_device = ToObjcDevice(device);
  if (objc_device == nil) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=compile): null device");
  }

  const int tile_input   = module.geometry.tile_input;
  const int cat_h        = module.geometry.unpacked_h;
  const int width        = module.post_channels;
  constexpr int batch_size = 2;
  constexpr int cat_ch     = 6;
  MPSShape* input_shape  = MakeShape(batch_size, tile_input, tile_input, 3);
  MPSShape* output_shape = MakeShape(batch_size, cat_h, cat_h, cat_ch);

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
      static_cast<std::size_t>(batch_size) * tile_input * tile_input * 3U * sizeof(float);
  const std::size_t output_bytes =
      static_cast<std::size_t>(batch_size) * cat_h * cat_h * static_cast<std::size_t>(cat_ch) *
      sizeof(float);

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

  // Fused-tail weights: post OIHW as-is, output prepacked CIO once at cold load.
  {
    const auto& post_w = nn::RequireF32Tensor(tensors, "post_conv.weight", {width, 6, 3, 3});
    const auto& post_b = nn::RequireF32Tensor(tensors, "post_conv.bias", {width});
    const auto& out_w  = nn::RequireF32Tensor(tensors, "output.weight", {3, width, 1, 1});
    const auto& out_b  = nn::RequireF32Tensor(tensors, "output.bias", {3});
    module.post_w_buffer =
        MakeSharedWeightBuffer(device, post_w.data.data(), post_w.data.size(), module_name,
                               "post_w");
    module.post_b_buffer =
        MakeSharedWeightBuffer(device, post_b.data.data(), post_b.data.size(), module_name,
                               "post_b");
    const std::vector<float> out_cio = PrepackOutputWeightsCio(out_w, width);
    module.out_w_cio_buffer =
        MakeSharedWeightBuffer(device, out_cio.data(), out_cio.size(), module_name, "out_w_cio");
    module.out_b_buffer =
        MakeSharedWeightBuffer(device, out_b.data.data(), out_b.data.size(), module_name, "out_b");
  }

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
  RunObjc("compile", [&] { CompileAndBind(staging, device, tensors, module_name); });

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

auto GetFusedTailPipeline(const char* function_name) -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_DEMOSAICNET_IO_METALLIB_PATH
  (void)function_name;
  throw std::runtime_error(
      "Metal Neural Engine failed (stage=tile_output): demosaicnet_io metallib path is not "
      "configured");
#else
  return metal::ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_DEMOSAICNET_IO_METALLIB_PATH, function_name, "Metal DemosaicNet Fused Tail");
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

[[nodiscard]] auto FusedRgbaKernelName(int post_channels) -> const char* {
  if (post_channels == 24) {
    return "demosaicnet_fused_tail_rgba_w24";
  }
  if (post_channels == 32) {
    return "demosaicnet_fused_tail_rgba_w32";
  }
  throw std::runtime_error(
      "Metal Neural Engine failed (stage=tile_output): post_channels must be 24 or 32");
}

[[nodiscard]] auto FusedNhwcKernelName(int post_channels) -> const char* {
  if (post_channels == 24) {
    return "demosaicnet_fused_tail_nhwc_w24";
  }
  if (post_channels == 32) {
    return "demosaicnet_fused_tail_nhwc_w32";
  }
  throw std::runtime_error(
      "Metal Neural Engine failed (stage=tile_output): post_channels must be 24 or 32");
}

void EncodeFusedTailRgbaImpl(const GraphModule& module, id<MTLCommandBuffer> command_buffer,
                             id<MTLTexture> output_rgba, const DemosaicNetFusedTailParams& params,
                             const char* module_name) {
  if (!module.ready || module.output_buffer.get() == nullptr ||
      module.post_w_buffer.get() == nullptr || module.post_b_buffer.get() == nullptr ||
      module.out_w_cio_buffer.get() == nullptr || module.out_b_buffer.get() == nullptr) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=tile_output): not ready");
  }
  if (command_buffer == nil || output_rgba == nil) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=tile_output): null command or "
                             "texture");
  }
  if (params.owned_w <= 0 || params.owned_h <= 0 || params.cat_h < 3 || params.cat_w < 3 ||
      params.export_h < 1 || params.export_w < 1) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=tile_output): invalid fused "
                             "geometry");
  }

  auto pipeline_cpp = GetFusedTailPipeline(FusedRgbaKernelName(module.post_channels));
  id<MTLComputePipelineState> pipeline =
      (__bridge id<MTLComputePipelineState>)(reinterpret_cast<void*>(pipeline_cpp.get()));

  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=tile_output): compute encoder");
  }
  encoder.label = @"DemosaicNet Fused Tail RGBA";
  [encoder setComputePipelineState:pipeline];
  [encoder setBuffer:ToObjcBuffer(module.output_buffer.get()) offset:0 atIndex:0];
  [encoder setBuffer:ToObjcBuffer(module.post_w_buffer.get()) offset:0 atIndex:1];
  [encoder setBuffer:ToObjcBuffer(module.post_b_buffer.get()) offset:0 atIndex:2];
  [encoder setBuffer:ToObjcBuffer(module.out_w_cio_buffer.get()) offset:0 atIndex:3];
  [encoder setBuffer:ToObjcBuffer(module.out_b_buffer.get()) offset:0 atIndex:4];
  [encoder setBytes:&params length:sizeof(params) atIndex:5];
  [encoder setTexture:output_rgba atIndex:0];
  Dispatch2D(encoder, pipeline, params.owned_w, params.owned_h);
  [encoder endEncoding];
}

void EncodeFusedTailNhwcImpl(const GraphModule& module, id<MTLCommandBuffer> command_buffer,
                             id<MTLBuffer> rgb_nhwc, const DemosaicNetFusedTailParams& params,
                             const char* module_name) {
  if (!module.ready || module.output_buffer.get() == nullptr ||
      module.post_w_buffer.get() == nullptr || module.post_b_buffer.get() == nullptr ||
      module.out_w_cio_buffer.get() == nullptr || module.out_b_buffer.get() == nullptr) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=tile_output): not ready");
  }
  if (command_buffer == nil || rgb_nhwc == nil) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=tile_output): null command or "
                             "rgb buffer");
  }

  auto pipeline_cpp = GetFusedTailPipeline(FusedNhwcKernelName(module.post_channels));
  id<MTLComputePipelineState> pipeline =
      (__bridge id<MTLComputePipelineState>)(reinterpret_cast<void*>(pipeline_cpp.get()));

  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=tile_output): compute encoder");
  }
  encoder.label = @"DemosaicNet Fused Tail NHWC";
  [encoder setComputePipelineState:pipeline];
  [encoder setBuffer:ToObjcBuffer(module.output_buffer.get()) offset:0 atIndex:0];
  [encoder setBuffer:ToObjcBuffer(module.post_w_buffer.get()) offset:0 atIndex:1];
  [encoder setBuffer:ToObjcBuffer(module.post_b_buffer.get()) offset:0 atIndex:2];
  [encoder setBuffer:ToObjcBuffer(module.out_w_cio_buffer.get()) offset:0 atIndex:3];
  [encoder setBuffer:ToObjcBuffer(module.out_b_buffer.get()) offset:0 atIndex:4];
  [encoder setBuffer:rgb_nhwc offset:0 atIndex:5];
  [encoder setBytes:&params length:sizeof(params) atIndex:6];
  Dispatch2D(encoder, pipeline, params.export_w, params.export_h);
  [encoder endEncoding];
}

[[nodiscard]] auto MakeReferenceTailParams(const GraphModule& module, int batch_index,
                                           int apply_gamma) -> DemosaicNetFusedTailParams {
  DemosaicNetFusedTailParams p;
  p.batch_index = batch_index;
  p.cat_h       = module.geometry.unpacked_h;
  p.cat_w       = module.geometry.unpacked_h;
  p.export_h    = module.geometry.tile_output;
  p.export_w    = module.geometry.tile_output;
  p.final_crop  = module.geometry.final_top;
  p.apply_gamma = apply_gamma;
  return p;
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
  constexpr int batch_size = 2;
  const std::size_t input_elems =
      static_cast<std::size_t>(batch_size) * 3U * tile_input * tile_input;
  // Fused tail writes NHWC RGB for both batch lanes; reference reads lane 0 only.
  const std::size_t rgb_elems =
      static_cast<std::size_t>(batch_size) * 3U * tile_output * tile_output;

  MTL::Device*       device = ResolveDevice(nullptr);
  MTL::CommandQueue* queue  = ResolveQueue();

  auto host_in = NS::TransferPtr(device->newBuffer(static_cast<NS::UInteger>(input_elems * sizeof(float)),
                                                   MTL::ResourceStorageModeShared));
  auto host_rgb =
      NS::TransferPtr(device->newBuffer(static_cast<NS::UInteger>(rgb_elems * sizeof(float)),
                                        MTL::ResourceStorageModeShared));
  if (!host_in || !host_rgb) {
    throw std::runtime_error(std::string(module_name) +
                             ": Metal Neural Engine failed (stage=prepare): staging allocation");
  }

  float* host_in_ptr = static_cast<float*>(ToObjcBuffer(host_in.get()).contents);
  const std::size_t one_input_elems = 3U * static_cast<std::size_t>(tile_input) * tile_input;
  NchwToNhwc(input_nchw, host_in_ptr, tile_input, tile_input);
  std::memcpy(host_in_ptr + one_input_elems, host_in_ptr, one_input_elems * sizeof(float));

  RunObjc("graph_execute", [&] {
    module.last_encode_error = nil;
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

    // Graph product is concat [N,H,W,6]. Fused Metal tail produces export RGB without gamma.
    const DemosaicNetFusedTailParams tail_params =
        MakeReferenceTailParams(module, /*batch_index=*/0, /*apply_gamma=*/0);
    EncodeFusedTailNhwcImpl(module, mps_cb, ToObjcBuffer(host_rgb.get()), tail_params,
                            module_name);

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

  NhwcToNchw(static_cast<const float*>(ToObjcBuffer(host_rgb.get()).contents), output_nchw,
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
      static_cast<std::size_t>(MetalBayerDemosaicNet::kBatchSize) * g.tile_input * g.tile_input *
      3U * sizeof(float);
  // Graph product is concat [N, unpacked, unpacked, 6].
  const std::size_t cat =
      static_cast<std::size_t>(MetalBayerDemosaicNet::kBatchSize) * g.unpacked_h * g.unpacked_h *
      6U * sizeof(float);
  return in + cat;
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

auto MetalBayerDemosaicNet::CatHeight() const -> int {
  return impl_ != nullptr && impl_->module.ready ? impl_->module.geometry.unpacked_h : 0;
}

auto MetalBayerDemosaicNet::FinalCrop() const -> int {
  return impl_ != nullptr && impl_->module.ready ? impl_->module.geometry.final_top : 0;
}

void MetalBayerDemosaicNet::EncodeOnMpsCommandBuffer(void* mps_command_buffer) const {
  RunObjc("graph_encode",
          [&] { EncodeGraph(impl_->module, mps_command_buffer, "MetalBayerDemosaicNet"); });
}

void MetalBayerDemosaicNet::EncodeFusedTailRgba(void* mtl_command_buffer, MTL::Texture* output_rgba,
                                                const DemosaicNetFusedTailParams& params) const {
  RunObjc("tile_output", [&] {
    EncodeFusedTailRgbaImpl(impl_->module, (__bridge id<MTLCommandBuffer>)mtl_command_buffer,
                            ToObjcTexture(output_rgba), params, "MetalBayerDemosaicNet");
  });
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
      static_cast<std::size_t>(MetalXTransDemosaicNet::kBatchSize) * g.tile_input * g.tile_input *
      3U * sizeof(float);
  const std::size_t cat =
      static_cast<std::size_t>(MetalXTransDemosaicNet::kBatchSize) * g.unpacked_h * g.unpacked_h *
      6U * sizeof(float);
  return in + cat;
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

auto MetalXTransDemosaicNet::CatHeight() const -> int {
  return impl_ != nullptr && impl_->module.ready ? impl_->module.geometry.unpacked_h : 0;
}

auto MetalXTransDemosaicNet::FinalCrop() const -> int {
  return impl_ != nullptr && impl_->module.ready ? impl_->module.geometry.final_top : 0;
}

void MetalXTransDemosaicNet::EncodeOnMpsCommandBuffer(void* mps_command_buffer) const {
  RunObjc("graph_encode",
          [&] { EncodeGraph(impl_->module, mps_command_buffer, "MetalXTransDemosaicNet"); });
}

void MetalXTransDemosaicNet::EncodeFusedTailRgba(void* mtl_command_buffer,
                                                 MTL::Texture* output_rgba,
                                                 const DemosaicNetFusedTailParams& params) const {
  RunObjc("tile_output", [&] {
    EncodeFusedTailRgbaImpl(impl_->module, (__bridge id<MTLCommandBuffer>)mtl_command_buffer,
                            ToObjcTexture(output_rgba), params, "MetalXTransDemosaicNet");
  });
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
