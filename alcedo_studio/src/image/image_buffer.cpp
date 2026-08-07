//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "image/image_buffer.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace alcedo {

namespace {
auto DefaultGpuBackend() -> GpuBackendKind {
#ifdef HAVE_CUDA
  return GpuBackendKind::CUDA;
#elif defined(HAVE_METAL)
  return GpuBackendKind::Metal;
#elif defined(HAVE_OPENCL)
  return GpuBackendKind::OpenCL;
#else
  return GpuBackendKind::None;
#endif
}

auto ResolveGpuBackend(GpuBackendKind backend) -> GpuBackendKind {
  return backend == GpuBackendKind::None ? DefaultGpuBackend() : backend;
}
}  // namespace

#ifdef HAVE_CUDA
GpuImageWrapper::GpuImageWrapper(cv::cuda::GpuMat&& image)
    : backend_(GpuBackendKind::CUDA), cuda_image_(std::move(image)) {}

auto GpuImageWrapper::GetCUDAImage() -> cv::cuda::GpuMat& {
  if (backend_ != GpuBackendKind::CUDA) {
    throw std::runtime_error("GpuImageWrapper: Active GPU backend is not CUDA.");
  }
  return cuda_image_;
}

auto GpuImageWrapper::GetCUDAImage() const -> const cv::cuda::GpuMat& {
  if (backend_ != GpuBackendKind::CUDA) {
    throw std::runtime_error("GpuImageWrapper: Active GPU backend is not CUDA.");
  }
  return cuda_image_;
}
#endif

#ifdef HAVE_OPENCL
GpuImageWrapper::GpuImageWrapper(opencl::OpenClImage&& image)
    : backend_(GpuBackendKind::OpenCL), opencl_image_(std::move(image)) {}

auto GpuImageWrapper::GetOpenClImage() -> opencl::OpenClImage& {
  if (backend_ != GpuBackendKind::OpenCL) {
    throw std::runtime_error("GpuImageWrapper: Active GPU backend is not OpenCL.");
  }
  return opencl_image_;
}

auto GpuImageWrapper::GetOpenClImage() const -> const opencl::OpenClImage& {
  if (backend_ != GpuBackendKind::OpenCL) {
    throw std::runtime_error("GpuImageWrapper: Active GPU backend is not OpenCL.");
  }
  return opencl_image_;
}
#endif

#ifdef HAVE_METAL
GpuImageWrapper::GpuImageWrapper(metal::MetalImage&& image)
    : backend_(GpuBackendKind::Metal), metal_image_(std::move(image)) {}

auto GpuImageWrapper::GetMetalImage() -> metal::MetalImage& {
  if (backend_ != GpuBackendKind::Metal) {
    throw std::runtime_error("GpuImageWrapper: Active GPU backend is not Metal.");
  }
  return metal_image_;
}

auto GpuImageWrapper::GetMetalImage() const -> const metal::MetalImage& {
  if (backend_ != GpuBackendKind::Metal) {
    throw std::runtime_error("GpuImageWrapper: Active GPU backend is not Metal.");
  }
  return metal_image_;
}
#endif

auto GpuImageWrapper::Backend() const -> GpuBackendKind { return backend_; }

auto GpuImageWrapper::Empty() const -> bool {
  switch (backend_) {
#ifdef HAVE_CUDA
    case GpuBackendKind::CUDA:
      return cuda_image_.empty();
#endif
#ifdef HAVE_OPENCL
    case GpuBackendKind::OpenCL:
      return opencl_image_.Empty();
#endif
#ifdef HAVE_METAL
    case GpuBackendKind::Metal:
      return metal_image_.Empty();
#endif
    case GpuBackendKind::None:
      return true;
  }
  return true;
}

auto GpuImageWrapper::Width() const -> int {
  switch (backend_) {
#ifdef HAVE_CUDA
    case GpuBackendKind::CUDA:
      return cuda_image_.cols;
#endif
#ifdef HAVE_OPENCL
    case GpuBackendKind::OpenCL:
      return opencl_image_.Width();
#endif
#ifdef HAVE_METAL
    case GpuBackendKind::Metal:
      return static_cast<int>(metal_image_.Width());
#endif
    case GpuBackendKind::None:
      return 0;
  }
  return 0;
}

auto GpuImageWrapper::Height() const -> int {
  switch (backend_) {
#ifdef HAVE_CUDA
    case GpuBackendKind::CUDA:
      return cuda_image_.rows;
#endif
#ifdef HAVE_OPENCL
    case GpuBackendKind::OpenCL:
      return opencl_image_.Height();
#endif
#ifdef HAVE_METAL
    case GpuBackendKind::Metal:
      return static_cast<int>(metal_image_.Height());
#endif
    case GpuBackendKind::None:
      return 0;
  }
  return 0;
}

auto GpuImageWrapper::Type() const -> int {
  switch (backend_) {
#ifdef HAVE_CUDA
    case GpuBackendKind::CUDA:
      return cuda_image_.type();
#endif
#ifdef HAVE_OPENCL
    case GpuBackendKind::OpenCL:
      return opencl_image_.Type();
#endif
#ifdef HAVE_METAL
    case GpuBackendKind::Metal:
      return metal::MetalImage::CVTypeFromPixelFormat(metal_image_.Format());
#endif
    case GpuBackendKind::None:
      return -1;
  }
  return -1;
}

void GpuImageWrapper::Create(int width, int height, int type, GpuBackendKind backend) {
  const auto resolved_backend = ResolveGpuBackend(backend);
  if (backend_ != resolved_backend) {
    Release();
    backend_ = resolved_backend;
  }
  switch (resolved_backend) {
#ifdef HAVE_CUDA
    case GpuBackendKind::CUDA:
      cuda_image_.create(height, width, type);
      return;
#endif
#ifdef HAVE_OPENCL
    case GpuBackendKind::OpenCL:
      opencl_image_.Create(width, height, type);
      return;
#endif
#ifdef HAVE_METAL
    case GpuBackendKind::Metal:
      metal_image_.Create(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                          metal::MetalImage::PixelFormatFromCVType(type));
      return;
#endif
    case GpuBackendKind::None:
      break;
  }
  throw std::runtime_error("GpuImageWrapper: Requested GPU backend is not compiled.");
}

void GpuImageWrapper::Upload(const cv::Mat& cpu_data, GpuBackendKind backend) {
  const auto resolved_backend = ResolveGpuBackend(backend);
  if (backend_ != resolved_backend) {
    Release();
    backend_ = resolved_backend;
  }
  switch (resolved_backend) {
#ifdef HAVE_CUDA
    case GpuBackendKind::CUDA:
      cuda_image_.upload(cpu_data);
      return;
#endif
#ifdef HAVE_OPENCL
    case GpuBackendKind::OpenCL:
      opencl_image_.Upload(cpu_data);
      return;
#endif
#ifdef HAVE_METAL
    case GpuBackendKind::Metal:
      metal_image_.Upload(cpu_data);
      return;
#endif
    case GpuBackendKind::None:
      break;
  }
  throw std::runtime_error("GpuImageWrapper: Requested GPU backend is not compiled.");
}

void GpuImageWrapper::Download(cv::Mat& cpu_data) const {
  switch (backend_) {
#ifdef HAVE_CUDA
    case GpuBackendKind::CUDA:
      cuda_image_.download(cpu_data);
      return;
#endif
#ifdef HAVE_OPENCL
    case GpuBackendKind::OpenCL:
      opencl_image_.Download(cpu_data);
      return;
#endif
#ifdef HAVE_METAL
    case GpuBackendKind::Metal:
      metal_image_.Download(cpu_data);
      return;
#endif
    case GpuBackendKind::None:
      break;
  }
  throw std::runtime_error("GpuImageWrapper: No active GPU backend.");
}

void GpuImageWrapper::ShareFrom(const GpuImageWrapper& src) {
  if (src.Empty()) {
    Release();
    return;
  }

  Release();
  backend_ = src.backend_;
  switch (src.backend_) {
#ifdef HAVE_CUDA
    case GpuBackendKind::CUDA:
      cuda_image_ = src.cuda_image_;
      return;
#endif
#ifdef HAVE_OPENCL
    case GpuBackendKind::OpenCL:
      opencl_image_.ShareFrom(src.opencl_image_);
      return;
#endif
#ifdef HAVE_METAL
    case GpuBackendKind::Metal:
      metal_image_ = src.metal_image_;
      return;
#endif
    case GpuBackendKind::None:
      return;
  }
  throw std::runtime_error("GpuImageWrapper: Source GPU backend is not compiled.");
}

void GpuImageWrapper::CopyTo(GpuImageWrapper& dst) const {
  if (Empty()) {
    throw std::runtime_error("GpuImageWrapper: Cannot copy empty GPU data.");
  }

  dst.Release();
  dst.backend_ = backend_;
  switch (backend_) {
#ifdef HAVE_CUDA
    case GpuBackendKind::CUDA:
      dst.cuda_image_.create(cuda_image_.rows, cuda_image_.cols, cuda_image_.type());
      cuda_image_.copyTo(dst.cuda_image_);
      return;
#endif
#ifdef HAVE_OPENCL
    case GpuBackendKind::OpenCL:
      opencl_image_.CopyTo(dst.opencl_image_);
      return;
#endif
#ifdef HAVE_METAL
    case GpuBackendKind::Metal:
      metal_image_.CopyTo(dst.metal_image_);
      return;
#endif
    case GpuBackendKind::None:
      break;
  }
  throw std::runtime_error("GpuImageWrapper: Active GPU backend is not compiled.");
}

void GpuImageWrapper::ConvertTo(GpuImageWrapper& dst, int type, double alpha, double beta) const {
  if (Empty()) {
    throw std::runtime_error("GpuImageWrapper: Cannot convert empty GPU data.");
  }

  dst.Release();
  dst.backend_ = backend_;
  switch (backend_) {
#ifdef HAVE_CUDA
    case GpuBackendKind::CUDA:
      cuda_image_.convertTo(dst.cuda_image_, type, alpha, beta);
      return;
#endif
#ifdef HAVE_OPENCL
    case GpuBackendKind::OpenCL:
      opencl_image_.ConvertTo(dst.opencl_image_, type, alpha, beta);
      return;
#endif
#ifdef HAVE_METAL
    case GpuBackendKind::Metal:
      metal_image_.ConvertTo(dst.metal_image_, metal::MetalImage::PixelFormatFromCVType(type),
                             alpha, beta);
      return;
#endif
    case GpuBackendKind::None:
      break;
  }
  throw std::runtime_error("GpuImageWrapper: Active GPU backend is not compiled.");
}

void GpuImageWrapper::Release() {
#ifdef HAVE_CUDA
  cuda_image_.release();
#endif
#ifdef HAVE_OPENCL
  opencl_image_.Release();
#endif
#ifdef HAVE_METAL
  metal_image_.Release();
#endif
  backend_ = GpuBackendKind::None;
}

ImageBuffer::~ImageBuffer() {
  cpu_data_.release();
  gpu_data_.Release();
  buffer_.reset();
  cpu_data_valid_ = false;
  gpu_data_valid_ = false;
  buffer_valid_   = false;
}

ImageBuffer::ImageBuffer(cv::Mat& data) : cpu_data_valid_(true) { data.copyTo(cpu_data_); }

ImageBuffer::ImageBuffer(cv::Mat&& data) : cpu_data_(std::move(data)), cpu_data_valid_(true) {}

ImageBuffer::ImageBuffer(std::vector<uint8_t>&& buffer) : buffer_valid_(true) {
  buffer_ = std::make_unique<std::vector<uint8_t>>(std::move(buffer));
}

ImageBuffer::ImageBuffer(ImageBuffer&& other) noexcept
    : cpu_data_(std::move(other.cpu_data_)),
      gpu_data_(std::move(other.gpu_data_)),
      buffer_(std::move(other.buffer_)),
      cpu_data_valid_(other.cpu_data_valid_),
      gpu_data_valid_(other.gpu_data_valid_),
      buffer_valid_(other.buffer_valid_) {
  other.cpu_data_valid_ = false;
  other.gpu_data_valid_ = false;
  other.buffer_valid_   = false;
}

#ifdef HAVE_CUDA
ImageBuffer::ImageBuffer(cv::cuda::GpuMat&& data)
    : gpu_data_(std::move(data)), gpu_data_valid_(true) {}
#endif

#ifdef HAVE_OPENCL
ImageBuffer::ImageBuffer(opencl::OpenClImage&& data)
    : gpu_data_(std::move(data)), gpu_data_valid_(true) {}
#endif

#ifdef HAVE_METAL
ImageBuffer::ImageBuffer(metal::MetalImage&& data)
    : gpu_data_(std::move(data)), gpu_data_valid_(true) {}
#endif

ImageBuffer& ImageBuffer::operator=(ImageBuffer&& other) noexcept {
  if (this != &other) {
    cpu_data_             = std::move(other.cpu_data_);
    gpu_data_             = std::move(other.gpu_data_);
    buffer_               = std::move(other.buffer_);
    cpu_data_valid_       = other.cpu_data_valid_;
    gpu_data_valid_       = other.gpu_data_valid_;
    buffer_valid_         = other.buffer_valid_;

    other.cpu_data_valid_ = false;
    other.gpu_data_valid_ = false;
    other.buffer_valid_   = false;
  }
  return *this;
}

void ImageBuffer::ReadFromVectorBuffer(std::vector<uint8_t>&& buffer) {
  buffer_       = std::make_unique<std::vector<uint8_t>>(std::move(buffer));
  buffer_valid_ = true;
}

auto ImageBuffer::GetCPUData() -> cv::Mat& {
  if (!cpu_data_valid_) {
    throw std::runtime_error("ImageBuffer: No valid CPU image data.");
  }
  return cpu_data_;
}

auto ImageBuffer::GetBuffer() -> std::vector<uint8_t>& {
  if (!buffer_valid_) {
    throw std::runtime_error("ImageBuffer: No valid encoded buffer data.");
  }
  return *buffer_;
}

#ifdef HAVE_CUDA
auto ImageBuffer::GetCUDAImage() -> cv::cuda::GpuMat& {
  if (!gpu_data_valid_) {
    throw std::runtime_error("ImageBuffer: No valid GPU image data.");
  }
  return gpu_data_.GetCUDAImage();
}

auto ImageBuffer::GetCUDAImage() const -> const cv::cuda::GpuMat& {
  if (!gpu_data_valid_) {
    throw std::runtime_error("ImageBuffer: No valid GPU image data.");
  }
  return gpu_data_.GetCUDAImage();
}
#endif

#ifdef HAVE_OPENCL
auto ImageBuffer::GetOpenClImage() -> opencl::OpenClImage& {
  if (!gpu_data_valid_) {
    throw std::runtime_error("ImageBuffer: No valid GPU image data.");
  }
  return gpu_data_.GetOpenClImage();
}

auto ImageBuffer::GetOpenClImage() const -> const opencl::OpenClImage& {
  if (!gpu_data_valid_) {
    throw std::runtime_error("ImageBuffer: No valid GPU image data.");
  }
  return gpu_data_.GetOpenClImage();
}
#endif

#ifdef HAVE_METAL
auto ImageBuffer::GetMetalImage() -> metal::MetalImage& {
  if (!gpu_data_valid_) {
    throw std::runtime_error("ImageBuffer: No valid GPU image data.");
  }
  return gpu_data_.GetMetalImage();
}

auto ImageBuffer::GetMetalImage() const -> const metal::MetalImage& {
  if (!gpu_data_valid_) {
    throw std::runtime_error("ImageBuffer: No valid GPU image data.");
  }
  return gpu_data_.GetMetalImage();
}
#endif

auto ImageBuffer::GetGPUBackend() const -> GpuBackendKind {
  if (!gpu_data_valid_) {
    return GpuBackendKind::None;
  }
  return gpu_data_.Backend();
}

auto ImageBuffer::GetGPUWidth() const -> int {
  if (!gpu_data_valid_) {
    throw std::runtime_error("ImageBuffer: No valid GPU image data.");
  }
  return gpu_data_.Width();
}

auto ImageBuffer::GetGPUHeight() const -> int {
  if (!gpu_data_valid_) {
    throw std::runtime_error("ImageBuffer: No valid GPU image data.");
  }
  return gpu_data_.Height();
}

auto ImageBuffer::GetGPUType() const -> int {
  if (!gpu_data_valid_) {
    throw std::runtime_error("ImageBuffer: No valid GPU image data.");
  }
  return gpu_data_.Type();
}

auto ImageBuffer::SyncToGPU() -> void { SyncToGPU(GpuBackendKind::None); }

auto ImageBuffer::SyncToGPU(GpuBackendKind backend) -> void {
  if (!cpu_data_valid_ || cpu_data_.empty()) {
    throw std::runtime_error("ImageBuffer: No valid CPU data to sync to GPU.");
  }
  gpu_data_.Upload(cpu_data_, backend);
  gpu_data_valid_ = true;
}

auto ImageBuffer::SyncToCPU() -> void {
  if (!gpu_data_valid_ || gpu_data_.Empty()) {
    throw std::runtime_error("ImageBuffer: No valid GPU data to sync to CPU.");
  }
  gpu_data_.Download(cpu_data_);
  cpu_data_valid_ = true;
}

void ImageBuffer::ConvertGPUDataTo(int type, double alpha, double beta) {
  if (!gpu_data_valid_ || gpu_data_.Empty()) {
    throw std::runtime_error("ImageBuffer: No valid GPU data to convert.");
  }

  GpuImageWrapper converted;
  gpu_data_.ConvertTo(converted, type, alpha, beta);
  gpu_data_       = std::move(converted);
  gpu_data_valid_ = true;
}

void ImageBuffer::ShareGPUDataFrom(const ImageBuffer& src) {
  if (!src.gpu_data_valid_ || src.gpu_data_.Empty()) {
    throw std::runtime_error("ImageBuffer: No valid GPU data to share.");
  }

  gpu_data_.ShareFrom(src.gpu_data_);
  gpu_data_valid_ = true;
}

void ImageBuffer::CopyGPUDataTo(ImageBuffer& dst) const {
  if (!gpu_data_valid_ || gpu_data_.Empty()) {
    throw std::runtime_error("ImageBuffer: No valid GPU data to copy.");
  }

  gpu_data_.CopyTo(dst.gpu_data_);
  dst.gpu_data_valid_ = true;
}

void ImageBuffer::InitGPUData(int width, int height, int type, GpuBackendKind backend) {
  const auto requested_backend = ResolveGpuBackend(backend);
  if ((gpu_data_valid_ || !gpu_data_.Empty()) && gpu_data_.Backend() == requested_backend) {
    return;
  }
  gpu_data_.Create(width, height, type, requested_backend);
  gpu_data_valid_ = true;
}

void        ImageBuffer::SetGPUDataValid(bool valid) { gpu_data_valid_ = valid; }

ImageBuffer ImageBuffer::Clone() const {
  if (cpu_data_valid_) {
    return ImageBuffer{cpu_data_.clone()};
  }
  if (gpu_data_valid_) {
    cv::Mat cpu_copy;
    gpu_data_.Download(cpu_copy);
    return ImageBuffer{std::move(cpu_copy)};
  }
  if (buffer_valid_) {
    auto buffer = *buffer_;
    return ImageBuffer{std::move(buffer)};
  }
  throw std::runtime_error("ImageBuffer: No valid data to clone.");
}

void ImageBuffer::ReleaseCPUData() {
  cpu_data_.release();
  cpu_data_valid_ = false;
}

void ImageBuffer::ReleaseGPUData() {
  gpu_data_.Release();
  gpu_data_valid_ = false;
}

void ImageBuffer::ReleaseBuffer() {
  buffer_.reset();
  buffer_valid_ = false;
}

}  // namespace alcedo
