//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/lease_target_adapters.hpp"

#include <QtGui/rhi/qrhi.h>
#include <QtGui/rhi/qrhi_platform.h>

#include <QDebug>
#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "ui/editor_rhi/native_resource_counters.hpp"

#if defined(_WIN32) && defined(HAVE_CUDA)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cuda_runtime_api.h>

#include "ui/edit_viewer/d3d_cuda_interop_utils.hpp"
#include "ui/editor_rhi/cuda_adapter_discovery.hpp"
#endif

#if defined(HAVE_OPENCL)
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <CL/cl.h>
#include <CL/cl_gl.h>

#include "opencl/opencl_context.hpp"
#endif

namespace alcedo::editor_rhi {

#if defined(_WIN32) && defined(HAVE_CUDA)
struct CudaD3D11LeaseAdapter::State {
  using ComPtr = Microsoft::WRL::ComPtr<ID3D11Texture2D>;

  struct Target {
    ComPtr texture;
    HANDLE shared_handle = nullptr;
    cudaExternalMemory_t external_memory = nullptr;
    cudaMipmappedArray_t mipmapped_array = nullptr;
    cudaArray_t image_array = nullptr;
    std::uintptr_t native_handle = 0;
    std::shared_ptr<LeaseLifetimeToken> lifetime_token;
  };

  ID3D11Device* device = nullptr;
  int cuda_device = -1;
  std::vector<std::unique_ptr<Target>> targets;
};
#else
struct CudaD3D11LeaseAdapter::State {};
#endif

#if defined(HAVE_OPENCL)
struct OpenClOpenGlLeaseAdapter::State {
  struct Target {
    GLuint texture = 0;
    cl_mem image = nullptr;
    std::uintptr_t native_handle = 0;
    bool acquired = false;
    std::shared_ptr<LeaseLifetimeToken> lifetime_token;
  };

  std::vector<std::unique_ptr<Target>> targets;
};
#else
struct OpenClOpenGlLeaseAdapter::State {};
#endif

CudaD3D11LeaseAdapter::CudaD3D11LeaseAdapter() : state_(std::make_unique<State>()) {}

CudaD3D11LeaseAdapter::~CudaD3D11LeaseAdapter() {
#if defined(_WIN32) && defined(HAVE_CUDA)
  if (!state_) {
    return;
  }
  while (!state_->targets.empty()) {
    WritableTargetLease lease;
    lease.native_handle = state_->targets.back()->native_handle;
    lease.lifetime_token = state_->targets.back()->lifetime_token;
    DestroyTarget(lease);
  }
#endif
}

auto CudaD3D11LeaseAdapter::CreateTarget(QRhi* rhi, const QSize& size,
                                         TargetGeneration generation, LeaseFrameLayer layer)
    -> std::optional<WritableTargetLease> {
#if defined(_WIN32) && defined(HAVE_CUDA)
  if (!rhi || rhi->backend() != QRhi::D3D11 || !size.isValid() || size.width() <= 0 ||
      size.height() <= 0) {
    last_error_ = "CUDA lease target requires a D3D11 QRhi and a positive size";
    return std::nullopt;
  }

  const auto* native = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
  auto* device = native ? static_cast<ID3D11Device*>(native->dev) : nullptr;
  if (!device) {
    last_error_ = "QRhi did not expose its D3D11 device";
    return std::nullopt;
  }
  int cuda_device = -1;
  const auto discovery = DiscoverCudaAdapters();
  for (const auto& candidate : discovery.devices) {
    if (D3D11DeviceMatchesCudaDevice(device, candidate.device_index)) {
      cuda_device = candidate.device_index;
      break;
    }
  }
  if (cuda_device < 0 ||
      !BindCudaDeviceOnCurrentThread(cuda_device, "CudaD3D11LeaseAdapter::CreateTarget")) {
    last_error_ = "D3D11 device does not have a compatible CUDA device";
    return std::nullopt;
  }

  auto target = std::make_unique<State::Target>();
  const auto cleanup_target = [&target]() {
    if (target->mipmapped_array) {
      cudaFreeMipmappedArray(target->mipmapped_array);
      target->mipmapped_array = nullptr;
    }
    target->image_array = nullptr;
    if (target->external_memory) {
      cudaDestroyExternalMemory(target->external_memory);
      target->external_memory = nullptr;
      NativeResourceCounters::Instance().OnDestroyExternalMemory();
    }
    if (target->shared_handle) {
      CloseHandle(target->shared_handle);
      target->shared_handle = nullptr;
    }
    if (target->texture) {
      target->texture.Reset();
      NativeResourceCounters::Instance().OnDestroySharedTexture();
    }
  };
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = static_cast<UINT>(size.width());
  desc.Height = static_cast<UINT>(size.height());
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, target->texture.GetAddressOf()))) {
    last_error_ = "CreateTexture2D failed for CUDA lease target";
    return std::nullopt;
  }
  NativeResourceCounters::Instance().OnCreateSharedTexture();

  Microsoft::WRL::ComPtr<IDXGIResource1> resource;
  if (FAILED(target->texture.As(&resource)) ||
      FAILED(resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &target->shared_handle)) ||
      !target->shared_handle) {
    last_error_ = "failed to create a shared D3D11 handle";
    cleanup_target();
    return std::nullopt;
  }

  const auto bytes = static_cast<unsigned long long>(size.width()) *
                     static_cast<unsigned long long>(size.height()) * sizeof(float) * 4ULL;
  const auto handle_desc = MakeDedicatedCudaExternalMemoryHandleDesc(
      target->shared_handle, cudaExternalMemoryHandleTypeD3D11Resource, bytes);
  if (cudaImportExternalMemory(&target->external_memory, &handle_desc) != cudaSuccess) {
    last_error_ = "cudaImportExternalMemory failed for lease target";
    cleanup_target();
    return std::nullopt;
  }
  NativeResourceCounters::Instance().OnCreateExternalMemory();

  cudaExternalMemoryMipmappedArrayDesc array_desc{};
  array_desc.formatDesc = cudaCreateChannelDesc(32, 32, 32, 32, cudaChannelFormatKindFloat);
  array_desc.extent =
      cudaExtent{static_cast<size_t>(size.width()), static_cast<size_t>(size.height()), 0};
  array_desc.flags = cudaArrayColorAttachment;
  array_desc.numLevels = 1;
  if (cudaExternalMemoryGetMappedMipmappedArray(&target->mipmapped_array, target->external_memory,
                                                 &array_desc) != cudaSuccess ||
      cudaGetMipmappedArrayLevel(&target->image_array, target->mipmapped_array, 0) !=
          cudaSuccess) {
    last_error_ = "failed to map CUDA array for lease target";
    cleanup_target();
    return std::nullopt;
  }

  target->native_handle = reinterpret_cast<std::uintptr_t>(target->texture.Get());
  target->lifetime_token = std::make_shared<LeaseLifetimeToken>();
  state_->device = device;
  state_->cuda_device = cuda_device;
  state_->targets.push_back(std::move(target));

  WritableTargetLease lease;
  lease.backend = EditorBackend::Cuda;
  lease.handle_kind = LeaseNativeHandleKind::D3D11Texture2D;
  lease.writable_kind = LeaseWritableResourceKind::CudaArray;
  lease.pixel_format = LeasePixelFormat::Rgba32f;
  lease.dimensions = {size.width(), size.height()};
  lease.generation = generation;
  lease.layer = layer;
  lease.native_handle = state_->targets.back()->native_handle;
  lease.writable_resource =
      reinterpret_cast<std::uintptr_t>(state_->targets.back()->image_array);
  // Shared NT handle is retained for diagnostics only; CUDA array is the write target.
  lease.sync_object = reinterpret_cast<std::uintptr_t>(state_->targets.back()->shared_handle);
  lease.lifetime_token = state_->targets.back()->lifetime_token;
  return lease;
#else
  (void)rhi;
  (void)size;
  (void)generation;
  (void)layer;
  last_error_ = "CUDA/D3D11 lease adapter is unavailable in this build";
  return std::nullopt;
#endif
}

void CudaD3D11LeaseAdapter::DestroyTarget(const WritableTargetLease& lease) {
#if defined(_WIN32) && defined(HAVE_CUDA)
  if (!state_ || lease.native_handle == 0) {
    return;
  }
  const auto it = std::find_if(
      state_->targets.begin(), state_->targets.end(), [&lease](const auto& target) {
        return target && target->native_handle == lease.native_handle;
      });
  if (it == state_->targets.end()) {
    return;
  }
  auto& target = *it;
  if (target->mipmapped_array) {
    cudaFreeMipmappedArray(target->mipmapped_array);
    target->mipmapped_array = nullptr;
  }
  target->image_array = nullptr;
  if (target->external_memory) {
    cudaDestroyExternalMemory(target->external_memory);
    target->external_memory = nullptr;
    NativeResourceCounters::Instance().OnDestroyExternalMemory();
  }
  if (target->shared_handle) {
    CloseHandle(target->shared_handle);
    target->shared_handle = nullptr;
  }
  target->texture.Reset();
  NativeResourceCounters::Instance().OnDestroySharedTexture();
  state_->targets.erase(it);
#else
  (void)lease;
#endif
}

auto CudaD3D11LeaseAdapter::WaitProducerWriteComplete(const WritableTargetLease& lease) -> bool {
#if defined(_WIN32) && defined(HAVE_CUDA)
  if (lease.writable_kind != LeaseWritableResourceKind::CudaArray ||
      lease.writable_resource == 0) {
    last_error_ = "CUDA wait requires a valid cudaArray writable resource";
    return false;
  }
  const cudaError_t err = cudaDeviceSynchronize();
  if (err != cudaSuccess) {
    last_error_ = std::string("cudaDeviceSynchronize failed: ") + cudaGetErrorString(err);
    return false;
  }
  return true;
#else
  (void)lease;
  last_error_ = "CUDA wait unavailable in this build";
  return false;
#endif
}

OpenClOpenGlLeaseAdapter::OpenClOpenGlLeaseAdapter() : state_(std::make_unique<State>()) {}

OpenClOpenGlLeaseAdapter::~OpenClOpenGlLeaseAdapter() {
#if defined(HAVE_OPENCL)
  if (state_) {
    while (!state_->targets.empty()) {
      WritableTargetLease lease;
      lease.native_handle = state_->targets.back()->native_handle;
      DestroyTarget(lease);
    }
  }
#endif
}

auto OpenClOpenGlLeaseAdapter::CreateTarget(QRhi* rhi, const QSize& size,
                                            TargetGeneration generation, LeaseFrameLayer layer)
    -> std::optional<WritableTargetLease> {
#if defined(HAVE_OPENCL)
  if (!rhi || rhi->backend() != QRhi::OpenGLES2 || !size.isValid() || size.width() <= 0 ||
      size.height() <= 0) {
    last_error_ = "OpenCL lease target requires an OpenGL QRhi and a positive size";
    return std::nullopt;
  }
  if (!OpenClContext::Instance().IsInitialized() ||
      !OpenClContext::Instance().GLSharingEnabled()) {
    last_error_ = "OpenCL GL sharing is not initialized";
    return std::nullopt;
  }
  auto* context = QOpenGLContext::currentContext();
  if (!context) {
    last_error_ = "OpenCL lease target requires the render-thread OpenGL context";
    return std::nullopt;
  }

  auto target = std::make_unique<State::Target>();
  const auto cleanup_target = [&target, context]() {
    if (target->image) {
      clReleaseMemObject(target->image);
      target->image = nullptr;
      NativeResourceCounters::Instance().OnDestroyOpenClImage();
    }
    if (target->texture != 0) {
      context->functions()->glDeleteTextures(1, &target->texture);
      target->texture = 0;
      NativeResourceCounters::Instance().OnDestroySharedTexture();
    }
  };
  auto* functions = context->functions();
  functions->initializeOpenGLFunctions();
  functions->glGenTextures(1, &target->texture);
  functions->glBindTexture(GL_TEXTURE_2D, target->texture);
  functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  functions->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, size.width(), size.height(), 0, GL_RGBA,
                          GL_FLOAT, nullptr);
  functions->glBindTexture(GL_TEXTURE_2D, 0);
  functions->glFlush();
  if (target->texture == 0) {
    last_error_ = "glGenTextures failed for OpenCL lease target";
    return std::nullopt;
  }
  NativeResourceCounters::Instance().OnCreateSharedTexture();

  cl_int error = CL_SUCCESS;
  target->image = clCreateFromGLTexture(OpenClContext::Instance().Context(), CL_MEM_WRITE_ONLY,
                                        GL_TEXTURE_2D, 0, target->texture, &error);
  if (error != CL_SUCCESS || !target->image) {
    last_error_ = "clCreateFromGLTexture failed for lease target";
    cleanup_target();
    return std::nullopt;
  }
  NativeResourceCounters::Instance().OnCreateOpenClImage();
  target->native_handle = static_cast<std::uintptr_t>(target->texture);
  target->lifetime_token = std::make_shared<LeaseLifetimeToken>();
  state_->targets.push_back(std::move(target));

  WritableTargetLease lease;
  lease.backend = EditorBackend::OpenCl;
  lease.handle_kind = LeaseNativeHandleKind::OpenGLTexture2D;
  lease.writable_kind = LeaseWritableResourceKind::OpenClImage;
  lease.pixel_format = LeasePixelFormat::Rgba32f;
  lease.dimensions = {size.width(), size.height()};
  lease.generation = generation;
  lease.layer = layer;
  lease.native_handle = state_->targets.back()->native_handle;
  lease.writable_resource =
      reinterpret_cast<std::uintptr_t>(state_->targets.back()->image);
  // sync_object remains 0; OpenCL/GL ordering uses acquire/release + clFinish.
  lease.sync_object = 0;
  lease.lifetime_token = state_->targets.back()->lifetime_token;
  return lease;
#else
  (void)rhi;
  (void)size;
  (void)generation;
  (void)layer;
  last_error_ = "OpenCL/OpenGL lease adapter is unavailable in this build";
  return std::nullopt;
#endif
}

void OpenClOpenGlLeaseAdapter::DestroyTarget(const WritableTargetLease& lease) {
#if defined(HAVE_OPENCL)
  if (!state_ || lease.native_handle == 0) {
    return;
  }
  const auto it = std::find_if(
      state_->targets.begin(), state_->targets.end(), [&lease](const auto& target) {
        return target && target->native_handle == lease.native_handle;
      });
  if (it == state_->targets.end()) {
    return;
  }
  auto& target = *it;
  if (target->acquired && target->image) {
    cl_mem image = target->image;
    (void)clEnqueueReleaseGLObjects(OpenClContext::Instance().ProductQueue(), 1, &image, 0, nullptr,
                                    nullptr);
    (void)clFinish(OpenClContext::Instance().ProductQueue());
    target->acquired = false;
  }
  if (target->image) {
    clReleaseMemObject(target->image);
    target->image = nullptr;
    NativeResourceCounters::Instance().OnDestroyOpenClImage();
  }
  if (target->texture != 0 && QOpenGLContext::currentContext()) {
    QOpenGLContext::currentContext()->functions()->glDeleteTextures(1, &target->texture);
  }
  if (target->texture != 0) {
    target->texture = 0;
    NativeResourceCounters::Instance().OnDestroySharedTexture();
  }
  state_->targets.erase(it);
#else
  (void)lease;
#endif
}

auto OpenClOpenGlLeaseAdapter::AcquireForProducerWrite(const WritableTargetLease& lease) -> bool {
#if defined(HAVE_OPENCL)
  if (lease.writable_kind != LeaseWritableResourceKind::OpenClImage ||
      lease.writable_resource == 0) {
    last_error_ = "OpenCL acquire requires a valid cl_mem writable resource";
    return false;
  }
  auto* image = reinterpret_cast<cl_mem>(lease.writable_resource);
  const cl_int error =
      clEnqueueAcquireGLObjects(OpenClContext::Instance().ProductQueue(), 1, &image, 0, nullptr,
                                nullptr);
  if (error != CL_SUCCESS) {
    last_error_ = "clEnqueueAcquireGLObjects failed: " + std::to_string(error);
    return false;
  }
  if (state_) {
    for (auto& target : state_->targets) {
      if (target && target->native_handle == lease.native_handle) {
        target->acquired = true;
        break;
      }
    }
  }
  return true;
#else
  (void)lease;
  return false;
#endif
}

auto OpenClOpenGlLeaseAdapter::ReleaseAfterProducerWrite(const WritableTargetLease& lease)
    -> bool {
#if defined(HAVE_OPENCL)
  if (lease.writable_kind != LeaseWritableResourceKind::OpenClImage ||
      lease.writable_resource == 0) {
    last_error_ = "OpenCL release requires a valid cl_mem writable resource";
    return false;
  }
  auto* image = reinterpret_cast<cl_mem>(lease.writable_resource);
  const cl_int error =
      clEnqueueReleaseGLObjects(OpenClContext::Instance().ProductQueue(), 1, &image, 0, nullptr,
                                nullptr);
  if (error != CL_SUCCESS) {
    last_error_ = "clEnqueueReleaseGLObjects failed: " + std::to_string(error);
    return false;
  }
  if (state_) {
    for (auto& target : state_->targets) {
      if (target && target->native_handle == lease.native_handle) {
        target->acquired = false;
        break;
      }
    }
  }
  return true;
#else
  (void)lease;
  return false;
#endif
}

auto OpenClOpenGlLeaseAdapter::WaitProducerWriteComplete(const WritableTargetLease& lease)
    -> bool {
#if defined(HAVE_OPENCL)
  (void)lease;
  const cl_int error = clFinish(OpenClContext::Instance().ProductQueue());
  if (error != CL_SUCCESS) {
    last_error_ = "clFinish failed after OpenCL producer write: " + std::to_string(error);
    return false;
  }
  if (QOpenGLContext::currentContext()) {
    QOpenGLContext::currentContext()->functions()->glFlush();
  }
  return true;
#else
  (void)lease;
  return false;
#endif
}

auto MakeLeaseTargetAdapter(EditorBackend backend) -> std::unique_ptr<ILeaseTargetAdapter> {
  switch (backend) {
    case EditorBackend::Cuda:
      return std::make_unique<CudaD3D11LeaseAdapter>();
    case EditorBackend::OpenCl:
      return std::make_unique<OpenClOpenGlLeaseAdapter>();
    case EditorBackend::Metal:
      return std::make_unique<UnsupportedLeaseTargetAdapter>(backend);
    case EditorBackend::Cpu:
      return std::make_unique<UnsupportedLeaseTargetAdapter>(backend);
  }
  return std::make_unique<UnsupportedLeaseTargetAdapter>(backend);
}

auto ProducerAcquireWritable(const WritableTargetLease& lease) -> bool {
  if (lease.writable_kind == LeaseWritableResourceKind::OpenClImage) {
#if defined(HAVE_OPENCL)
    if (lease.writable_resource == 0) {
      return false;
    }
    auto* image = reinterpret_cast<cl_mem>(lease.writable_resource);
    const cl_int error =
        clEnqueueAcquireGLObjects(OpenClContext::Instance().ProductQueue(), 1, &image, 0, nullptr,
                                  nullptr);
    if (error != CL_SUCCESS) {
      qWarning("ProducerAcquireWritable: clEnqueueAcquireGLObjects failed: %d "
               "(OpenCL/GL share group mismatch?)",
               static_cast<int>(error));
      return false;
    }
    return true;
#else
    return false;
#endif
  }
  return lease.writable_resource != 0;
}

auto ProducerReleaseWritable(const WritableTargetLease& lease) -> bool {
  if (lease.writable_kind == LeaseWritableResourceKind::OpenClImage) {
#if defined(HAVE_OPENCL)
    if (lease.writable_resource == 0) {
      return false;
    }
    auto* image = reinterpret_cast<cl_mem>(lease.writable_resource);
    return clEnqueueReleaseGLObjects(OpenClContext::Instance().ProductQueue(), 1, &image, 0, nullptr,
                                     nullptr) == CL_SUCCESS;
#else
    return false;
#endif
  }
  return true;
}

auto ProducerWaitWritableComplete(const WritableTargetLease& lease) -> bool {
  if (lease.writable_kind == LeaseWritableResourceKind::CudaArray) {
#if defined(_WIN32) && defined(HAVE_CUDA)
    return cudaDeviceSynchronize() == cudaSuccess;
#else
    return false;
#endif
  }
  if (lease.writable_kind == LeaseWritableResourceKind::OpenClImage) {
#if defined(HAVE_OPENCL)
    return clFinish(OpenClContext::Instance().ProductQueue()) == CL_SUCCESS;
#else
    return false;
#endif
  }
  return true;
}

}  // namespace alcedo::editor_rhi
