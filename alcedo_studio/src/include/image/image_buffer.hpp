//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <opencv2/opencv.hpp>
#include <utility>
#include <vector>

#include "image/gpu_backend.hpp"

#ifdef HAVE_CUDA
#include <opencv2/core/cuda.hpp>
#endif

#ifdef HAVE_METAL
#include "metal_image.hpp"
#endif

#ifdef HAVE_OPENCL
#include "image/opencl_image.hpp"
#endif

namespace alcedo {
class GpuImageWrapper {
 public:
  GpuImageWrapper()                                              = default;
  ~GpuImageWrapper()                                             = default;
  GpuImageWrapper(const GpuImageWrapper&)                        = delete;
  auto operator=(const GpuImageWrapper&) -> GpuImageWrapper&     = delete;
  GpuImageWrapper(GpuImageWrapper&&) noexcept                    = default;
  auto operator=(GpuImageWrapper&&) noexcept -> GpuImageWrapper& = default;

#ifdef HAVE_CUDA
  explicit GpuImageWrapper(cv::cuda::GpuMat&& image);
  auto GetCUDAImage() -> cv::cuda::GpuMat&;
  auto GetCUDAImage() const -> const cv::cuda::GpuMat&;
#endif

#ifdef HAVE_OPENCL
  explicit GpuImageWrapper(opencl::OpenClImage&& image);
  auto GetOpenClImage() -> opencl::OpenClImage&;
  auto GetOpenClImage() const -> const opencl::OpenClImage&;
#endif

#ifdef HAVE_METAL
  explicit GpuImageWrapper(metal::MetalImage&& image);
  auto GetMetalImage() -> metal::MetalImage&;
  auto GetMetalImage() const -> const metal::MetalImage&;
#endif
  auto Backend() const -> GpuBackendKind;
  auto Empty() const -> bool;
  auto Width() const -> int;
  auto Height() const -> int;
  auto Type() const -> int;

  void Create(int width, int height, int type, GpuBackendKind backend = GpuBackendKind::None);
  void Upload(const cv::Mat& cpu_data, GpuBackendKind backend = GpuBackendKind::None);
  void Download(cv::Mat& cpu_data) const;
  void ShareFrom(const GpuImageWrapper& src);
  void CopyTo(GpuImageWrapper& dst) const;
  void ConvertTo(GpuImageWrapper& dst, int type, double alpha = 1.0, double beta = 0.0) const;
  void Release();

 private:
  GpuBackendKind backend_ = GpuBackendKind::None;
#ifdef HAVE_CUDA
  cv::cuda::GpuMat cuda_image_;
#endif
#ifdef HAVE_OPENCL
  opencl::OpenClImage opencl_image_;
#endif
#ifdef HAVE_METAL
  metal::MetalImage metal_image_;
#endif
};

class ImageBuffer {
 private:
  cv::Mat                               cpu_data_;
  GpuImageWrapper                       gpu_data_;

  std::unique_ptr<std::vector<uint8_t>> buffer_;

 public:
  bool cpu_data_valid_ = false;
  bool gpu_data_valid_ = false;

  bool buffer_valid_   = false;
  ImageBuffer()        = default;
  ~ImageBuffer();

  // Non-copyable
  ImageBuffer(const ImageBuffer&)            = delete;
  ImageBuffer& operator=(const ImageBuffer&) = delete;

  // Movable
  ImageBuffer(ImageBuffer&& other) noexcept;
  ImageBuffer& operator=(ImageBuffer&& other) noexcept;

  ImageBuffer(cv::Mat& data);
  ImageBuffer(cv::Mat&& data);
#ifdef HAVE_CUDA
  ImageBuffer(cv::cuda::GpuMat&& data);
#endif
#ifdef HAVE_OPENCL
  ImageBuffer(opencl::OpenClImage&& data);
#endif
#ifdef HAVE_METAL
  ImageBuffer(metal::MetalImage&& data);
#endif
  ImageBuffer(std::vector<uint8_t>&& buffer);

  void ReadFromVectorBuffer(std::vector<uint8_t>&& buffer);

  auto GetCPUData() -> cv::Mat&;
  auto GetBuffer() -> std::vector<uint8_t>&;

#ifdef HAVE_CUDA
  auto GetCUDAImage() -> cv::cuda::GpuMat&;
  auto GetCUDAImage() const -> const cv::cuda::GpuMat&;
#endif
#ifdef HAVE_OPENCL
  auto GetOpenClImage() -> opencl::OpenClImage&;
  auto GetOpenClImage() const -> const opencl::OpenClImage&;
#endif
#ifdef HAVE_METAL
  auto GetMetalImage() -> metal::MetalImage&;
  auto GetMetalImage() const -> const metal::MetalImage&;
#endif
  auto        GetGPUBackend() const -> GpuBackendKind;
  auto        GetGPUWidth() const -> int;
  auto        GetGPUHeight() const -> int;
  auto        GetGPUType() const -> int;

  void SyncToGPU();
  void        SyncToGPU(GpuBackendKind backend);
  void        SyncToCPU();
  void        ConvertGPUDataTo(int type, double alpha = 1.0, double beta = 0.0);
  void        ShareGPUDataFrom(const ImageBuffer& src);
  void        CopyGPUDataTo(ImageBuffer& dst) const;

  void InitGPUData(int width, int height, int type, GpuBackendKind backend = GpuBackendKind::None);
  void        SetGPUDataValid(bool valid);

  ImageBuffer Clone() const;

  void        ReleaseCPUData();
  void        ReleaseGPUData();
  void        ReleaseBuffer();
};
};  // namespace alcedo
