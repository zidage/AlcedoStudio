//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include <gtest/gtest.h>

#include <Metal/Metal.h>
#include <MetalPerformanceShaders/MetalPerformanceShaders.h>
#include <MetalPerformanceShadersGraph/MetalPerformanceShadersGraph.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

#include "metal/metal_context.hpp"

namespace alcedo {
namespace {

constexpr std::size_t kConvInputElements    = 3U * 3U;
constexpr std::size_t kConvOutputElements   = 2U * 2U;
constexpr std::size_t kUnpackInputElements  = 12U * 12U;
constexpr std::size_t kUnpackOutputElements = 12U * 2U * 2U * 3U;

struct BoundaryParams {
  std::uint32_t element_count;
  float         addend;
};

auto ToObjcDevice(MTL::Device* device) -> id<MTLDevice> {
  return (__bridge id<MTLDevice>)(reinterpret_cast<void*>(device));
}

auto ToObjcQueue(MTL::CommandQueue* queue) -> id<MTLCommandQueue> {
  return (__bridge id<MTLCommandQueue>)(reinterpret_cast<void*>(queue));
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
  const char* name    = exception.name.UTF8String;
  const char* reason  = exception.reason.UTF8String;
  std::string message = name != nullptr ? name : "Objective-C exception";
  if (reason != nullptr && reason[0] != '\0') {
    message += ": ";
    message += reason;
  }
  return message;
}

void RunObjcBoundary(const std::function<void()>& body) {
  @try {
    body();
  } @catch (NSException* exception) {
    throw std::runtime_error("Metal DemosaicNet boundary Objective-C failure: " +
                             NSExceptionMessage(exception));
  }
}

void ThrowIfCommandFailed(MPSCommandBuffer* command_buffer, NSError* graph_error) {
  if (graph_error != nil) {
    throw std::runtime_error("Metal DemosaicNet boundary MPSGraph execution failed: " +
                             NSErrorMessage(graph_error));
  }

  NSError* command_error = command_buffer.error;
  if (command_error != nil) {
    throw std::runtime_error("Metal DemosaicNet boundary Metal command failed: " +
                             NSErrorMessage(command_error));
  }
  if (command_buffer.status != MTLCommandBufferStatusCompleted) {
    throw std::runtime_error("Metal DemosaicNet boundary command did not complete successfully.");
  }
}

auto MakeBuffer(MTL::Device* device, std::size_t bytes, MTL::ResourceOptions options,
                const char* label) -> NS::SharedPtr<MTL::Buffer> {
  auto buffer = NS::TransferPtr(device->newBuffer(static_cast<NS::UInteger>(bytes), options));
  if (!buffer) {
    throw std::runtime_error(std::string("Metal DemosaicNet boundary: failed to allocate ") +
                             label + " buffer.");
  }
  return buffer;
}

auto LoadPipeline(id<MTLDevice> device, NSString* function_name) -> id<MTLComputePipelineState> {
#ifndef ALCEDO_METAL_DEMOSAICNET_BOUNDARY_METALLIB_PATH
  (void)device;
  (void)function_name;
  throw std::runtime_error("Metal DemosaicNet boundary: test metallib path is not configured.");
#else
  NSError*  error = nil;
  NSString* path  = [NSString stringWithUTF8String:ALCEDO_METAL_DEMOSAICNET_BOUNDARY_METALLIB_PATH];
  NSURL*    url   = [NSURL fileURLWithPath:path];
  id<MTLLibrary> library = [device newLibraryWithURL:url error:&error];
  if (library == nil) {
    throw std::runtime_error("Metal DemosaicNet boundary: failed to load test metallib: " +
                             NSErrorMessage(error));
  }

  id<MTLFunction> function = [library newFunctionWithName:function_name];
  if (function == nil) {
    throw std::runtime_error("Metal DemosaicNet boundary: test function is missing: " +
                             std::string(function_name.UTF8String));
  }

  id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function
                                                                               error:&error];
  if (pipeline == nil) {
    throw std::runtime_error("Metal DemosaicNet boundary: failed to create test pipeline: " +
                             NSErrorMessage(error));
  }
  return pipeline;
#endif
}

void EncodeBoundaryKernel(id<MTLCommandBuffer> command_buffer, id<MTLComputePipelineState> pipeline,
                          id<MTLBuffer> src, id<MTLBuffer> dst, const BoundaryParams& params) {
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) {
    throw std::runtime_error("Metal DemosaicNet boundary: failed to create compute encoder.");
  }

  [encoder setComputePipelineState:pipeline];
  [encoder setBuffer:src offset:0 atIndex:0];
  [encoder setBuffer:dst offset:0 atIndex:1];
  [encoder setBytes:&params length:sizeof(params) atIndex:2];

  const NSUInteger element_count = params.element_count;
  const NSUInteger thread_width =
      std::max<NSUInteger>(1U, std::min(element_count, pipeline.threadExecutionWidth));
  [encoder dispatchThreads:MTLSizeMake(element_count, 1U, 1U)
      threadsPerThreadgroup:MTLSizeMake(thread_width, 1U, 1U)];
  [encoder endEncoding];
}

auto MakeMpsCommandBuffer(MTL::CommandQueue*                 queue,
                          NS::SharedPtr<MTL::CommandBuffer>& backing_command_buffer)
    -> MPSCommandBuffer* {
  backing_command_buffer = NS::RetainPtr(queue->commandBuffer());
  if (!backing_command_buffer) {
    throw std::runtime_error("Metal DemosaicNet boundary: failed to create command buffer.");
  }

  id<MTLCommandBuffer> objc_command_buffer = ToObjcCommandBuffer(backing_command_buffer.get());
  MPSCommandBuffer*    mps_command_buffer =
      [MPSCommandBuffer commandBufferWithCommandBuffer:objc_command_buffer];
  if (mps_command_buffer == nil) {
    throw std::runtime_error("Metal DemosaicNet boundary: failed to wrap command buffer.");
  }
  return mps_command_buffer;
}

void CommitAndWait(MPSCommandBuffer* command_buffer, NSError* graph_error) {
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  ThrowIfCommandFailed(command_buffer, graph_error);
}

}  // namespace

TEST(MetalDemosaicNetGraphTest, ValidOihwConvolutionUsesPrivateBuffersAndOrderedMpsCommandBuffer) {
  RunObjcBoundary([&] {
    @autoreleasepool {
      auto&              context = MetalContext::Instance();
      MTL::Device*       device  = context.Device();
      MTL::CommandQueue* queue   = context.Queue();
      ASSERT_NE(device, nullptr);
      ASSERT_NE(queue, nullptr);

      id<MTLDevice>       objc_device = ToObjcDevice(device);
      id<MTLCommandQueue> objc_queue  = ToObjcQueue(queue);
      ASSERT_NE(objc_device, nil);
      ASSERT_NE(objc_queue, nil);

      auto source_buffer   = MakeBuffer(device, kConvInputElements * sizeof(float),
                                        MTL::ResourceStorageModeShared, "source");
      auto input_buffer    = MakeBuffer(device, kConvInputElements * sizeof(float),
                                        MTL::ResourceStorageModePrivate, "graph input");
      auto output_buffer   = MakeBuffer(device, kConvOutputElements * sizeof(float),
                                        MTL::ResourceStorageModePrivate, "graph output");
      auto readback_buffer = MakeBuffer(device, kConvOutputElements * sizeof(float),
                                        MTL::ResourceStorageModeShared, "readback");

      const std::array<float, kConvInputElements> source_values = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f,
                                                                   6.0f, 7.0f, 8.0f, 9.0f};
      std::copy(source_values.begin(), source_values.end(),
                static_cast<float*>(ToObjcBuffer(source_buffer.get()).contents));

      id<MTLComputePipelineState> prepare_pipeline =
          LoadPipeline(objc_device, @"demosaicnet_boundary_prepare");
      id<MTLComputePipelineState> finish_pipeline =
          LoadPipeline(objc_device, @"demosaicnet_boundary_finish");

      MPSGraph* graph                                   = [MPSGraph new];
      graph.options                                     = MPSGraphOptionsNone;
      MPSShape*                          input_shape    = @[ @1, @3, @3, @1 ];
      MPSShape*                          output_shape   = @[ @1, @2, @2, @1 ];
      MPSGraphTensor*                    input          = [graph placeholderWithShape:input_shape
                                                 dataType:MPSDataTypeFloat32
                                                     name:@"input"];
      const std::array<float, 4>         weights_values = {1.0f, 2.0f, 3.0f, 4.0f};
      NSData*                            weights_data = [NSData dataWithBytes:weights_values.data()
                                            length:sizeof(weights_values)];
      MPSGraphTensor*                    weights      = [graph constantWithData:weights_data
                                                  shape:@[ @1, @1, @2, @2 ]
                                               dataType:MPSDataTypeFloat32];
      MPSGraphConvolution2DOpDescriptor* convolution_descriptor = [MPSGraphConvolution2DOpDescriptor
          descriptorWithStrideInX:1
                        strideInY:1
                  dilationRateInX:1
                  dilationRateInY:1
                           groups:1
                     paddingStyle:MPSGraphPaddingStyleExplicit
                       dataLayout:MPSGraphTensorNamedDataLayoutNHWC
                    weightsLayout:MPSGraphTensorNamedDataLayoutOIHW];
      ASSERT_NE(convolution_descriptor, nil);
      convolution_descriptor.paddingLeft   = 0;
      convolution_descriptor.paddingRight  = 0;
      convolution_descriptor.paddingTop    = 0;
      convolution_descriptor.paddingBottom = 0;
      MPSGraphTensor* output               = [graph convolution2DWithSourceTensor:input
                                                      weightsTensor:weights
                                                         descriptor:convolution_descriptor
                                                               name:@"valid_conv"];
      ASSERT_NE(output, nil);

      MPSGraphShapedType* feed_type = [[MPSGraphShapedType alloc] initWithShape:input_shape
                                                                       dataType:MPSDataTypeFloat32];
      MPSGraphCompilationDescriptor* compilation = [MPSGraphCompilationDescriptor new];
      compilation.waitForCompilationCompletion   = YES;
      __block NSError* compile_error             = nil;
      compilation.compilationCompletionHandler   = ^(MPSGraphExecutable*, NSError* error) {
        if (error != nil && compile_error == nil) {
          compile_error = error;
        }
      };
      MPSGraphDevice* graph_device = [MPSGraphDevice deviceWithMTLDevice:objc_device];
      ASSERT_NE(graph_device, nil);
      ASSERT_EQ(graph_device.metalDevice, objc_device);
      MPSGraphExecutable* executable = [graph compileWithDevice:graph_device
                                                          feeds:@{input : feed_type}
                                                  targetTensors:@[ output ]
                                               targetOperations:nil
                                          compilationDescriptor:compilation];
      if (compile_error != nil) {
        throw std::runtime_error("Metal DemosaicNet boundary MPSGraph compilation failed: " +
                                 NSErrorMessage(compile_error));
      }
      ASSERT_NE(executable, nil);

      MPSGraphTensorData* input_data =
          [[MPSGraphTensorData alloc] initWithMTLBuffer:ToObjcBuffer(input_buffer.get())
                                                  shape:input_shape
                                               dataType:MPSDataTypeFloat32];
      MPSGraphTensorData* output_data =
          [[MPSGraphTensorData alloc] initWithMTLBuffer:ToObjcBuffer(output_buffer.get())
                                                  shape:output_shape
                                               dataType:MPSDataTypeFloat32];
      ASSERT_NE(input_data, nil);
      ASSERT_NE(output_data, nil);

      NS::SharedPtr<MTL::CommandBuffer> backing_command_buffer;
      MPSCommandBuffer* mps_command_buffer = MakeMpsCommandBuffer(queue, backing_command_buffer);
      ASSERT_EQ(mps_command_buffer.commandBuffer.device, objc_device);
      ASSERT_EQ(mps_command_buffer.commandBuffer.commandQueue, objc_queue);

      EncodeBoundaryKernel(mps_command_buffer, prepare_pipeline, ToObjcBuffer(source_buffer.get()),
                           ToObjcBuffer(input_buffer.get()),
                           BoundaryParams{static_cast<std::uint32_t>(kConvInputElements), 1.0f});

      __block NSError*                       graph_error = nil;
      MPSGraphExecutableExecutionDescriptor* execution =
          [MPSGraphExecutableExecutionDescriptor new];
      execution.completionHandler = ^(NSArray<MPSGraphTensorData*>*, NSError* error) {
        if (error != nil && graph_error == nil) {
          graph_error = error;
        }
      };
      NSArray<MPSGraphTensorData*>* results = [executable encodeToCommandBuffer:mps_command_buffer
                                                                    inputsArray:@[ input_data ]
                                                                   resultsArray:@[ output_data ]
                                                            executionDescriptor:execution];
      ASSERT_NE(results, nil);

      EncodeBoundaryKernel(mps_command_buffer, finish_pipeline, ToObjcBuffer(output_buffer.get()),
                           ToObjcBuffer(readback_buffer.get()),
                           BoundaryParams{static_cast<std::uint32_t>(kConvOutputElements), 2.0f});

      CommitAndWait(mps_command_buffer, graph_error);

      const std::array<float, kConvOutputElements> expected = {49.0f, 59.0f, 79.0f, 89.0f};
      const float* actual = static_cast<const float*>(ToObjcBuffer(readback_buffer.get()).contents);
      for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(actual[i], expected[i], 1.0e-4f) << "output index " << i;
      }
    }
  });
}

TEST(MetalDemosaicNetGraphTest, NativeDepthToSpaceMapsAllTwelveOneHotChannels) {
  RunObjcBoundary([&] {
    @autoreleasepool {
      auto&              context = MetalContext::Instance();
      MTL::Device*       device  = context.Device();
      MTL::CommandQueue* queue   = context.Queue();
      ASSERT_NE(device, nullptr);
      ASSERT_NE(queue, nullptr);

      id<MTLDevice> objc_device   = ToObjcDevice(device);
      auto          input_buffer  = MakeBuffer(device, kUnpackInputElements * sizeof(float),
                                               MTL::ResourceStorageModeShared, "unpack input");
      auto          output_buffer = MakeBuffer(device, kUnpackOutputElements * sizeof(float),
                                               MTL::ResourceStorageModeShared, "unpack output");

      float*        input_values  = static_cast<float*>(ToObjcBuffer(input_buffer.get()).contents);
      std::fill_n(input_values, kUnpackInputElements, 0.0f);
      for (std::size_t one_hot = 0; one_hot < 12U; ++one_hot) {
        input_values[one_hot * 12U + one_hot] = 1.0f;
      }

      MPSGraph* graph                  = [MPSGraph new];
      graph.options                    = MPSGraphOptionsNone;
      MPSShape*       input_shape      = @[ @12, @1, @1, @12 ];
      MPSShape*       output_shape     = @[ @12, @2, @2, @3 ];
      MPSGraphTensor* input            = [graph placeholderWithShape:input_shape
                                                 dataType:MPSDataTypeFloat32
                                                     name:@"input"];
      MPSGraphTensor* output = [graph depthToSpace2DTensor:input
                                                 widthAxis:2
                                                heightAxis:1
                                                 depthAxis:3
                                                 blockSize:2
                                      usePixelShuffleOrder:YES
                                                        name:@"unpack"];
      ASSERT_NE(output, nil);
      ASSERT_TRUE([output.shape isEqual:output_shape]);

      MPSGraphShapedType* feed_type = [[MPSGraphShapedType alloc] initWithShape:input_shape
                                                                       dataType:MPSDataTypeFloat32];
      MPSGraphCompilationDescriptor* compilation = [MPSGraphCompilationDescriptor new];
      compilation.waitForCompilationCompletion   = YES;
      MPSGraphDevice*     graph_device           = [MPSGraphDevice deviceWithMTLDevice:objc_device];
      MPSGraphExecutable* executable             = [graph compileWithDevice:graph_device
                                                          feeds:@{input : feed_type}
                                                  targetTensors:@[ output ]
                                               targetOperations:nil
                                          compilationDescriptor:compilation];
      ASSERT_NE(executable, nil);

      MPSGraphTensorData* input_data =
          [[MPSGraphTensorData alloc] initWithMTLBuffer:ToObjcBuffer(input_buffer.get())
                                                  shape:input_shape
                                               dataType:MPSDataTypeFloat32];
      MPSGraphTensorData* output_data =
          [[MPSGraphTensorData alloc] initWithMTLBuffer:ToObjcBuffer(output_buffer.get())
                                                  shape:output_shape
                                               dataType:MPSDataTypeFloat32];
      ASSERT_NE(input_data, nil);
      ASSERT_NE(output_data, nil);

      NS::SharedPtr<MTL::CommandBuffer> backing_command_buffer;
      MPSCommandBuffer* command_buffer = MakeMpsCommandBuffer(queue, backing_command_buffer);
      __block NSError*  graph_error    = nil;
      MPSGraphExecutableExecutionDescriptor* execution =
          [MPSGraphExecutableExecutionDescriptor new];
      execution.completionHandler = ^(NSArray<MPSGraphTensorData*>*, NSError* error) {
        if (error != nil && graph_error == nil) {
          graph_error = error;
        }
      };
      NSArray<MPSGraphTensorData*>* results = [executable encodeToCommandBuffer:command_buffer
                                                                    inputsArray:@[ input_data ]
                                                                   resultsArray:@[ output_data ]
                                                            executionDescriptor:execution];
      ASSERT_NE(results, nil);
      CommitAndWait(command_buffer, graph_error);

      const float* actual = static_cast<const float*>(ToObjcBuffer(output_buffer.get()).contents);
      for (std::size_t one_hot = 0; one_hot < 12U; ++one_hot) {
        const std::size_t expected_rgb = one_hot / 4U;
        const std::size_t expected_y   = (one_hot % 4U) / 2U;
        const std::size_t expected_x   = one_hot % 2U;
        for (std::size_t y = 0; y < 2U; ++y) {
          for (std::size_t x = 0; x < 2U; ++x) {
            for (std::size_t rgb = 0; rgb < 3U; ++rgb) {
              const std::size_t index = (((one_hot * 2U + y) * 2U + x) * 3U) + rgb;
              const float       expected =
                  (y == expected_y && x == expected_x && rgb == expected_rgb) ? 1.0f : 0.0f;
              EXPECT_NEAR(actual[index], expected, 1.0e-4f)
                  << "one-hot channel " << one_hot << " at [" << y << "," << x << "," << rgb << "]";
            }
          }
        }
      }
    }
  });
}

}  // namespace alcedo

#endif  // HAVE_METAL
