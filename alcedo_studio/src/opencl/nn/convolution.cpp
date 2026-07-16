//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "opencl/nn/convolution.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "opencl/nn/demosaicnet_stage_profiler.hpp"
#include "opencl/opencl_api_counters.hpp"

namespace alcedo::opencl::nn {
namespace {

thread_local DispatchInstrumentation g_instrumentation{};

void ValidateNhwc4(const Nhwc4TensorView& view, const char* what) {
  if (view.buffer == nullptr) {
    throw std::runtime_error(std::string(what) + ": null buffer");
  }
  if (view.batch <= 0 || view.height <= 0 || view.width <= 0) {
    throw std::runtime_error(std::string(what) + ": non-positive spatial/batch dimensions");
  }
  if (view.logical_channels < 0 || view.channel_blocks <= 0) {
    throw std::runtime_error(std::string(what) + ": invalid channel metadata");
  }
  if (view.channel_blocks < ChannelBlocks(view.logical_channels)) {
    throw std::runtime_error(std::string(what) + ": channel_blocks too small for logical channels");
  }
  if (view.byte_offset != 0) {
    throw std::runtime_error(std::string(what) +
                             ": non-zero byte_offset requires a SubBuffer-bound view");
  }
}

template <typename T>
void SetArg(cl_kernel kernel, cl_uint index, const T& value, const char* what) {
  CheckOpenCl(clSetKernelArg(kernel, index, sizeof(T), &value), what);
}

void Enqueue3D(cl_kernel kernel, size_t gx, size_t gy, size_t gz, cl_command_queue queue,
               const EnqueueOptions& options, const char* what) {
  if (kernel == nullptr) {
    throw std::runtime_error(std::string(what) + ": null kernel");
  }
  if (queue == nullptr) {
    throw std::runtime_error(std::string(what) + ": null command queue");
  }
  if (gx == 0 || gy == 0 || gz == 0) {
    throw std::runtime_error(std::string(what) + ": zero global size");
  }
  const size_t global[3] = {gx, gy, gz};
  ScopedStageEvent stage_event;
  cl_event*        event_out = options.event != nullptr ? options.event : stage_event.out();
  CheckOpenCl(clEnqueueNDRangeKernel(queue, kernel, 3, nullptr, global, nullptr, 0, nullptr,
                                     event_out),
              what);
  ++g_instrumentation.enqueue_count;
  NoteOpenClEnqueueNdRange();
  // Intentionally no clFinish / event wait here.
}

void EnqueueConvTile3D(cl_kernel kernel, size_t gx, size_t gy, size_t gz,
                       cl_command_queue queue, const EnqueueOptions& options, const char* what) {
  if (kernel == nullptr) {
    throw std::runtime_error(std::string(what) + ": null kernel");
  }
  if (queue == nullptr) {
    throw std::runtime_error(std::string(what) + ": null command queue");
  }
  if (gx == 0 || gy == 0 || gz == 0) {
    throw std::runtime_error(std::string(what) + ": zero global size");
  }
  constexpr size_t kLocalX = 16;
  constexpr size_t kLocalY = 8;
  const size_t global[3] = {
      ((gx + kLocalX - 1) / kLocalX) * kLocalX,
      ((gy + kLocalY - 1) / kLocalY) * kLocalY,
      gz,
  };
  const size_t local[3] = {kLocalX, kLocalY, 1};
  ScopedStageEvent stage_event;
  cl_event*        event_out = options.event != nullptr ? options.event : stage_event.out();
  CheckOpenCl(clEnqueueNDRangeKernel(queue, kernel, 3, nullptr, global, local, 0, nullptr,
                                     event_out),
              what);
  ++g_instrumentation.enqueue_count;
  NoteOpenClEnqueueNdRange();
  // Intentionally no clFinish / event wait here.
}

}  // namespace

auto GetDispatchInstrumentation() -> DispatchInstrumentation& { return g_instrumentation; }

void ResetDispatchInstrumentation() { g_instrumentation = {}; }

void BindConv3x3Nhwc4InvariantArgs(const cl_kernel kernel, const cl_mem weights,
                                   const cl_mem bias, const int in_channel_blocks,
                                   const int out_channel_blocks, const int in_logical_channels,
                                   const int out_logical_channels) {
  if (kernel == nullptr) {
    throw std::runtime_error("BindConv3x3Nhwc4InvariantArgs: null kernel");
  }
  if (weights == nullptr) {
    throw std::runtime_error("BindConv3x3Nhwc4InvariantArgs: null weights");
  }
  if (in_channel_blocks <= 0 || out_channel_blocks <= 0 || in_logical_channels < 0 ||
      out_logical_channels < 0) {
    throw std::runtime_error("BindConv3x3Nhwc4InvariantArgs: invalid channel metadata");
  }

  SetArg(kernel, 1, weights, "BindConv3x3Nhwc4InvariantArgs weights");
  if (bias != nullptr) {
    SetArg(kernel, 2, bias, "BindConv3x3Nhwc4InvariantArgs bias");
  } else {
    CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(cl_mem), nullptr),
                "BindConv3x3Nhwc4InvariantArgs null bias");
  }
  SetArg(kernel, 11, in_channel_blocks, "BindConv3x3Nhwc4InvariantArgs input blocks");
  SetArg(kernel, 12, out_channel_blocks, "BindConv3x3Nhwc4InvariantArgs output blocks");
  SetArg(kernel, 13, in_logical_channels, "BindConv3x3Nhwc4InvariantArgs input channels");
  SetArg(kernel, 14, out_logical_channels, "BindConv3x3Nhwc4InvariantArgs output channels");
}

void BindConv1x1Nhwc4InvariantArgs(const cl_kernel kernel, const cl_mem weights,
                                   const cl_mem bias, const int in_channel_blocks,
                                   const int out_channel_blocks, const int in_logical_channels,
                                   const int out_logical_channels, const int apply_relu) {
  if (kernel == nullptr) {
    throw std::runtime_error("BindConv1x1Nhwc4InvariantArgs: null kernel");
  }
  if (weights == nullptr) {
    throw std::runtime_error("BindConv1x1Nhwc4InvariantArgs: null weights");
  }
  if (in_channel_blocks <= 0 || out_channel_blocks <= 0 || in_logical_channels < 0 ||
      out_logical_channels < 0) {
    throw std::runtime_error("BindConv1x1Nhwc4InvariantArgs: invalid channel metadata");
  }

  SetArg(kernel, 1, weights, "BindConv1x1Nhwc4InvariantArgs weights");
  if (bias != nullptr) {
    SetArg(kernel, 2, bias, "BindConv1x1Nhwc4InvariantArgs bias");
  } else {
    CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(cl_mem), nullptr),
                "BindConv1x1Nhwc4InvariantArgs null bias");
  }
  SetArg(kernel, 7, in_channel_blocks, "BindConv1x1Nhwc4InvariantArgs input blocks");
  SetArg(kernel, 8, out_channel_blocks, "BindConv1x1Nhwc4InvariantArgs output blocks");
  SetArg(kernel, 9, in_logical_channels, "BindConv1x1Nhwc4InvariantArgs input channels");
  SetArg(kernel, 10, out_logical_channels, "BindConv1x1Nhwc4InvariantArgs output channels");
  SetArg(kernel, 11, apply_relu != 0 ? 1 : 0, "BindConv1x1Nhwc4InvariantArgs ReLU");
}

void BindConv3x3Nhwc4DispatchArgs(const Conv3x3Dispatch& dispatch) {
  if (dispatch.kernel == nullptr || dispatch.input == nullptr || dispatch.output == nullptr) {
    throw std::runtime_error("BindConv3x3Nhwc4DispatchArgs: null kernel or tensor view");
  }
  if (dispatch.weights == nullptr) {
    throw std::runtime_error("BindConv3x3Nhwc4DispatchArgs: null weights");
  }
  ValidateNhwc4(*dispatch.input, "BindConv3x3Nhwc4DispatchArgs input");
  ValidateNhwc4(*dispatch.output, "BindConv3x3Nhwc4DispatchArgs output");

  const auto& in  = *dispatch.input;
  const auto& out = *dispatch.output;
  if (in.batch != out.batch) {
    throw std::runtime_error("BindConv3x3Nhwc4DispatchArgs: batch mismatch");
  }
  const int expected_out_h = ConvOutputSize(in.height, dispatch.pad_h, 3);
  const int expected_out_w = ConvOutputSize(in.width, dispatch.pad_w, 3);
  if (out.height != expected_out_h || out.width != expected_out_w) {
    throw std::runtime_error("BindConv3x3Nhwc4DispatchArgs: output spatial size mismatch");
  }

  const cl_int batch                = in.batch;
  const cl_int in_h                 = in.height;
  const cl_int in_w                 = in.width;
  const cl_int out_h                = out.height;
  const cl_int out_w                = out.width;
  const cl_int pad_h                = dispatch.pad_h;
  const cl_int pad_w                = dispatch.pad_w;
  const cl_int in_channel_blocks    = in.channel_blocks;
  const cl_int out_channel_blocks   = out.channel_blocks;
  const cl_int in_logical_channels  = in.logical_channels;
  const cl_int out_logical_channels = out.logical_channels;
  const cl_mem input_buf            = in.buffer;
  const cl_mem output_buf           = out.buffer;
  const cl_mem weights              = dispatch.weights;
  const cl_mem bias                 = dispatch.bias;

  SetArg(dispatch.kernel, 0, input_buf, "BindConv3x3Nhwc4DispatchArgs arg0");
  if (!dispatch.invariant_args_bound) {
    SetArg(dispatch.kernel, 1, weights, "BindConv3x3Nhwc4DispatchArgs arg1");
    if (bias != nullptr) {
      SetArg(dispatch.kernel, 2, bias, "BindConv3x3Nhwc4DispatchArgs arg2");
    } else {
      CheckOpenCl(clSetKernelArg(dispatch.kernel, 2, sizeof(cl_mem), nullptr),
                  "BindConv3x3Nhwc4DispatchArgs arg2 null bias");
    }
  }
  SetArg(dispatch.kernel, 3, output_buf, "BindConv3x3Nhwc4DispatchArgs arg3");
  SetArg(dispatch.kernel, 4, batch, "BindConv3x3Nhwc4DispatchArgs arg4");
  SetArg(dispatch.kernel, 5, in_h, "BindConv3x3Nhwc4DispatchArgs arg5");
  SetArg(dispatch.kernel, 6, in_w, "BindConv3x3Nhwc4DispatchArgs arg6");
  SetArg(dispatch.kernel, 7, out_h, "BindConv3x3Nhwc4DispatchArgs arg7");
  SetArg(dispatch.kernel, 8, out_w, "BindConv3x3Nhwc4DispatchArgs arg8");
  SetArg(dispatch.kernel, 9, pad_h, "BindConv3x3Nhwc4DispatchArgs arg9");
  SetArg(dispatch.kernel, 10, pad_w, "BindConv3x3Nhwc4DispatchArgs arg10");
  if (!dispatch.invariant_args_bound) {
    SetArg(dispatch.kernel, 11, in_channel_blocks, "BindConv3x3Nhwc4DispatchArgs arg11");
    SetArg(dispatch.kernel, 12, out_channel_blocks, "BindConv3x3Nhwc4DispatchArgs arg12");
    SetArg(dispatch.kernel, 13, in_logical_channels, "BindConv3x3Nhwc4DispatchArgs arg13");
    SetArg(dispatch.kernel, 14, out_logical_channels, "BindConv3x3Nhwc4DispatchArgs arg14");
  }
}

void BindConv1x1Nhwc4DispatchArgs(const Conv1x1Dispatch& dispatch) {
  if (dispatch.kernel == nullptr || dispatch.input == nullptr || dispatch.output == nullptr) {
    throw std::runtime_error("BindConv1x1Nhwc4DispatchArgs: null kernel or tensor view");
  }
  if (dispatch.weights == nullptr) {
    throw std::runtime_error("BindConv1x1Nhwc4DispatchArgs: null weights");
  }
  ValidateNhwc4(*dispatch.input, "BindConv1x1Nhwc4DispatchArgs input");
  ValidateNhwc4(*dispatch.output, "BindConv1x1Nhwc4DispatchArgs output");

  const auto& in  = *dispatch.input;
  const auto& out = *dispatch.output;
  if (in.batch != out.batch || in.height != out.height || in.width != out.width) {
    throw std::runtime_error("BindConv1x1Nhwc4DispatchArgs: batch/spatial mismatch");
  }

  const cl_int batch                = in.batch;
  const cl_int height               = in.height;
  const cl_int width                = in.width;
  const cl_int in_channel_blocks    = in.channel_blocks;
  const cl_int out_channel_blocks   = out.channel_blocks;
  const cl_int in_logical_channels  = in.logical_channels;
  const cl_int out_logical_channels = out.logical_channels;
  const cl_int apply_relu           = dispatch.apply_relu != 0 ? 1 : 0;
  const cl_mem input_buf            = in.buffer;
  const cl_mem output_buf           = out.buffer;
  const cl_mem weights              = dispatch.weights;
  const cl_mem bias                 = dispatch.bias;

  SetArg(dispatch.kernel, 0, input_buf, "BindConv1x1Nhwc4DispatchArgs arg0");
  if (!dispatch.invariant_args_bound) {
    SetArg(dispatch.kernel, 1, weights, "BindConv1x1Nhwc4DispatchArgs arg1");
    if (bias != nullptr) {
      SetArg(dispatch.kernel, 2, bias, "BindConv1x1Nhwc4DispatchArgs arg2");
    } else {
      CheckOpenCl(clSetKernelArg(dispatch.kernel, 2, sizeof(cl_mem), nullptr),
                  "BindConv1x1Nhwc4DispatchArgs arg2 null bias");
    }
  }
  SetArg(dispatch.kernel, 3, output_buf, "BindConv1x1Nhwc4DispatchArgs arg3");
  SetArg(dispatch.kernel, 4, batch, "BindConv1x1Nhwc4DispatchArgs arg4");
  SetArg(dispatch.kernel, 5, height, "BindConv1x1Nhwc4DispatchArgs arg5");
  SetArg(dispatch.kernel, 6, width, "BindConv1x1Nhwc4DispatchArgs arg6");
  if (!dispatch.invariant_args_bound) {
    SetArg(dispatch.kernel, 7, in_channel_blocks, "BindConv1x1Nhwc4DispatchArgs arg7");
    SetArg(dispatch.kernel, 8, out_channel_blocks, "BindConv1x1Nhwc4DispatchArgs arg8");
    SetArg(dispatch.kernel, 9, in_logical_channels, "BindConv1x1Nhwc4DispatchArgs arg9");
    SetArg(dispatch.kernel, 10, out_logical_channels, "BindConv1x1Nhwc4DispatchArgs arg10");
    SetArg(dispatch.kernel, 11, apply_relu, "BindConv1x1Nhwc4DispatchArgs arg11");
  }
}

void WaitQueue(cl_command_queue queue) {
  if (queue == nullptr) {
    throw std::runtime_error("WaitQueue: null queue");
  }
  CheckOpenCl(clFinish(queue), "WaitQueue");
  ++g_instrumentation.finish_count;
  ++g_instrumentation.wait_count;
  NoteOpenClFinalWait();
  NoteOpenClQueueFinish();
}

void WaitEvent(cl_event event) {
  if (event == nullptr) {
    throw std::runtime_error("WaitEvent: null event");
  }
  CheckOpenCl(clWaitForEvents(1, &event), "WaitEvent");
  ++g_instrumentation.wait_count;
}

auto Ohwi4o4iElementCount(int out_channels, int in_channels, int kernel_h, int kernel_w)
    -> std::size_t {
  if (out_channels < 0 || in_channels < 0 || kernel_h < 1 || kernel_w < 1) {
    throw std::runtime_error("Ohwi4o4iElementCount: invalid geometry");
  }
  const int out_blocks = ChannelBlocks(out_channels);
  const int in_blocks  = ChannelBlocks(in_channels);
  return static_cast<std::size_t>(out_blocks) * static_cast<std::size_t>(kernel_h) *
         static_cast<std::size_t>(kernel_w) * static_cast<std::size_t>(in_blocks) * 4u * 4u;
}

void PackOhwi4o4iFromOihw(const float* oihw, int out_channels, int in_channels, int kernel_h,
                          int kernel_w, float* dst) {
  if (oihw == nullptr || dst == nullptr) {
    throw std::runtime_error("PackOhwi4o4iFromOihw: null pointer");
  }
  if (out_channels < 0 || in_channels < 0 || kernel_h < 1 || kernel_w < 1) {
    throw std::runtime_error("PackOhwi4o4iFromOihw: invalid geometry");
  }

  const int out_blocks = ChannelBlocks(out_channels);
  const int in_blocks  = ChannelBlocks(in_channels);
  const int k_area     = kernel_h * kernel_w;

  std::size_t dst_i = 0;
  for (int out_b = 0; out_b < out_blocks; ++out_b) {
    for (int ky = 0; ky < kernel_h; ++ky) {
      for (int kx = 0; kx < kernel_w; ++kx) {
        for (int in_b = 0; in_b < in_blocks; ++in_b) {
          for (int out_lane = 0; out_lane < 4; ++out_lane) {
            for (int in_lane = 0; in_lane < 4; ++in_lane) {
              const int oc = out_b * 4 + out_lane;
              const int ic = in_b * 4 + in_lane;
              float     v  = 0.0f;
              if (oc < out_channels && ic < in_channels) {
                // OIHW: [oc, ic, ky, kx]
                const std::size_t src_i =
                    (static_cast<std::size_t>(oc) * static_cast<std::size_t>(in_channels) +
                     static_cast<std::size_t>(ic)) *
                        static_cast<std::size_t>(k_area) +
                    static_cast<std::size_t>(ky) * static_cast<std::size_t>(kernel_w) +
                    static_cast<std::size_t>(kx);
                v = oihw[src_i];
              }
              dst[dst_i++] = v;
            }
          }
        }
      }
    }
  }
}

auto PackOhwi4o4iFromOihw(const float* oihw, int out_channels, int in_channels, int kernel_h,
                          int kernel_w) -> std::vector<float> {
  std::vector<float> packed(Ohwi4o4iElementCount(out_channels, in_channels, kernel_h, kernel_w));
  PackOhwi4o4iFromOihw(oihw, out_channels, in_channels, kernel_h, kernel_w, packed.data());
  return packed;
}

void EnqueueConv3x3Nhwc4(const Conv3x3Dispatch& dispatch, cl_command_queue queue,
                         const EnqueueOptions& options) {
  if (dispatch.kernel == nullptr) {
    throw std::runtime_error("EnqueueConv3x3Nhwc4: null kernel");
  }
  if (dispatch.input == nullptr || dispatch.output == nullptr) {
    throw std::runtime_error("EnqueueConv3x3Nhwc4: null input/output view");
  }
  if (dispatch.weights == nullptr) {
    throw std::runtime_error("EnqueueConv3x3Nhwc4: null weights");
  }
  ValidateNhwc4(*dispatch.input, "EnqueueConv3x3Nhwc4 input");
  ValidateNhwc4(*dispatch.output, "EnqueueConv3x3Nhwc4 output");

  const auto& in  = *dispatch.input;
  const auto& out = *dispatch.output;
  if (in.batch != out.batch) {
    throw std::runtime_error("EnqueueConv3x3Nhwc4: batch mismatch");
  }

  const int expected_out_h = ConvOutputSize(in.height, dispatch.pad_h, 3);
  const int expected_out_w = ConvOutputSize(in.width, dispatch.pad_w, 3);
  if (out.height != expected_out_h || out.width != expected_out_w) {
    throw std::runtime_error("EnqueueConv3x3Nhwc4: output spatial size mismatch");
  }

  if (!dispatch.all_args_bound) {
    BindConv3x3Nhwc4DispatchArgs(dispatch);
  }

  // The tiled kernel computes every output channel block for one spatial tile
  // and batch plane; z is therefore batch-only, matching the CUDA mapping.
  EnqueueConvTile3D(dispatch.kernel, static_cast<size_t>(out.width),
                    static_cast<size_t>(out.height), static_cast<size_t>(in.batch), queue, options,
                    "EnqueueConv3x3Nhwc4");
}

void EnqueueConv1x1Nhwc4(const Conv1x1Dispatch& dispatch, cl_command_queue queue,
                         const EnqueueOptions& options) {
  if (dispatch.kernel == nullptr) {
    throw std::runtime_error("EnqueueConv1x1Nhwc4: null kernel");
  }
  if (dispatch.input == nullptr || dispatch.output == nullptr) {
    throw std::runtime_error("EnqueueConv1x1Nhwc4: null input/output view");
  }
  if (dispatch.weights == nullptr) {
    throw std::runtime_error("EnqueueConv1x1Nhwc4: null weights");
  }
  ValidateNhwc4(*dispatch.input, "EnqueueConv1x1Nhwc4 input");
  ValidateNhwc4(*dispatch.output, "EnqueueConv1x1Nhwc4 output");

  const auto& in  = *dispatch.input;
  const auto& out = *dispatch.output;
  if (in.batch != out.batch || in.height != out.height || in.width != out.width) {
    throw std::runtime_error("EnqueueConv1x1Nhwc4: batch/spatial mismatch");
  }

  if (!dispatch.all_args_bound) {
    BindConv1x1Nhwc4DispatchArgs(dispatch);
  }

  // One work-item owns one spatial position and all output channel blocks.
  Enqueue3D(dispatch.kernel, static_cast<size_t>(out.width), static_cast<size_t>(out.height),
            static_cast<size_t>(in.batch), queue, options, "EnqueueConv1x1Nhwc4");
}

auto EventDurationNs(cl_event event) -> std::uint64_t {
  if (event == nullptr) {
    throw std::runtime_error("EventDurationNs: null event");
  }
  CheckOpenCl(clWaitForEvents(1, &event), "EventDurationNs wait");
  cl_ulong start = 0;
  cl_ulong end   = 0;
  CheckOpenCl(clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_START, sizeof(start), &start,
                                      nullptr),
              "EventDurationNs start");
  CheckOpenCl(
      clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_END, sizeof(end), &end, nullptr),
      "EventDurationNs end");
  if (end < start) {
    return 0;
  }
  return static_cast<std::uint64_t>(end - start);
}

}  // namespace alcedo::opencl::nn

#endif  // HAVE_OPENCL
