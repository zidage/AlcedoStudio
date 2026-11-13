//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "decoders/processor/operators/gpu/opencl_to_linear_ref.hpp"

#include <algorithm>
#include <stdexcept>

#include "decoders/processor/operators/gpu/opencl_encode.hpp"
#include "decoders/processor/raw_normalization.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"

namespace alcedo {
namespace OpenCL {
namespace {

// Host-side mirror of the OpenCL kernel structs.  All members are naturally
// 4-byte aligned so the layout matches the OpenCL C definitions on every
// target we care about.
struct WBParams {
  float black_level[4];
  float white_level[4];
  float wb_multipliers[4];
  int   apply_white_balance;
  int   black_tile_width;
  int   black_tile_height;
  float pattern_black[36];
};

struct PatternParams {
  int width;
  int height;
  int tile_width;
  int tile_height;
  int raw_fc[36];
};

auto GetWBCoeff(const libraw_rawdata_t& raw_data) -> const float* {
  return raw_data.color.cam_mul;
}

void CheckOpenCl(cl_int err, const char* operation) {
  if (err != CL_SUCCESS) {
    throw std::runtime_error(std::string("OpenCL ToLinearRef: ") + operation +
                             " failed with error " + std::to_string(err) + ".");
  }
}

}  // namespace

void ToLinearRef(opencl::OpenClImage& img, LibRaw& raw_processor, const RawCfaPattern& pattern) {
  const auto raw_curve = raw_norm::BuildLinearizationCurve(raw_processor.imgdata.rawdata);
  const auto wb        = GetWBCoeff(raw_processor.imgdata.rawdata);

  if (img.Type() != CV_16UC1) {
    throw std::runtime_error("OpenCL ToLinearRef: expected CV_16UC1 input.");
  }

  const int width  = img.Width();
  const int height = img.Height();

  // Allocate float output buffer.
  opencl::OpenClImage float_img;
  float_img.Create(width, height, CV_32FC1);

  // Populate WBParams.
  WBParams wb_params = {};
  for (int c = 0; c < 4; ++c) {
    wb_params.black_level[c]    = raw_curve.black_level[c];
    wb_params.white_level[c]    = raw_curve.white_level[c];
    wb_params.wb_multipliers[c] = wb[c];
  }
  wb_params.apply_white_balance =
      (raw_processor.imgdata.color.as_shot_wb_applied & LIBRAW_ASWB_APPLIED) == 0 ? 1 : 0;

  const int tile_width  = raw_processor.imgdata.rawdata.color.cblack[4];
  const int tile_height = raw_processor.imgdata.rawdata.color.cblack[5];
  const int entries     = tile_width * tile_height;
  wb_params.black_tile_width  = tile_width;
  wb_params.black_tile_height = tile_height;
  if (entries > 0 && entries <= 36) {
    for (int i = 0; i < entries; ++i) {
      wb_params.pattern_black[i] =
          static_cast<float>(raw_processor.imgdata.rawdata.color.cblack[6 + i]);
    }
  }

  // Populate PatternParams.
  PatternParams pattern_params = {};
  pattern_params.width         = width;
  pattern_params.height        = height;
  if (pattern.kind == RawCfaKind::XTrans6x6) {
    pattern_params.tile_width  = 6;
    pattern_params.tile_height = 6;
    for (int i = 0; i < 36; ++i) {
      pattern_params.raw_fc[i] = pattern.xtrans_pattern.raw_fc[i];
    }
  } else {
    pattern_params.tile_width  = 2;
    pattern_params.tile_height = 2;
    for (int i = 0; i < 4; ++i) {
      pattern_params.raw_fc[i] = pattern.bayer_pattern.raw_fc[i];
    }
  }

  // Retrieve compiled program & create kernel.
  auto& context = OpenClContext::Instance();
  if (!context.IsInitialized()) {
    context.Initialize();
  }

  cl_program program = OpenClProgramLibrary::Instance().GetProgram("raw_processor_core");
  cl_int     err     = CL_SUCCESS;
  cl_kernel  kernel  = clCreateKernel(program, "to_linear_ref_u16_to_f32", &err);
  CheckOpenCl(err, "clCreateKernel");

  cl_mem input_buffer  = img.Buffer();
  cl_mem output_buffer = float_img.Buffer();

  err = clSetKernelArg(kernel, 0, sizeof(cl_mem), &input_buffer);
  CheckOpenCl(err, "clSetKernelArg(0)");
  err = clSetKernelArg(kernel, 1, sizeof(cl_mem), &output_buffer);
  CheckOpenCl(err, "clSetKernelArg(1)");
  err = clSetKernelArg(kernel, 2, sizeof(WBParams), &wb_params);
  CheckOpenCl(err, "clSetKernelArg(2)");
  err = clSetKernelArg(kernel, 3, sizeof(PatternParams), &pattern_params);
  CheckOpenCl(err, "clSetKernelArg(3)");
  const cl_uint zero = 0;
  err = clSetKernelArg(kernel, 4, sizeof(cl_uint), &zero);
  CheckOpenCl(err, "clSetKernelArg(4)");
  err = clSetKernelArg(kernel, 5, sizeof(cl_uint), &zero);
  CheckOpenCl(err, "clSetKernelArg(5)");

  const size_t local_size[2]  = {16, 16};
  const size_t global_size[2] = {
      ((static_cast<size_t>(width) + local_size[0] - 1) / local_size[0]) * local_size[0],
      ((static_cast<size_t>(height) + local_size[1] - 1) / local_size[1]) * local_size[1]};

  err = clEnqueueNDRangeKernel(context.Queue(), kernel, 2, nullptr, global_size, local_size, 0,
                               nullptr, nullptr);
  CheckOpenCl(err, "clEnqueueNDRangeKernel");
  err = clFinish(context.Queue());
  CheckOpenCl(err, "clFinish");

  clReleaseKernel(kernel);

  img = std::move(float_img);
}

void LinearizeRgb(opencl::OpenClImage& img, const RawRgbLinearizationParams& params) {
  if (img.Type() != CV_32FC4) throw std::runtime_error("OpenCL RGB: expected F32 RGBA");
  opencl::OpenClEncodeQueue stream{.queue = OpenClContext::Instance().Queue()};
  EncodeLinearizeRgb(stream, {img.Buffer(), 0}, img.Width(), img.Height(), params);
  CheckOpenCl(clFinish(stream.queue), "RGB linearization");
}

}  // namespace OpenCL
}  // namespace alcedo

#endif
