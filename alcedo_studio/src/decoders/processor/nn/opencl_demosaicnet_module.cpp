//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "decoders/processor/nn/opencl_demosaicnet_module.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "decoders/processor/operators/gpu/opencl_demosaicnet_programs.hpp"
#include "opencl/nn/activation_slots.hpp"
#include "opencl/nn/common.hpp"
#include "opencl/nn/convolution.hpp"
#include "opencl/nn/demosaicnet_stage_profiler.hpp"
#include "opencl/nn/tensor_view.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_backend_program_registry.hpp"
#include "opencl/opencl_program_library.hpp"

namespace alcedo {
namespace {

namespace nn_ocl = opencl::nn;

// ---------------------------------------------------------------------------
// Kernel RAII
// ---------------------------------------------------------------------------

class KernelHolder {
 public:
  KernelHolder() = default;
  KernelHolder(cl_program program, const char* name) {
    cl_int err = CL_SUCCESS;
    kernel_    = clCreateKernel(program, name, &err);
    nn_ocl::CheckOpenCl(err, name);
    NoteOpenClCreateKernel();
  }
  ~KernelHolder() { Reset(); }

  KernelHolder(const KernelHolder&)            = delete;
  KernelHolder& operator=(const KernelHolder&) = delete;

  KernelHolder(KernelHolder&& other) noexcept : kernel_(other.kernel_) { other.kernel_ = nullptr; }
  auto operator=(KernelHolder&& other) noexcept -> KernelHolder& {
    if (this != &other) {
      Reset();
      kernel_       = other.kernel_;
      other.kernel_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] auto get() const -> cl_kernel { return kernel_; }
  [[nodiscard]] auto empty() const -> bool { return kernel_ == nullptr; }

  void               Reset() noexcept {
    if (kernel_ != nullptr) {
      clReleaseKernel(kernel_);
      NoteOpenClReleaseKernel();
      kernel_ = nullptr;
    }
  }

 private:
  cl_kernel kernel_ = nullptr;
};

// A fixed network layer owns a dedicated kernel object so its immutable
// arguments can be installed once at model-load time. OpenCL kernel argument
// state is mutable, therefore sharing one kernel across trunk layers would
// make this optimization unsafe.
struct BoundConvKernel {
  struct Conv3x3LaunchKey {
    cl_mem input   = nullptr;
    cl_mem output  = nullptr;
    int    batch   = 0;
    int    in_h    = 0;
    int    in_w    = 0;
    int    out_h   = 0;
    int    out_w   = 0;
    int    pad_h   = 0;
    int    pad_w   = 0;

    bool operator==(const Conv3x3LaunchKey&) const = default;
  };

  struct Conv1x1LaunchKey {
    cl_mem input  = nullptr;
    cl_mem output = nullptr;
    int    batch  = 0;
    int    height = 0;
    int    width  = 0;

    bool operator==(const Conv1x1LaunchKey&) const = default;
  };

  KernelHolder kernel;
  cl_mem       weights = nullptr;
  cl_mem       bias    = nullptr;

  void Bind3x3(const cl_mem weights_in, const cl_mem bias_in, const int in_blocks,
               const int out_blocks, const int in_channels, const int out_channels) {
    weights = weights_in;
    bias    = bias_in;
    nn_ocl::BindConv3x3Nhwc4InvariantArgs(kernel.get(), weights, bias, in_blocks, out_blocks,
                                          in_channels, out_channels);
  }

  void Bind1x1(const cl_mem weights_in, const cl_mem bias_in, const int in_blocks,
               const int out_blocks, const int in_channels, const int out_channels,
               const int apply_relu) {
    weights = weights_in;
    bias    = bias_in;
    nn_ocl::BindConv1x1Nhwc4InvariantArgs(kernel.get(), weights, bias, in_blocks, out_blocks,
                                          in_channels, out_channels, apply_relu);
  }

  // Product decode is serialized by g_neural_decode_mutex, so the mutable
  // launch arguments can be resident on this kernel object. The key also
  // invalidates the cache when a boundary tile changes geometry or a caller
  // supplies different slot buffers.
  void Prepare3x3(const nn_ocl::Conv3x3Dispatch& dispatch) const {
    const auto& in  = *dispatch.input;
    const auto& out = *dispatch.output;
    const Conv3x3LaunchKey key{in.buffer,  out.buffer, in.batch, in.height, in.width,
                               out.height, out.width, dispatch.pad_h, dispatch.pad_w};
    if (!has_3x3_launch_ || key != launch_3x3_) {
      nn_ocl::BindConv3x3Nhwc4DispatchArgs(dispatch);
      launch_3x3_     = key;
      has_3x3_launch_ = true;
    }
  }

  void Prepare1x1(const nn_ocl::Conv1x1Dispatch& dispatch) const {
    const auto& in  = *dispatch.input;
    const auto& out = *dispatch.output;
    const Conv1x1LaunchKey key{in.buffer, out.buffer, in.batch, in.height, in.width};
    if (!has_1x1_launch_ || key != launch_1x1_) {
      nn_ocl::BindConv1x1Nhwc4DispatchArgs(dispatch);
      launch_1x1_     = key;
      has_1x1_launch_ = true;
    }
  }

 private:
  mutable bool               has_3x3_launch_ = false;
  mutable Conv3x3LaunchKey   launch_3x3_;
  mutable bool               has_1x1_launch_ = false;
  mutable Conv1x1LaunchKey   launch_1x1_;
};

// ---------------------------------------------------------------------------
// Shared helpers
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

[[nodiscard]] auto ResolveQueue(cl_command_queue queue) -> cl_command_queue {
  if (queue != nullptr) {
    return queue;
  }
  auto& ctx = OpenClContext::Instance();
  if (!ctx.IsInitialized()) {
    throw std::runtime_error("OpenCL DemosaicNet: OpenClContext not initialized");
  }
  return ctx.Queue();
}

[[nodiscard]] auto Nhwc4Bytes(int batch, int height, int width, int logical_channels,
                              int channel_blocks = -1) -> std::size_t {
  if (channel_blocks < 0) {
    channel_blocks = nn_ocl::ChannelBlocks(logical_channels);
  }
  return static_cast<std::size_t>(batch) * static_cast<std::size_t>(height) *
         static_cast<std::size_t>(width) * static_cast<std::size_t>(channel_blocks) * 4u *
         sizeof(float);
}

[[nodiscard]] auto AlignUp(std::size_t value, std::size_t alignment) -> std::size_t {
  return (value + alignment - 1) & ~(alignment - 1);
}

// Peak-live NHWC4 size for one of the two dedicated ping-pong activation slots.
[[nodiscard]] auto EstimateNhwc4ActivationSlotBytes(int input_h, int input_w, int batch,
                                                    int pack_out_ch, int width, int residual_ch,
                                                    int depth, int pack_factor) -> std::size_t {
  if (batch < 1 || input_h < 1 || input_w < 1 || pack_factor < 1 || depth < 1) {
    return 0;
  }
  if ((input_h % pack_factor) != 0 || (input_w % pack_factor) != 0) {
    return 0;
  }
  const int ph = input_h / pack_factor;
  const int pw = input_w / pack_factor;
  if (ph <= 2 * depth || pw <= 2 * depth) {
    return 0;
  }
  const int mh = ph - 2 * depth;
  const int mw = pw - 2 * depth;
  const int uh = mh * pack_factor;
  const int uw = mw * pack_factor;
  const int nh = uh - 2;
  const int nw = uw - 2;
  if (mh < 1 || mw < 1 || nh < 1 || nw < 1) {
    return 0;
  }

  // Physical blocks for C6 post input (logical 6 → 2 blocks).
  constexpr int kPostInBlocks = 2;
  std::size_t   trunk         = Nhwc4Bytes(batch, ph, pw, pack_out_ch);
  trunk                       = std::max(trunk, Nhwc4Bytes(batch, ph - 2, pw - 2, width));
  trunk                       = std::max(trunk, Nhwc4Bytes(batch, mh, mw, residual_ch));
  trunk                       = std::max(trunk, Nhwc4Bytes(batch, uh, uw, 6, kPostInBlocks));
  trunk                       = std::max(trunk, Nhwc4Bytes(batch, nh, nw, width));
  trunk                       = std::max(trunk, Nhwc4Bytes(batch, nh, nw, 3));
  // 256-byte alignment matches DeviceBuffer/WorkspacePool defaults.
  return AlignUp(trunk, 256);
}

void UploadPackedWeight(const float* oihw, int out_c, int in_c, int kh, int kw,
                        nn_ocl::DeviceBuffer& dst, cl_command_queue queue) {
  auto packed = nn_ocl::PackOhwi4o4iFromOihw(oihw, out_c, in_c, kh, kw);
  dst         = nn_ocl::DeviceBuffer::Floats(packed.size());
  dst.UploadFloats(packed, queue, /*blocking=*/true);
}

void UploadBias(const nn::SafetensorsTensor& host, nn_ocl::DeviceBuffer& dst,
                cl_command_queue queue) {
  dst = nn_ocl::DeviceBuffer::Floats(host.data.size());
  dst.UploadFloats(host.data, queue, /*blocking=*/true);
}

void Enqueue3D(cl_kernel kernel, size_t gx, size_t gy, size_t gz, cl_command_queue queue,
               const char* what) {
  if (kernel == nullptr) {
    throw std::runtime_error(std::string(what) + ": null kernel");
  }
  const size_t             global[3] = {gx, gy, gz};
  nn_ocl::ScopedStageEvent stage_event;
  nn_ocl::CheckOpenCl(clEnqueueNDRangeKernel(queue, kernel, 3, nullptr, global, nullptr, 0, nullptr,
                                             stage_event.out()),
                      what);
  ++nn_ocl::GetDispatchInstrumentation().enqueue_count;
  NoteOpenClEnqueueNdRange();
}

template <typename T>
void SetArg(cl_kernel kernel, cl_uint index, const T& value, const char* what) {
  nn_ocl::CheckOpenCl(clSetKernelArg(kernel, index, sizeof(T), &value), what);
}

// ---------------------------------------------------------------------------
// Shared module state for Bayer / X-Trans
// ---------------------------------------------------------------------------

struct ModuleState {
  bool                 loaded = false;

  // Weights (OHWI4o4i for convs; bias as length-logical vectors).
  nn_ocl::DeviceBuffer trunk_w_[8];  // depth max 8
  nn_ocl::DeviceBuffer trunk_b_[8];
  nn_ocl::DeviceBuffer residual_w_;
  nn_ocl::DeviceBuffer residual_b_;
  nn_ocl::DeviceBuffer post_w_;
  nn_ocl::DeviceBuffer post_b_;
  nn_ocl::DeviceBuffer output_w_;
  nn_ocl::DeviceBuffer output_b_;

  // Kernels created once at load. Every weighted layer has its own kernel
  // object because clSetKernelArg mutates kernel state.
  KernelHolder         pack;
  KernelHolder         pack_reflect_hwc;
  BoundConvKernel      trunk[8];
  BoundConvKernel      residual;
  BoundConvKernel      post;
  BoundConvKernel      output;
  KernelHolder         unpack_concat;
  KernelHolder         unpack_reflect_concat;
  KernelHolder         output_rgb;

  int                  depth       = 0;
  int                  width       = 0;
  int                  pack_out_ch = 0;
  int                  residual_ch = 0;
  int                  pack_factor = 2;
  bool                 is_bayer    = true;
};

void EnsureProgramsRegistered() {
  // Central registry path — modules never register programs themselves.
  RegisterOpenClBackendPrograms();
}

void CreateKernels(ModuleState& s, bool is_bayer) {
  EnsureProgramsRegistered();
  auto&       lib         = OpenClProgramLibrary::Instance();
  const char* conv_name   = is_bayer ? OpenCL::DemosaicNet::kConvBayerProgramName
                                     : OpenCL::DemosaicNet::kConvXTransProgramName;
  cl_program  conv_prog   = lib.GetProgram(conv_name);
  cl_program  struct_prog = lib.GetProgram(OpenCL::DemosaicNet::kStructuralProgramName);

  for (int i = 0; i < s.depth; ++i) {
    s.trunk[i].kernel = KernelHolder(conv_prog, OpenCL::DemosaicNet::kConv3x3KernelName);
  }
  s.residual.kernel = KernelHolder(conv_prog, OpenCL::DemosaicNet::kConv1x1KernelName);
  s.post.kernel     = KernelHolder(conv_prog, OpenCL::DemosaicNet::kConv3x3C6KernelName);
  s.output.kernel  = KernelHolder(conv_prog, OpenCL::DemosaicNet::kConv1x1Output3KernelName);
  s.pack = KernelHolder(struct_prog, is_bayer ? OpenCL::DemosaicNet::kPackBayerNchwKernelName
                                              : OpenCL::DemosaicNet::kPackXTransNchwKernelName);
  s.pack_reflect_hwc =
      KernelHolder(struct_prog, is_bayer ? OpenCL::DemosaicNet::kPackReflectBayerNhwc4KernelName
                                         : OpenCL::DemosaicNet::kPackReflectXTransNhwc4KernelName);
  s.unpack_concat = KernelHolder(struct_prog, OpenCL::DemosaicNet::kUnpackCropConcatKernelName);
  s.unpack_reflect_concat =
      KernelHolder(struct_prog, OpenCL::DemosaicNet::kUnpackReflectConcatKernelName);
  s.output_rgb = KernelHolder(struct_prog, OpenCL::DemosaicNet::kOutputRgbHwcKernelName);
}

void BindLayerArguments(ModuleState& s) {
  const int trunk_blocks = nn_ocl::ChannelBlocks(s.width);
  for (int i = 0; i < s.depth; ++i) {
    const int input_channels = i == 0 ? s.pack_out_ch : s.width;
    s.trunk[i].Bind3x3(s.trunk_w_[i].get(), s.trunk_b_[i].get(),
                       nn_ocl::ChannelBlocks(input_channels), trunk_blocks, input_channels,
                       s.width);
  }
  s.residual.Bind1x1(s.residual_w_.get(), s.residual_b_.get(), trunk_blocks,
                     nn_ocl::ChannelBlocks(s.residual_ch), s.width, s.residual_ch,
                     /*apply_relu=*/0);
  s.post.Bind3x3(s.post_w_.get(), s.post_b_.get(), /*in_blocks=*/2, trunk_blocks,
                 /*in_channels=*/6, s.width);
  s.output.Bind1x1(s.output_w_.get(), s.output_b_.get(), trunk_blocks, /*out_blocks=*/1, s.width,
                   /*out_channels=*/3, /*apply_relu=*/0);
}

void LoadCommonWeights(ModuleState& s, const nn::SafetensorsTensorMap& tensors, int depth,
                       int width, int pack_out_ch, int residual_ch, cl_command_queue queue,
                       const char* module, bool is_bayer) {
  s.depth       = depth;
  s.width       = width;
  s.pack_out_ch = pack_out_ch;
  s.residual_ch = residual_ch;
  s.is_bayer    = is_bayer;

  // Fixed pack / unpack one-hots (validated, not stored — pack/unpack are structural kernels).
  {
    const auto& pack = nn::RequireF32Tensor(tensors, "pack.weight", {pack_out_ch, 3, 2, 2});
    RequireExactHostWeight(pack, is_bayer ? ExpectedBayerPackWeight() : ExpectedXTransPackWeight(),
                           "pack.weight", module);
  }
  {
    const auto& unpack = nn::RequireF32Tensor(tensors, "unpack.weight", {residual_ch, 1, 2, 2});
    RequireExactHostWeight(unpack, ExpectedUnpackWeight(), "unpack.weight", module);
  }

  // Trunk layers 0..depth-1: all packed OHWI4o4i 3x3 + bias.
  for (int i = 0; i < depth; ++i) {
    const int         in_c   = (i == 0) ? pack_out_ch : width;
    const std::string wk     = "trunk." + std::to_string(i) + ".weight";
    const std::string bk     = "trunk." + std::to_string(i) + ".bias";
    const auto&       weight = nn::RequireF32Tensor(tensors, wk, {width, in_c, 3, 3});
    const auto&       bias   = nn::RequireF32Tensor(tensors, bk, {width});
    UploadPackedWeight(weight.data.data(), width, in_c, 3, 3, s.trunk_w_[i], queue);
    UploadBias(bias, s.trunk_b_[i], queue);
  }

  {
    const auto& weight =
        nn::RequireF32Tensor(tensors, "residual.weight", {residual_ch, width, 1, 1});
    UploadPackedWeight(weight.data.data(), residual_ch, width, 1, 1, s.residual_w_, queue);
  }
  {
    const auto& bias = nn::RequireF32Tensor(tensors, "residual.bias", {residual_ch});
    UploadBias(bias, s.residual_b_, queue);
  }
  {
    const auto& weight = nn::RequireF32Tensor(tensors, "post_conv.weight", {width, 6, 3, 3});
    UploadPackedWeight(weight.data.data(), width, 6, 3, 3, s.post_w_, queue);
  }
  {
    const auto& bias = nn::RequireF32Tensor(tensors, "post_conv.bias", {width});
    UploadBias(bias, s.post_b_, queue);
  }
  {
    const auto& weight = nn::RequireF32Tensor(tensors, "output.weight", {3, width, 1, 1});
    UploadPackedWeight(weight.data.data(), 3, width, 1, 1, s.output_w_, queue);
  }
  {
    const auto& bias = nn::RequireF32Tensor(tensors, "output.bias", {3});
    UploadBias(bias, s.output_b_, queue);
  }

  CreateKernels(s, is_bayer);
  BindLayerArguments(s);
  s.loaded = true;
}

[[nodiscard]] auto ResidentBytes(const ModuleState& s) -> std::size_t {
  std::size_t total = 0;
  auto        add   = [&](const nn_ocl::DeviceBuffer& b) { total += b.byte_capacity(); };
  for (int i = 0; i < s.depth; ++i) {
    add(s.trunk_w_[i]);
    add(s.trunk_b_[i]);
  }
  add(s.residual_w_);
  add(s.residual_b_);
  add(s.post_w_);
  add(s.post_b_);
  add(s.output_w_);
  add(s.output_b_);
  return total;
}

enum class ForwardInputMode { Nchw, ReflectHwc3 };

void ForwardImpl(const ModuleState& s, cl_mem input, int batch, int height, int width,
                 cl_mem output_rgb_hwc, nn_ocl::ActivationSlots& activation_slots,
                 cl_command_queue queue, bool apply_gamma_decode, int (*output_height_fn)(int, int),
                 int (*output_width_fn)(int, int), int min_spatial, const char* module,
                 ForwardInputMode input_mode = ForwardInputMode::Nchw, int frame_height = 0,
                 int frame_width = 0, int origin_y = 0, int origin_x = 0) {
  if (!s.loaded) {
    throw std::runtime_error(std::string(module) + ": weights not loaded");
  }
  if (input == nullptr || output_rgb_hwc == nullptr) {
    throw std::runtime_error(std::string(module) + ": null input/output buffer");
  }
  if (batch != 1 || (height % s.pack_factor) != 0 || (width % s.pack_factor) != 0 ||
      height < min_spatial || width < min_spatial) {
    throw std::runtime_error(std::string(module) + ": invalid input geometry");
  }

  queue                        = ResolveQueue(queue);

  const int         out_h      = output_height_fn(height, width);
  const int         out_w      = output_width_fn(width, height);
  const int         ph         = height / s.pack_factor;
  const int         pw         = width / s.pack_factor;

  const std::size_t slot_bytes = EstimateNhwc4ActivationSlotBytes(
      height, width, batch, s.pack_out_ch, s.width, s.residual_ch, s.depth, s.pack_factor);
  if (slot_bytes == 0) {
    throw std::runtime_error(std::string(module) + ": invalid activation geometry");
  }

  // Dedicated full buffers (no SubBuffer). Grow-only after warm-up.
  activation_slots.EnsureSlotBytes(slot_bytes);
  cl_mem slot_a = activation_slots.slot_a();
  cl_mem slot_b = activation_slots.slot_b();
  if (slot_a == nullptr || slot_b == nullptr) {
    throw std::runtime_error(std::string(module) +
                             ": activation slots empty after EnsureSlotBytes");
  }

  if (input_mode == ForwardInputMode::Nchw) {
    // ---- Pack NCHW → NHWC4 (reference/module test path) ----
    nn_ocl::BeginDemosaicNetStage("pack_nchw_to_nhwc4");
    {
      const cl_int b  = batch;
      const cl_int ih = height;
      const cl_int iw = width;
      const cl_int oh = ph;
      const cl_int ow = pw;
      SetArg(s.pack.get(), 0, input, "pack arg0");
      SetArg(s.pack.get(), 1, slot_a, "pack arg1");
      SetArg(s.pack.get(), 2, b, "pack arg2");
      SetArg(s.pack.get(), 3, ih, "pack arg3");
      SetArg(s.pack.get(), 4, iw, "pack arg4");
      SetArg(s.pack.get(), 5, oh, "pack arg5");
      SetArg(s.pack.get(), 6, ow, "pack arg6");
      Enqueue3D(s.pack.get(), static_cast<size_t>(pw), static_cast<size_t>(ph),
                static_cast<size_t>(batch), queue, "pack enqueue");
    }
    nn_ocl::FinishDemosaicNetStage("pack_nchw_to_nhwc4", queue);
  } else {
    if (frame_height <= 0 || frame_width <= 0) {
      throw std::runtime_error(std::string(module) + ": invalid reflected tile geometry");
    }
    // Product path: reflect-101 + signed gamma + variant-specific packing
    // writes slot_a directly. No intermediate NCHW tile is materialised.
    nn_ocl::BeginDemosaicNetStage("reflect_pack_nhwc4");
    const cl_int fh = frame_height;
    const cl_int fw = frame_width;
    const cl_int oy = origin_y;
    const cl_int ox = origin_x;
    const cl_int th = height;
    const cl_int tw = width;
    SetArg(s.pack_reflect_hwc.get(), 0, input, "reflect pack arg0");
    SetArg(s.pack_reflect_hwc.get(), 1, slot_a, "reflect pack arg1");
    SetArg(s.pack_reflect_hwc.get(), 2, fh, "reflect pack arg2");
    SetArg(s.pack_reflect_hwc.get(), 3, fw, "reflect pack arg3");
    SetArg(s.pack_reflect_hwc.get(), 4, oy, "reflect pack arg4");
    SetArg(s.pack_reflect_hwc.get(), 5, ox, "reflect pack arg5");
    SetArg(s.pack_reflect_hwc.get(), 6, th, "reflect pack arg6");
    SetArg(s.pack_reflect_hwc.get(), 7, tw, "reflect pack arg7");
    Enqueue3D(s.pack_reflect_hwc.get(), static_cast<size_t>(pw), static_cast<size_t>(ph), 1, queue,
              "reflect pack enqueue");
    nn_ocl::FinishDemosaicNetStage("reflect_pack_nhwc4", queue);
  }

  // ---- Trunk: depth × 3x3 bias+ReLU, ping-pong ----
  int    cur_h = ph;
  int    cur_w = pw;
  cl_mem cur   = slot_a;
  cl_mem next  = slot_b;
  int    cur_c = s.pack_out_ch;

  nn_ocl::BeginDemosaicNetStage("trunk_3x3");
  for (int i = 0; i < s.depth; ++i) {
    const int next_h   = cur_h - 2;
    const int next_w   = cur_w - 2;
    auto      in_view  = nn_ocl::Nhwc4TensorView::Contiguous(cur, batch, cur_h, cur_w, cur_c);
    auto      out_view = nn_ocl::Nhwc4TensorView::Contiguous(next, batch, next_h, next_w, s.width);
    nn_ocl::Conv3x3Dispatch d;
    d.kernel  = s.trunk[i].kernel.get();
    d.input   = &in_view;
    d.output  = &out_view;
    d.weights = s.trunk[i].weights;
    d.bias    = s.trunk[i].bias;
    d.pad_h   = 0;
    d.pad_w   = 0;
    d.invariant_args_bound = true;
    s.trunk[i].Prepare3x3(d);
    d.all_args_bound = true;
    nn_ocl::EnqueueConv3x3Nhwc4(d, queue);
    std::swap(cur, next);
    cur_h = next_h;
    cur_w = next_w;
    cur_c = s.width;
  }
  nn_ocl::FinishDemosaicNetStage("trunk_3x3", queue);

  // ---- Residual 1x1 (no ReLU) → residual buffer on next slot ----
  nn_ocl::BeginDemosaicNetStage("residual_unpack_concat");
  {
    auto in_view  = nn_ocl::Nhwc4TensorView::Contiguous(cur, batch, cur_h, cur_w, s.width);
    auto out_view = nn_ocl::Nhwc4TensorView::Contiguous(next, batch, cur_h, cur_w, s.residual_ch);
    nn_ocl::Conv1x1Dispatch d;
    d.kernel     = s.residual.kernel.get();
    d.input      = &in_view;
    d.output     = &out_view;
    d.weights    = s.residual.weights;
    d.bias       = s.residual.bias;
    d.apply_relu = 0;
    d.invariant_args_bound = true;
    s.residual.Prepare1x1(d);
    d.all_args_bound = true;
    nn_ocl::EnqueueConv1x1Nhwc4(d, queue);
  }
  // residual is in next; reuse cur for C6 concat.
  cl_mem    residual_buf    = next;
  cl_mem    cat_buf         = cur;

  const int residual_h      = cur_h;
  const int residual_w      = cur_w;
  const int up_h            = residual_h * s.pack_factor;
  const int up_w            = residual_w * s.pack_factor;
  const int residual_blocks = nn_ocl::ChannelBlocks(s.residual_ch);

  // ---- Unpack + crop mosaic + concat → C6 NHWC4 (2 blocks) ----
  {
    const cl_int rh     = residual_h;
    const cl_int rw     = residual_w;
    const cl_int rcb    = residual_blocks;
    cl_kernel    kernel = s.unpack_concat.get();
    if (input_mode == ForwardInputMode::Nchw) {
      const cl_int b  = batch;
      const cl_int ih = height;
      const cl_int iw = width;
      SetArg(kernel, 0, input, "unpack arg0");
      SetArg(kernel, 1, residual_buf, "unpack arg1");
      SetArg(kernel, 2, cat_buf, "unpack arg2");
      SetArg(kernel, 3, b, "unpack arg3");
      SetArg(kernel, 4, ih, "unpack arg4");
      SetArg(kernel, 5, iw, "unpack arg5");
      SetArg(kernel, 6, rh, "unpack arg6");
      SetArg(kernel, 7, rw, "unpack arg7");
      SetArg(kernel, 8, rcb, "unpack arg8");
    } else {
      const cl_int fh = frame_height;
      const cl_int fw = frame_width;
      const cl_int oy = origin_y;
      const cl_int ox = origin_x;
      const cl_int th = height;
      const cl_int tw = width;
      kernel          = s.unpack_reflect_concat.get();
      SetArg(kernel, 0, input, "reflect unpack arg0");
      SetArg(kernel, 1, residual_buf, "reflect unpack arg1");
      SetArg(kernel, 2, cat_buf, "reflect unpack arg2");
      SetArg(kernel, 3, fh, "reflect unpack arg3");
      SetArg(kernel, 4, fw, "reflect unpack arg4");
      SetArg(kernel, 5, oy, "reflect unpack arg5");
      SetArg(kernel, 6, ox, "reflect unpack arg6");
      SetArg(kernel, 7, th, "reflect unpack arg7");
      SetArg(kernel, 8, tw, "reflect unpack arg8");
      SetArg(kernel, 9, rh, "reflect unpack arg9");
      SetArg(kernel, 10, rw, "reflect unpack arg10");
      SetArg(kernel, 11, rcb, "reflect unpack arg11");
    }
    Enqueue3D(kernel, static_cast<size_t>(up_w), static_cast<size_t>(up_h),
              static_cast<size_t>(batch), queue, "unpack enqueue");
  }
  nn_ocl::FinishDemosaicNetStage("residual_unpack_concat", queue);

  // ---- Post 3x3 C6→width + ReLU (valid) ----
  nn_ocl::BeginDemosaicNetStage("post_3x3");
  const int post_h = up_h - 2;
  const int post_w = up_w - 2;
  {
    auto in_view = nn_ocl::Nhwc4TensorView::ContiguousBlocked(cat_buf, batch, up_h, up_w, 6, 2);
    auto out_view =
        nn_ocl::Nhwc4TensorView::Contiguous(residual_buf, batch, post_h, post_w, s.width);
    nn_ocl::Conv3x3Dispatch d;
    d.kernel  = s.post.kernel.get();
    d.input   = &in_view;
    d.output  = &out_view;
    d.weights = s.post.weights;
    d.bias    = s.post.bias;
    d.pad_h   = 0;
    d.pad_w   = 0;
    d.invariant_args_bound = true;
    s.post.Prepare3x3(d);
    d.all_args_bound = true;
    nn_ocl::EnqueueConv3x3Nhwc4(d, queue);
  }
  nn_ocl::FinishDemosaicNetStage("post_3x3", queue);

  // ---- Output 1x1 width→3 (no ReLU) into cat_buf ----
  nn_ocl::BeginDemosaicNetStage("output_tail");
  {
    auto in_view =
        nn_ocl::Nhwc4TensorView::Contiguous(residual_buf, batch, post_h, post_w, s.width);
    auto out_view = nn_ocl::Nhwc4TensorView::Contiguous(cat_buf, batch, post_h, post_w, 3);
    nn_ocl::Conv1x1Dispatch d;
    d.kernel     = s.output.kernel.get();
    d.input      = &in_view;
    d.output     = &out_view;
    d.weights    = s.output.weights;
    d.bias       = s.output.bias;
    d.apply_relu = 0;
    d.invariant_args_bound = true;
    s.output.Prepare1x1(d);
    d.all_args_bound = true;
    nn_ocl::EnqueueConv1x1Nhwc4(d, queue);
  }
  nn_ocl::FinishDemosaicNetStage("output_tail", queue);

  // ---- Center-crop + optional gamma → HWC RGB ----
  nn_ocl::BeginDemosaicNetStage("output_rgb");
  const int crop_top  = (post_h - out_h) / 2;
  const int crop_left = (post_w - out_w) / 2;
  if (crop_top < 0 || crop_left < 0 || (post_h - out_h) % 2 != 0 || (post_w - out_w) % 2 != 0) {
    throw std::runtime_error(std::string(module) + ": invalid centered export crop");
  }
  {
    const cl_int b      = batch;
    const cl_int ih     = post_h;
    const cl_int iw     = post_w;
    const cl_int oh     = out_h;
    const cl_int ow     = out_w;
    const cl_int ct     = crop_top;
    const cl_int cl     = crop_left;
    const cl_int icb    = 1;
    const cl_int gamma  = apply_gamma_decode ? 1 : 0;
    const cl_int clampv = 0;
    SetArg(s.output_rgb.get(), 0, cat_buf, "out arg0");
    SetArg(s.output_rgb.get(), 1, output_rgb_hwc, "out arg1");
    SetArg(s.output_rgb.get(), 2, b, "out arg2");
    SetArg(s.output_rgb.get(), 3, ih, "out arg3");
    SetArg(s.output_rgb.get(), 4, iw, "out arg4");
    SetArg(s.output_rgb.get(), 5, oh, "out arg5");
    SetArg(s.output_rgb.get(), 6, ow, "out arg6");
    SetArg(s.output_rgb.get(), 7, ct, "out arg7");
    SetArg(s.output_rgb.get(), 8, cl, "out arg8");
    SetArg(s.output_rgb.get(), 9, icb, "out arg9");
    SetArg(s.output_rgb.get(), 10, gamma, "out arg10");
    SetArg(s.output_rgb.get(), 11, clampv, "out arg11");
    Enqueue3D(s.output_rgb.get(), static_cast<size_t>(out_w), static_cast<size_t>(out_h),
              static_cast<size_t>(batch), queue, "output enqueue");
  }
  nn_ocl::FinishDemosaicNetStage("output_rgb", queue);
}

}  // namespace

// ===========================================================================
// Bayer
// ===========================================================================

struct OpenClBayerDemosaicNet::Impl {
  ModuleState state;
};

OpenClBayerDemosaicNet::OpenClBayerDemosaicNet() : impl_(std::make_unique<Impl>()) {}
OpenClBayerDemosaicNet::~OpenClBayerDemosaicNet()                                 = default;
OpenClBayerDemosaicNet::OpenClBayerDemosaicNet(OpenClBayerDemosaicNet&&) noexcept = default;
OpenClBayerDemosaicNet& OpenClBayerDemosaicNet::operator=(OpenClBayerDemosaicNet&&) noexcept =
    default;

void OpenClBayerDemosaicNet::LoadWeights(const nn::SafetensorsTensorMap& tensors,
                                         cl_command_queue                queue) {
  RequireMetadata(tensors, "format", "demosaicnet-pytorch-state_dict", "OpenClBayerDemosaicNet");
  RequireMetadata(tensors, "architecture", kArchitecture, "OpenClBayerDemosaicNet");
  RequireMetadata(tensors, "architecture_version", "1", "OpenClBayerDemosaicNet");
  RequireMetadata(tensors, "variant", "bayer", "OpenClBayerDemosaicNet");
  RequireMetadata(tensors, "cfa_period", "2", "OpenClBayerDemosaicNet");
  RequireMetadata(tensors, "pack_factor", "2", "OpenClBayerDemosaicNet");
  RequireMetadata(tensors, "tile_input", "1086", "OpenClBayerDemosaicNet");
  RequireMetadata(tensors, "tile_output", "1024", "OpenClBayerDemosaicNet");
  RequireMetadata(tensors, "tile_border", "31", "OpenClBayerDemosaicNet");
  RequireMetadata(tensors, "tile_pad", "32", "OpenClBayerDemosaicNet");
  RequireMetadata(tensors, "tile_step", "1024", "OpenClBayerDemosaicNet");
  RequireMetadata(tensors, "checkpoint_sha256",
                  "f00fb0e4f4a49e32344ffb0add583bee98c7d5dbfda6c593b5b066d08f9de69f",
                  "OpenClBayerDemosaicNet");

  ModuleState staging;
  LoadCommonWeights(staging, tensors, kDepth, kWidth, kPackOutCh, kResidualCh, ResolveQueue(queue),
                    "OpenClBayerDemosaicNet", /*is_bayer=*/true);
  impl_->state = std::move(staging);
}

auto OpenClBayerDemosaicNet::weights_loaded() const -> bool {
  return impl_ != nullptr && impl_->state.loaded;
}

void OpenClBayerDemosaicNet::ForwardNchwToHwc(cl_mem input_nchw, int batch, int height, int width,
                                              cl_mem                       output_rgb_hwc,
                                              opencl::nn::ActivationSlots& activation_slots,
                                              cl_command_queue             queue,
                                              bool apply_gamma_decode) const {
  ForwardImpl(
      impl_->state, input_nchw, batch, height, width, output_rgb_hwc, activation_slots, queue,
      apply_gamma_decode, [](int h, int w) { return Spec::OutputHeight(h, w); },
      [](int w, int h) { return Spec::OutputWidth(w, h); }, kMinSpatial, "OpenClBayerDemosaicNet");
}

void OpenClBayerDemosaicNet::ForwardReflectHwc3ToHwc(cl_mem input_frame_hwc3, int frame_height,
                                                     int frame_width, int origin_y, int origin_x,
                                                     int tile_height, int tile_width,
                                                     cl_mem                       output_rgb_hwc,
                                                     opencl::nn::ActivationSlots& activation_slots,
                                                     cl_command_queue             queue,
                                                     bool apply_gamma_decode) const {
  ForwardImpl(
      impl_->state, input_frame_hwc3, 1, tile_height, tile_width, output_rgb_hwc, activation_slots,
      queue, apply_gamma_decode, [](int h, int w) { return Spec::OutputHeight(h, w); },
      [](int w, int h) { return Spec::OutputWidth(w, h); }, kMinSpatial, "OpenClBayerDemosaicNet",
      ForwardInputMode::ReflectHwc3, frame_height, frame_width, origin_y, origin_x);
}

auto OpenClBayerDemosaicNet::EstimateActivationSlotBytes(int input_h, int input_w, int batch)
    -> std::size_t {
  return EstimateNhwc4ActivationSlotBytes(input_h, input_w, batch, kPackOutCh, kWidth, kResidualCh,
                                          kDepth, kPackFactor);
}

auto OpenClBayerDemosaicNet::ResidentWeightBytes() const -> std::size_t {
  return impl_ != nullptr ? ResidentBytes(impl_->state) : 0;
}

auto OpenClBayerDemosaicNet::Trunk0WeightBuffer() const -> cl_mem {
  return impl_ != nullptr ? impl_->state.trunk_w_[0].get() : nullptr;
}

auto OpenClBayerDemosaicNet::OutputWeightBuffer() const -> cl_mem {
  return impl_ != nullptr ? impl_->state.output_w_.get() : nullptr;
}

// ===========================================================================
// X-Trans
// ===========================================================================

struct OpenClXTransDemosaicNet::Impl {
  ModuleState state;
};

OpenClXTransDemosaicNet::OpenClXTransDemosaicNet() : impl_(std::make_unique<Impl>()) {}
OpenClXTransDemosaicNet::~OpenClXTransDemosaicNet()                                  = default;
OpenClXTransDemosaicNet::OpenClXTransDemosaicNet(OpenClXTransDemosaicNet&&) noexcept = default;
OpenClXTransDemosaicNet& OpenClXTransDemosaicNet::operator=(OpenClXTransDemosaicNet&&) noexcept =
    default;

void OpenClXTransDemosaicNet::LoadWeights(const nn::SafetensorsTensorMap& tensors,
                                          cl_command_queue                queue) {
  RequireMetadata(tensors, "format", "demosaicnet-pytorch-state_dict", "OpenClXTransDemosaicNet");
  RequireMetadata(tensors, "architecture", kArchitecture, "OpenClXTransDemosaicNet");
  RequireMetadata(tensors, "architecture_version", "1", "OpenClXTransDemosaicNet");
  RequireMetadata(tensors, "variant", "xtrans", "OpenClXTransDemosaicNet");
  RequireMetadata(tensors, "cfa_period", "6", "OpenClXTransDemosaicNet");
  RequireMetadata(tensors, "pack_factor", "2", "OpenClXTransDemosaicNet");
  RequireMetadata(tensors, "tile_input", "1048", "OpenClXTransDemosaicNet");
  RequireMetadata(tensors, "tile_output", "1024", "OpenClXTransDemosaicNet");
  RequireMetadata(tensors, "tile_border", "12", "OpenClXTransDemosaicNet");
  RequireMetadata(tensors, "tile_pad", "12", "OpenClXTransDemosaicNet");
  RequireMetadata(tensors, "tile_step", "1020", "OpenClXTransDemosaicNet");
  RequireMetadata(tensors, "checkpoint_sha256",
                  "f985ba64404a4ef9e4662d4f556d184de1e47127ab046f7140fa4b614f4c7546",
                  "OpenClXTransDemosaicNet");

  ModuleState staging;
  LoadCommonWeights(staging, tensors, kDepth, kWidth, kPackOutCh, kResidualCh, ResolveQueue(queue),
                    "OpenClXTransDemosaicNet", /*is_bayer=*/false);
  impl_->state = std::move(staging);
}

auto OpenClXTransDemosaicNet::weights_loaded() const -> bool {
  return impl_ != nullptr && impl_->state.loaded;
}

void OpenClXTransDemosaicNet::ForwardNchwToHwc(cl_mem input_nchw, int batch, int height, int width,
                                               cl_mem                       output_rgb_hwc,
                                               opencl::nn::ActivationSlots& activation_slots,
                                               cl_command_queue             queue,
                                               bool apply_gamma_decode) const {
  ForwardImpl(
      impl_->state, input_nchw, batch, height, width, output_rgb_hwc, activation_slots, queue,
      apply_gamma_decode, [](int h, int w) { return Spec::OutputHeight(h, w); },
      [](int w, int h) { return Spec::OutputWidth(w, h); }, kMinSpatial, "OpenClXTransDemosaicNet");
}

void OpenClXTransDemosaicNet::ForwardReflectHwc3ToHwc(cl_mem input_frame_hwc3, int frame_height,
                                                      int frame_width, int origin_y, int origin_x,
                                                      int tile_height, int tile_width,
                                                      cl_mem                       output_rgb_hwc,
                                                      opencl::nn::ActivationSlots& activation_slots,
                                                      cl_command_queue             queue,
                                                      bool apply_gamma_decode) const {
  ForwardImpl(
      impl_->state, input_frame_hwc3, 1, tile_height, tile_width, output_rgb_hwc, activation_slots,
      queue, apply_gamma_decode, [](int h, int w) { return Spec::OutputHeight(h, w); },
      [](int w, int h) { return Spec::OutputWidth(w, h); }, kMinSpatial, "OpenClXTransDemosaicNet",
      ForwardInputMode::ReflectHwc3, frame_height, frame_width, origin_y, origin_x);
}

auto OpenClXTransDemosaicNet::EstimateActivationSlotBytes(int input_h, int input_w, int batch)
    -> std::size_t {
  return EstimateNhwc4ActivationSlotBytes(input_h, input_w, batch, kPackOutCh, kWidth, kResidualCh,
                                          kDepth, kPackFactor);
}

auto OpenClXTransDemosaicNet::ResidentWeightBytes() const -> std::size_t {
  return impl_ != nullptr ? ResidentBytes(impl_->state) : 0;
}

auto OpenClXTransDemosaicNet::Trunk0WeightBuffer() const -> cl_mem {
  return impl_ != nullptr ? impl_->state.trunk_w_[0].get() : nullptr;
}

auto OpenClXTransDemosaicNet::OutputWeightBuffer() const -> cl_mem {
  return impl_ != nullptr ? impl_->state.output_w_.get() : nullptr;
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
