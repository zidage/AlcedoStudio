/***************************************************************************************************
 * Copyright (c) 2017 - 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the conditions in third_party/cutlass/LICENSE.txt are met.
 *
 * Alcedo's fixed-shape wrapper and tile selection are Copyright 2026 Yurun Zi.
 **************************************************************************************************/

#include <cstddef>
#include <stdexcept>
#include <string>

#include "cuda/nn/cutlass_conv2d.hpp"
#include "cutlass/conv/conv2d_problem_size.h"
#include "cutlass/conv/device/implicit_gemm_convolution.h"
#include "cutlass/conv/kernel/default_conv2d_fprop.h"
#include "cutlass/cutlass.h"
#include "cutlass/epilogue/thread/linear_combination_relu.h"

namespace alcedo::cuda::nn {
namespace {

void CheckCutlass(const cutlass::Status status, const char* operation) {
  if (status != cutlass::Status::kSuccess) {
    throw std::runtime_error(std::string(operation) + ": " + cutlassGetStatusString(status));
  }
}

template <int CtaM, int WarpM>
struct CutlassStudentConv {
  using Epilogue = cutlass::epilogue::thread::LinearCombinationRelu<
      float, 1, float, float, cutlass::epilogue::thread::ScaleType::NoBetaScaling>;
  using Kernel = typename cutlass::conv::kernel::DefaultConv2dFprop<
      float, cutlass::layout::TensorNHWC, float, cutlass::layout::TensorNHWC, float,
      cutlass::layout::TensorNHWC, float, cutlass::arch::OpClassSimt, cutlass::arch::Sm50,
      cutlass::gemm::GemmShape<CtaM, 32, 8>, cutlass::gemm::GemmShape<WarpM, 32, 8>,
      cutlass::gemm::GemmShape<1, 1, 1>, Epilogue,
      cutlass::gemm::threadblock::GemmIdentityThreadblockSwizzle<>, 2, cutlass::arch::OpMultiplyAdd,
      cutlass::conv::IteratorAlgorithm::kOptimized>::Kernel;
  using Operation = cutlass::conv::device::ImplicitGemmConvolution<Kernel>;
};

template <typename Operation>
void LaunchCutlass(const float* input, float* output, const float* weight, const float* bias,
                   const int batch, const int height, const int width, const int channels,
                   const cudaStream_t stream) {
  const int out_h = height - 2;
  const int out_w = width - 2;
  using Layout    = cutlass::layout::TensorNHWC;

  const cutlass::conv::Conv2dProblemSize problem(
      cutlass::Tensor4DCoord(batch, height, width, channels),
      cutlass::Tensor4DCoord(channels, 3, 3, channels), cutlass::Tensor4DCoord(0, 0, 0, 0),
      cutlass::MatrixCoord(1, 1), cutlass::MatrixCoord(1, 1),
      cutlass::Tensor4DCoord(batch, out_h, out_w, channels), cutlass::conv::Mode::kCrossCorrelation,
      1);

  typename Operation::Arguments arguments{
      problem,
      {const_cast<float*>(input),
       Layout::packed(cutlass::Tensor4DCoord(batch, height, width, channels))},
      {const_cast<float*>(weight),
       Layout::packed(cutlass::Tensor4DCoord(channels, 3, 3, channels))},
      {const_cast<float*>(bias), Layout::Stride(0)},
      {output, Layout::packed(cutlass::Tensor4DCoord(batch, out_h, out_w, channels))},
      {1.0F}};

  Operation operation;
  CheckCutlass(operation.can_implement(arguments), "CUTLASS Conv2d can_implement");
  CheckCutlass(operation.initialize(arguments, nullptr, stream), "CUTLASS Conv2d initialize");
  CheckCutlass(operation(stream), "CUTLASS Conv2d launch");
}

}  // namespace

void TransformConv2d3x3WeightsCutlassKrsc(const float* src_oihw, const int channels,
                                          float* dst_krsc) {
  if (src_oihw == nullptr || dst_krsc == nullptr || channels != 32) {
    throw std::runtime_error("TransformConv2d3x3WeightsCutlassKrsc: expected a C=32 trunk");
  }
  for (int co = 0; co < channels; ++co) {
    for (int ky = 0; ky < 3; ++ky) {
      for (int kx = 0; kx < 3; ++kx) {
        for (int ci = 0; ci < channels; ++ci) {
          dst_krsc[((static_cast<std::size_t>(co) * 3 + ky) * 3 + kx) * channels + ci] =
              src_oihw[((static_cast<std::size_t>(co) * channels + ci) * 3 + ky) * 3 + kx];
        }
      }
    }
  }
}

void Conv2d3x3NhwcCutlassBiasRelu(const float* input_nhwc, float* output_nhwc,
                                  const float* weight_krsc, const float* bias, const int batch,
                                  const int height, const int width, const int channels,
                                  const cudaStream_t stream) {
  if (input_nhwc == nullptr || output_nhwc == nullptr || weight_krsc == nullptr ||
      bias == nullptr || batch < 1 || height < 3 || width < 3 || channels != 32) {
    throw std::runtime_error("Conv2d3x3NhwcCutlassBiasRelu: expected valid C=32 NHWC tensors");
  }
  LaunchCutlass<typename CutlassStudentConv<128, 64>::Operation>(
      input_nhwc, output_nhwc, weight_krsc, bias, batch, height, width, channels, stream);
}

}  // namespace alcedo::cuda::nn
