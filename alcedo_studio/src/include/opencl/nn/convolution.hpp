//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "opencl/nn/common.hpp"
#include "opencl/nn/tensor_view.hpp"

namespace alcedo::opencl::nn {

// ---------------------------------------------------------------------------
// OHWI4o4i weight packing (host-side, one-time per model load)
//
// Layout: [out_block][ky][kx][in_block][out_lane][in_lane]
// Source: PyTorch / safetensors OIHW [out_c, in_c, kH, kW]
// Padded physical lanes are zero-filled; logical channel counts stay in metadata.
// ---------------------------------------------------------------------------

[[nodiscard]] auto Ohwi4o4iElementCount(int out_channels, int in_channels, int kernel_h,
                                        int kernel_w) -> std::size_t;

// Pack OIHW FP32 weights into OHWI4o4i. Output size is Ohwi4o4iElementCount(...).
[[nodiscard]] auto PackOhwi4o4iFromOihw(const float* oihw, int out_channels, int in_channels,
                                        int kernel_h, int kernel_w) -> std::vector<float>;

void PackOhwi4o4iFromOihw(const float* oihw, int out_channels, int in_channels, int kernel_h,
                          int kernel_w, float* dst_ohwi4o4i);

// ---------------------------------------------------------------------------
// Kernel argument binding / enqueue helpers
//
// Callers must supply an already-created cl_kernel. These helpers never call
// OpenClProgramLibrary::GetProgram and never trigger an implicit program build.
// They never call clFinish; product code waits once at the Neural stage end.
// ---------------------------------------------------------------------------

// Bind arguments that are immutable for one fixed network layer. The enqueue
// helpers bind the input/output buffers and spatial dimensions on the first
// dispatch, unless a resident caller supplies all_args_bound after binding
// them separately. A layer must use a dedicated cl_kernel object after calling
// one of these functions; cl_kernel argument state is mutable.
void BindConv3x3Nhwc4InvariantArgs(cl_kernel kernel, cl_mem weights, cl_mem bias,
                                   int in_channel_blocks, int out_channel_blocks,
                                   int in_logical_channels, int out_logical_channels);

void BindConv1x1Nhwc4InvariantArgs(cl_kernel kernel, cl_mem weights, cl_mem bias,
                                   int in_channel_blocks, int out_channel_blocks,
                                   int in_logical_channels, int out_logical_channels,
                                   int apply_relu);

// Optional event for development harness timing. When non-null, the enqueue
// retains a CL event the caller must release after profiling.
struct EnqueueOptions {
  cl_event* event = nullptr;  // optional out-event; ownership stays with the caller
};

// Instrumentation counters for tests / diagnostics. Thread-local to the calling
// thread; product code may ignore them.
struct DispatchInstrumentation {
  std::uint64_t enqueue_count = 0;
  std::uint64_t finish_count  = 0;  // must remain 0 for primitive dispatch helpers
  std::uint64_t wait_count    = 0;  // explicit Wait* helpers only
};

// Returns the thread-local instrumentation counters (tests reset between cases).
[[nodiscard]] auto GetDispatchInstrumentation() -> DispatchInstrumentation&;
void               ResetDispatchInstrumentation();

// Explicit wait helpers (for tests / stage boundaries). Product tile loops must
// not call these between layers.
void WaitQueue(cl_command_queue queue);
void WaitEvent(cl_event event);

// ---------------------------------------------------------------------------
// Direct 3x3 NHWC4 convolution (+ fused bias + ReLU when program built that way)
//
// Kernel: demosaicnet_conv3x3_nhwc4
// Global size: (out_w, out_h, batch); each work-group computes every output
// channel block for its spatial tile.
// pad_h/pad_w: zero-pad amount (product valid conv uses 0).
// bias may be nullptr; the kernel checks it before applying the fused bias.
// ---------------------------------------------------------------------------

struct Conv3x3Dispatch {
  cl_kernel            kernel  = nullptr;  // demosaicnet_conv3x3_nhwc4
  const Nhwc4TensorView* input  = nullptr;
  const Nhwc4TensorView* output = nullptr;
  cl_mem               weights = nullptr;  // OHWI4o4i
  cl_mem               bias    = nullptr;  // length out_logical_channels, or nullptr
  int                  pad_h   = 0;
  int                  pad_w   = 0;
  // When true, args 1, 2, and 11..14 were installed by
  // BindConv3x3Nhwc4InvariantArgs on this dedicated kernel object.
  bool                 invariant_args_bound = false;
  // When true, all remaining launch arguments are already installed on the
  // dedicated kernel object and the enqueue helper only submits the NDRange.
  bool                 all_args_bound = false;
};

// Installs the launch arguments that depend on the current tensor geometry.
// This is separate from enqueue so a resident caller can bind them once and
// replay the same fixed-shape dispatch across several tiles.
void BindConv3x3Nhwc4DispatchArgs(const Conv3x3Dispatch& dispatch);

// Validates geometry and enqueues. Does not synchronize.
void EnqueueConv3x3Nhwc4(const Conv3x3Dispatch& dispatch, cl_command_queue queue,
                         const EnqueueOptions& options = {});

// ---------------------------------------------------------------------------
// Specialized 1x1 NHWC4 convolution (C4-aligned blocks, runtime ReLU flag)
//
// Kernel: demosaicnet_conv1x1_nhwc4
// Global size: (width, height, batch); each work-item computes every output
// channel block at its spatial position.
// ---------------------------------------------------------------------------

struct Conv1x1Dispatch {
  cl_kernel            kernel     = nullptr;  // demosaicnet_conv1x1_nhwc4
  const Nhwc4TensorView* input    = nullptr;
  const Nhwc4TensorView* output   = nullptr;
  cl_mem               weights    = nullptr;  // OHWI4o4i 1x1
  cl_mem               bias       = nullptr;  // optional
  int                  apply_relu = 1;
  // When true, args 1, 2, and 7..11 were installed by
  // BindConv1x1Nhwc4InvariantArgs on this dedicated kernel object.
  bool                 invariant_args_bound = false;
  bool                 all_args_bound       = false;
};

void BindConv1x1Nhwc4DispatchArgs(const Conv1x1Dispatch& dispatch);

void EnqueueConv1x1Nhwc4(const Conv1x1Dispatch& dispatch, cl_command_queue queue,
                         const EnqueueOptions& options = {});

// Spatial output size for stride-1 dilate-1 valid convolution (pad may be > 0).
[[nodiscard]] inline auto ConvOutputSize(int input_size, int pad, int kernel) -> int {
  if (input_size < 0 || pad < 0 || kernel < 1) {
    return -1;
  }
  return input_size + 2 * pad - kernel + 1;
}

// Event duration in nanoseconds (requires CL_QUEUE_PROFILING_ENABLE).
[[nodiscard]] auto EventDurationNs(cl_event event) -> std::uint64_t;

}  // namespace alcedo::opencl::nn

#endif  // HAVE_OPENCL
