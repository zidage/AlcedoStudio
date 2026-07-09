//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <vector>

#include <opencv2/core/cuda.hpp>

namespace alcedo::cuda::nn {

// Maximum supported rank for intermediate CNN activations (N,C,H,W and variants).
constexpr int kMaxTensorRank = 8;

// Non-owning device tensor view. Storage is always f32 for the first NN milestone.
// Layout is row-major (C-order): the last dimension has stride 1 when contiguous.
//
// Design notes for the demosaicnet-style CNN path:
// - Intermediate activations are typically NCHW contiguous buffers allocated by us.
// - Pipeline image I/O uses cv::cuda::GpuMat (HWC / planar rows with optional pitch).
// - FromGpuMat() builds a zero-copy view so ReLU/etc. can run without layout convert.
struct DeviceTensor {
  float*      data   = nullptr;
  int         rank   = 0;
  std::int64_t shape[kMaxTensorRank]   = {};
  std::int64_t strides[kMaxTensorRank] = {};  // element strides

  [[nodiscard]] auto empty() const -> bool { return data == nullptr || Numel() == 0; }

  [[nodiscard]] auto Numel() const -> std::int64_t {
    if (rank <= 0) {
      return 0;
    }
    std::int64_t n = 1;
    for (int i = 0; i < rank; ++i) {
      if (shape[i] < 0) {
        throw std::runtime_error("DeviceTensor: negative shape dimension");
      }
      n *= shape[i];
    }
    return n;
  }

  [[nodiscard]] auto IsContiguous() const -> bool {
    if (rank <= 0) {
      return true;
    }
    std::int64_t expected = 1;
    for (int i = rank - 1; i >= 0; --i) {
      if (shape[i] == 0) {
        return true;
      }
      if (shape[i] != 1 && strides[i] != expected) {
        return false;
      }
      expected *= shape[i];
    }
    return true;
  }

  [[nodiscard]] auto ByteSize() const -> std::size_t {
    return static_cast<std::size_t>(Numel()) * sizeof(float);
  }

  // Contiguous row-major tensor from an explicit shape list.
  static auto Contiguous(float* ptr, std::initializer_list<std::int64_t> dims) -> DeviceTensor {
    return Contiguous(ptr, dims.begin(), static_cast<int>(dims.size()));
  }

  static auto Contiguous(float* ptr, const std::int64_t* dims, int rank_in) -> DeviceTensor {
    if (rank_in < 0 || rank_in > kMaxTensorRank) {
      throw std::runtime_error("DeviceTensor: rank out of range");
    }
    DeviceTensor t;
    t.data = ptr;
    t.rank = rank_in;
    std::int64_t stride = 1;
    for (int i = rank_in - 1; i >= 0; --i) {
      t.shape[i]   = dims[i];
      t.strides[i] = stride;
      stride *= dims[i];
    }
    return t;
  }

  static auto Contiguous(float* ptr, const std::vector<std::int64_t>& dims) -> DeviceTensor {
    return Contiguous(ptr, dims.data(), static_cast<int>(dims.size()));
  }

  // Zero-copy view of a GpuMat as [H, W, C] (C = channels).
  // Strides respect GpuMat pitch so pitched allocations need no pack/transpose.
  static auto FromGpuMat(cv::cuda::GpuMat& mat) -> DeviceTensor {
    if (mat.empty()) {
      return {};
    }
    if (mat.depth() != CV_32F) {
      throw std::runtime_error("DeviceTensor::FromGpuMat: only CV_32F is supported");
    }
    const int channels = mat.channels();
    if (channels <= 0) {
      throw std::runtime_error("DeviceTensor::FromGpuMat: invalid channel count");
    }

    DeviceTensor t;
    t.data = mat.ptr<float>();
    t.rank = 3;
    t.shape[0] = mat.rows;
    t.shape[1] = mat.cols;
    t.shape[2] = channels;
    // step is in bytes between rows.
    t.strides[0] = static_cast<std::int64_t>(mat.step / sizeof(float));
    t.strides[1] = channels;
    t.strides[2] = 1;
    return t;
  }

  static auto FromGpuMat(const cv::cuda::GpuMat& mat) -> DeviceTensor {
    // Const cast is safe: view does not own memory; callers must not mutate via a const mat.
    return FromGpuMat(const_cast<cv::cuda::GpuMat&>(mat));
  }
};

}  // namespace alcedo::cuda::nn
