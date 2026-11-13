//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstddef>

#include <libraw/libraw.h>
#include <opencv2/core/cuda.hpp>

#include "decoders/processor/raw_processor_pattern.hpp"

namespace alcedo {
namespace CUDA {

struct HighlightCorrection {
  bool                 any_clipped = false;
  std::array<float, 4> clips       = {};
  std::array<float, 4> clipdark    = {};
  std::array<float, 4> chrominance = {};
};

struct HighlightAccumulation {
  bool                  any_clipped = false;
  std::array<double, 4> sums        = {};
  std::array<double, 4> cnts        = {};
};

class HighlightWorkspace {
 public:
  HighlightWorkspace();
  ~HighlightWorkspace();

  HighlightWorkspace(const HighlightWorkspace&)                    = delete;
  auto operator=(const HighlightWorkspace&) -> HighlightWorkspace& = delete;
  HighlightWorkspace(HighlightWorkspace&& other) noexcept;
  auto operator=(HighlightWorkspace&& other) noexcept -> HighlightWorkspace&;

  /**
   * @brief Use caller-owned device pointers. Subsequent @ref Reserve calls will not cudaMalloc.
   * @p result_rgb may be null when only stats buffers are required.
   */
  void BindExternal(int* anyclipped, float* sums, float* cnts, void* result_rgb, int width,
                    int height);

 private:
  friend auto BuildHighlightCorrection(LibRaw& raw_processor) -> HighlightCorrection;
  friend void Clamp01(cv::cuda::GpuMat& img, cv::cuda::Stream* stream);
  friend void AccumulateHighlightStats(const cv::cuda::GpuMat& img,
                                       const HighlightCorrection& correction,
                                       const cv::Rect& inner_region,
                                       HighlightWorkspace& workspace,
                                       HighlightAccumulation& accumulation,
                                       cv::cuda::Stream* stream);
  friend void AccumulateHighlightStats(const cv::cuda::GpuMat& red,
                                       const cv::cuda::GpuMat& green,
                                       const cv::cuda::GpuMat& blue,
                                       const HighlightCorrection& correction,
                                       const cv::Rect& inner_region,
                                       HighlightWorkspace& workspace,
                                       HighlightAccumulation& accumulation,
                                       cv::cuda::Stream* stream);
  friend void ApplyHighlightCorrection(cv::cuda::GpuMat& img,
                                       const HighlightCorrection& correction,
                                       HighlightWorkspace* workspace,
                                       cv::cuda::Stream* stream);
  friend void ApplyHighlightCorrectionAndPackRGBA(const cv::cuda::GpuMat& img,
                                                  cv::cuda::GpuMat& dst,
                                                  const HighlightCorrection& correction,
                                                  const float* cam_mul,
                                                  HighlightWorkspace* workspace,
                                                  cv::cuda::Stream* stream);
  friend void ApplyHighlightCorrectionAndPackRGBAOriented(const cv::cuda::GpuMat& img,
                                                          cv::cuda::GpuMat& dst,
                                                          const HighlightCorrection& correction,
                                                          const float* cam_mul, int flip,
                                                          HighlightWorkspace* workspace,
                                                          cv::cuda::Stream* stream);
  friend void ApplyHighlightCorrectionAndPackRGBA(const cv::cuda::GpuMat& red,
                                                  const cv::cuda::GpuMat& green,
                                                  const cv::cuda::GpuMat& blue,
                                                  cv::cuda::GpuMat& dst,
                                                  const HighlightCorrection& correction,
                                                  const float* cam_mul,
                                                  HighlightWorkspace* workspace,
                                                  cv::cuda::Stream* stream);
  friend void ApplyHighlightCorrectionAndPackRGBAOriented(const cv::cuda::GpuMat& red,
                                                          const cv::cuda::GpuMat& green,
                                                          const cv::cuda::GpuMat& blue,
                                                          cv::cuda::GpuMat& dst,
                                                          const HighlightCorrection& correction,
                                                          const float* cam_mul, int flip,
                                                          HighlightWorkspace* workspace,
                                                          cv::cuda::Stream* stream);
  void Reserve(int width, int height);
  void Release();

  int*             anyclipped_    = nullptr;
  float*           sums_          = nullptr;
  float*           cnts_          = nullptr;
  size_t           mask_capacity_ = 0;
  cv::cuda::GpuMat result_;
  bool             owns_device_   = true;
};

auto BuildHighlightCorrection(LibRaw& raw_processor) -> HighlightCorrection;
auto BuildHighlightCorrection(const float* cam_mul) -> HighlightCorrection;
void FinalizeHighlightCorrection(const HighlightAccumulation& accumulation,
                                 HighlightCorrection& correction);
void AccumulateHighlightStats(const cv::cuda::GpuMat& img, const HighlightCorrection& correction,
                              const cv::Rect& inner_region, HighlightWorkspace& workspace,
                              HighlightAccumulation& accumulation,
                              cv::cuda::Stream* stream = nullptr);
void AccumulateHighlightStats(const cv::cuda::GpuMat& red, const cv::cuda::GpuMat& green,
                              const cv::cuda::GpuMat& blue,
                              const HighlightCorrection& correction,
                              const cv::Rect& inner_region, HighlightWorkspace& workspace,
                              HighlightAccumulation& accumulation,
                              cv::cuda::Stream* stream = nullptr);
void ApplyHighlightCorrection(cv::cuda::GpuMat& img, const HighlightCorrection& correction,
                              HighlightWorkspace* workspace = nullptr,
                              cv::cuda::Stream* stream = nullptr);
void ApplyHighlightCorrectionAndPackRGBA(const cv::cuda::GpuMat& img, cv::cuda::GpuMat& dst,
                                         const HighlightCorrection& correction,
                                         const float* cam_mul,
                                         HighlightWorkspace* workspace = nullptr,
                                         cv::cuda::Stream* stream = nullptr);
void ApplyHighlightCorrectionAndPackRGBA(const cv::cuda::GpuMat& red,
                                         const cv::cuda::GpuMat& green,
                                         const cv::cuda::GpuMat& blue,
                                         cv::cuda::GpuMat& dst,
                                         const HighlightCorrection& correction,
                                         const float* cam_mul,
                                         HighlightWorkspace* workspace = nullptr,
                                         cv::cuda::Stream* stream = nullptr);
void ApplyHighlightCorrectionAndPackRGBAOriented(const cv::cuda::GpuMat& img, cv::cuda::GpuMat& dst,
                                                 const HighlightCorrection& correction,
                                                 const float* cam_mul, int flip,
                                                 HighlightWorkspace* workspace = nullptr,
                                                 cv::cuda::Stream* stream = nullptr);
void ApplyHighlightCorrectionAndPackRGBAOriented(const cv::cuda::GpuMat& red,
                                                 const cv::cuda::GpuMat& green,
                                                 const cv::cuda::GpuMat& blue,
                                                 cv::cuda::GpuMat& dst,
                                                 const HighlightCorrection& correction,
                                                 const float* cam_mul, int flip,
                                                 HighlightWorkspace* workspace = nullptr,
                                                 cv::cuda::Stream* stream = nullptr);
void HighlightReconstruct(cv::cuda::GpuMat& img, LibRaw& raw_processor,
                          HighlightWorkspace* workspace = nullptr,
                          cv::cuda::Stream* stream = nullptr);
void Clamp01(cv::cuda::GpuMat& img, cv::cuda::Stream* stream = nullptr);
};  // namespace CUDA
};  // namespace alcedo
