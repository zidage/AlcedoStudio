//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/rhi_edit_viewer_surface.hpp"

#include "ui/edit_viewer/color_manager.hpp"
#ifdef HAVE_CUDA
#include "ui/edit_viewer/d3d_cuda_interop_utils.hpp"
#endif
#ifdef HAVE_OPENCL
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"
#endif

#include <QtGui/rhi/qrhi.h>
#include <QtGui/rhi/qrhi_platform.h>

#include <QDebug>
#include <QFile>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSettings>
#include <QStringList>
#include <QWidget>
#include <QWindow>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>

#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#ifdef HAVE_CUDA
#include <cuda_runtime_api.h>
#endif
#ifdef HAVE_OPENCL
#include <CL/cl_d3d11.h>
#include <CL/cl_gl.h>
#endif
#endif

namespace alcedo {
namespace {

constexpr const char* kVertexShaderResource   = ":/shaders/edit_viewer/rhi_image.vert.qsb";
constexpr const char* kFragmentShaderResource = ":/shaders/edit_viewer/rhi_image.frag.qsb";

#if defined(Q_OS_WIN)
constexpr auto kAcceleratorBackendKey = "gpu/acceleratorBackend";

auto           ShouldUseOpenClRhiBackend() -> bool {
  return QSettings{}
             .value(QLatin1String(kAcceleratorBackendKey))
             .toString()
             .compare(QStringLiteral("opencl"), Qt::CaseInsensitive) == 0;
}
#endif

#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
using Microsoft::WRL::ComPtr;

constexpr size_t kRgba32fPixelBytes      = sizeof(float) * 4U;
constexpr size_t kDirectPresentSlotCount = 3U;

auto             IsValidSlotIndex(int slot_index, size_t slot_count) -> bool {
  return slot_index >= 0 && slot_index < static_cast<int>(slot_count);
}

auto DescribeDxgiAdapter(IDXGIAdapter* adapter) -> QString {
  if (!adapter) {
    return QStringLiteral("<null>");
  }

  DXGI_ADAPTER_DESC desc{};
  if (FAILED(adapter->GetDesc(&desc))) {
    return QStringLiteral("<unknown>");
  }
  return QString::fromWCharArray(desc.Description);
}

auto GetDxgiAdapterFromDevice(ID3D11Device* device) -> ComPtr<IDXGIAdapter> {
  if (!device) {
    return {};
  }

  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf()))) || !dxgi_device) {
    return {};
  }

  ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf()))) {
    return {};
  }
  return adapter;
}

#ifdef HAVE_CUDA
auto GetCudaDeviceLuid(int cuda_device) -> std::optional<LUID> {
  if (cuda_device < 0) {
    return std::nullopt;
  }

  cudaDeviceProp    prop{};
  const cudaError_t prop_err = cudaGetDeviceProperties(&prop, cuda_device);
  if (prop_err != cudaSuccess) {
    qWarning("RhiEditViewerSurface: cudaGetDeviceProperties failed for device %d: %s", cuda_device,
             cudaGetErrorString(prop_err));
    return std::nullopt;
  }

  LUID luid{};
  static_assert(sizeof(luid) == sizeof(prop.luid));
  std::memcpy(&luid, prop.luid, sizeof(luid));
  return luid;
}

auto LuidMatches(const LUID& lhs, const LUID& rhs) -> bool {
  return lhs.LowPart == rhs.LowPart && lhs.HighPart == rhs.HighPart;
}

auto DescribeLuid(const LUID& luid) -> QString {
  return QStringLiteral("%1:%2")
      .arg(static_cast<quint32>(luid.HighPart), 8, 16, QLatin1Char('0'))
      .arg(luid.LowPart, 8, 16, QLatin1Char('0'));
}

auto BindCudaDeviceOnCurrentThread(int cuda_device, const char* context) -> bool {
  if (cuda_device < 0) {
    return false;
  }

  const cudaError_t set_err = cudaSetDevice(cuda_device);
  if (set_err != cudaSuccess) {
    qWarning("RhiEditViewerSurface: cudaSetDevice(%d) failed in %s: %s", cuda_device, context,
             cudaGetErrorString(set_err));
    return false;
  }

  const cudaError_t init_err = cudaFree(nullptr);
  if (init_err != cudaSuccess) {
    qWarning("RhiEditViewerSurface: cudaFree(0) failed in %s for device %d: %s", context,
             cuda_device, cudaGetErrorString(init_err));
    return false;
  }

  const cudaError_t stale_err = cudaGetLastError();
  if (stale_err != cudaSuccess) {
    qWarning("RhiEditViewerSurface: cleared stale CUDA error before %s on device %d: %s", context,
             cuda_device, cudaGetErrorString(stale_err));
  }
  return true;
}

auto ResolveCudaDeviceForD3D11Device(ID3D11Device* device) -> int {
  if (!device) {
    return -1;
  }

  int               current_cuda_device = -1;
  const cudaError_t current_device_err  = cudaGetDevice(&current_cuda_device);
  if (current_device_err != cudaSuccess || current_cuda_device < 0) {
    qWarning("RhiEditViewerSurface: cudaGetDevice failed while validating D3D11 interop: %s",
             cudaGetErrorString(current_device_err));
    return -1;
  }

  const auto cuda_luid = GetCudaDeviceLuid(current_cuda_device);
  const auto adapter   = GetDxgiAdapterFromDevice(device);
  if (!cuda_luid || !adapter) {
    return -1;
  }

  DXGI_ADAPTER_DESC desc{};
  if (FAILED(adapter->GetDesc(&desc))) {
    return -1;
  }

  if (LuidMatches(desc.AdapterLuid, *cuda_luid)) {
    return current_cuda_device;
  }

  qWarning(
      "RhiEditViewerSurface: D3D11 adapter '%s' LUID %s does not match CUDA device %d "
      "LUID %s.",
      qPrintable(DescribeDxgiAdapter(adapter.Get())), qPrintable(DescribeLuid(desc.AdapterLuid)),
      current_cuda_device, qPrintable(DescribeLuid(*cuda_luid)));
  return -1;
}

auto ResolveCudaDeviceForD3D12Device(ID3D12Device* device) -> int {
  if (!device) {
    return -1;
  }

  int               current_cuda_device = -1;
  const cudaError_t current_device_err  = cudaGetDevice(&current_cuda_device);
  if (current_device_err != cudaSuccess || current_cuda_device < 0) {
    qWarning("RhiEditViewerSurface: cudaGetDevice failed while validating D3D12 interop: %s",
             cudaGetErrorString(current_device_err));
    return -1;
  }

  const auto cuda_luid = GetCudaDeviceLuid(current_cuda_device);
  if (!cuda_luid) {
    return -1;
  }

  const LUID device_luid = device->GetAdapterLuid();
  if (LuidMatches(device_luid, *cuda_luid)) {
    return current_cuda_device;
  }

  qWarning("RhiEditViewerSurface: D3D12 adapter LUID %s does not match CUDA device %d LUID %s.",
           qPrintable(DescribeLuid(device_luid)), current_cuda_device,
           qPrintable(DescribeLuid(*cuda_luid)));
  return -1;
}
#endif
#endif

constexpr float kViewportRoiMatchEpsilon    = 1.0e-4f;
constexpr float kDetailPatchAspectTolerance = 2.0e-2f;

auto BuildNormalizedViewportRoi(const std::optional<ViewportRenderRegion>& viewport_region)
    -> std::optional<FrameRoiRect> {
  if (!viewport_region.has_value() || viewport_region->reference_width_ <= 0 ||
      viewport_region->reference_height_ <= 0) {
    return std::nullopt;
  }

  const float reference_width = static_cast<float>(std::max(1, viewport_region->reference_width_));
  const float reference_height =
      static_cast<float>(std::max(1, viewport_region->reference_height_));
  return FrameRoiRect{
      std::clamp(static_cast<float>(std::max(0, viewport_region->x_)) / reference_width, 0.0f,
                 1.0f),
      std::clamp(static_cast<float>(std::max(0, viewport_region->y_)) / reference_height, 0.0f,
                 1.0f),
      std::clamp(viewport_region->scale_x_, 1.0e-4f, 1.0f),
      std::clamp(viewport_region->scale_y_, 1.0e-4f, 1.0f),
  };
}

auto RoiRectsMatch(const FrameRoiRect& lhs, const FrameRoiRect& rhs) -> bool {
  return std::abs(lhs.x - rhs.x) <= kViewportRoiMatchEpsilon &&
         std::abs(lhs.y - rhs.y) <= kViewportRoiMatchEpsilon &&
         std::abs(lhs.width - rhs.width) <= kViewportRoiMatchEpsilon &&
         std::abs(lhs.height - rhs.height) <= kViewportRoiMatchEpsilon;
}

}  // namespace

struct RhiImageRenderer::UniformData {
  float scale_zoom[4]   = {1.0f, 1.0f, 1.0f, 0.0f};
  float pan_mode[4]     = {0.0f, 0.0f, 0.0f, 0.0f};
  float detail_roi[4]   = {0.0f, 0.0f, 1.0f, 1.0f};
  float detail_flags[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct RhiImageRenderer::VertexData {
  float position[2];
  float uv[2];
};

struct RhiEditViewerSurface::PlatformState {
#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
  enum class DirectPresentBackend {
    None,
    OpenGL,
    D3D11,
    D3D12,
  };

  struct DirectPresentSlot {
    GLuint                  gl_texture = 0;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D12Resource>  resource;
    HANDLE                  shared_handle = nullptr;
#ifdef HAVE_CUDA
    cudaExternalMemory_t external_memory = nullptr;
    cudaMipmappedArray_t mipmapped_array = nullptr;
    cudaArray_t          image_array     = nullptr;
#endif
#ifdef HAVE_OPENCL
    cl_mem opencl_image    = nullptr;
    bool   opencl_acquired = false;
#endif
    int                   width                     = 0;
    int                   height                    = 0;
    std::uintptr_t        texture_handle            = 0;
    D3D12_RESOURCE_STATES d3d12_state               = D3D12_RESOURCE_STATE_COMMON;
    UINT64                pending_cuda_signal_value = 0;
    UINT64                ready_cuda_signal_value   = 0;
    UINT64                active_cuda_signal_value  = 0;
    FramePresentationMode presentation_mode         = FramePresentationMode::FullFrame;
    FramePreviewMetadata  preview_metadata{};
  };

  std::array<DirectPresentSlot, kDirectPresentSlotCount> targets{};
  ID3D11Device*                                          device       = nullptr;
  ID3D12Device*                                          d3d12_device = nullptr;
  ID3D12CommandQueue*                                    d3d12_queue  = nullptr;
  ComPtr<ID3D12CommandAllocator>                         d3d12_transition_allocator;
  ComPtr<ID3D12GraphicsCommandList>                      d3d12_transition_list;
  ComPtr<ID3D12Fence>                                    d3d12_transition_fence;
  std::unique_ptr<QOpenGLContext>                        gl_context;
  std::unique_ptr<QOffscreenSurface>                     gl_surface;
#ifdef HAVE_CUDA
  ComPtr<ID3D12Fence> d3d12_cuda_fence;
#endif
  HANDLE d3d12_transition_event = nullptr;
#ifdef HAVE_CUDA
  HANDLE                  d3d12_cuda_fence_shared_handle = nullptr;
  cudaExternalSemaphore_t cuda_signal_semaphore          = nullptr;
#endif
  UINT64 d3d12_transition_fence_value = 0;
#ifdef HAVE_CUDA
  UINT64 d3d12_cuda_fence_value = 0;
#endif
  DirectPresentBackend backend = DirectPresentBackend::None;
#ifdef HAVE_CUDA
  int cuda_device = -1;
#endif
#ifdef HAVE_OPENCL
  bool supports_opencl_d3d11 = false;
#endif
  mutable std::mutex                 mutex{};
  DirectPresentFrameQueue            pending_frames{};
  std::atomic<FramePresentationMode> active_presentation_mode{FramePresentationMode::FullFrame};
  std::atomic<FramePresentationMode> pending_presentation_mode{FramePresentationMode::FullFrame};
  std::atomic<bool>                  pending_presentation_mode_valid{false};
  FramePreviewMetadata               active_preview_metadata{};
  FramePreviewMetadata               pending_preview_metadata{};
  bool                               pending_preview_metadata_valid = false;
  int                                active_idx                     = 0;
  int                                write_idx                      = 1;
  int                                render_target_idx              = 0;
  int                                mapped_slot_idx                = -1;
  int                                ready_slot_idx                 = -1;
  bool                               supports_direct_present        = false;
#else
  bool supports_direct_present = false;
#endif
};

#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
auto EnsureSharedOpenGlContext(RhiEditViewerSurface::PlatformState& state) -> bool {
  if (state.gl_context && state.gl_surface && state.gl_surface->isValid()) {
    return true;
  }

  state.gl_context = std::make_unique<QOpenGLContext>();
  if (auto* global_share_context = QOpenGLContext::globalShareContext()) {
    state.gl_context->setShareContext(global_share_context);
    state.gl_context->setFormat(global_share_context->format());
  }
  if (!state.gl_context->create()) {
    qWarning("RhiEditViewerSurface: failed to create shared OpenGL interop context.");
    state.gl_context.reset();
    return false;
  }

  state.gl_surface = std::make_unique<QOffscreenSurface>();
  state.gl_surface->setFormat(state.gl_context->format());
  state.gl_surface->create();
  if (!state.gl_surface->isValid()) {
    qWarning("RhiEditViewerSurface: failed to create OpenGL interop offscreen surface.");
    state.gl_surface.reset();
    state.gl_context.reset();
    return false;
  }
  return true;
}

void ReleaseDirectPresentSlot(RhiEditViewerSurface::PlatformState&                    state,
                              RhiEditViewerSurface::PlatformState::DirectPresentSlot& slot) {
#ifdef HAVE_OPENCL
  if (slot.opencl_image) {
    clReleaseMemObject(slot.opencl_image);
    slot.opencl_image = nullptr;
  }
  slot.opencl_acquired = false;
#endif
  if (slot.gl_texture != 0) {
    if (state.gl_context && state.gl_surface &&
        state.gl_context->makeCurrent(state.gl_surface.get())) {
      QOpenGLFunctions* functions = state.gl_context->functions();
      functions->glDeleteTextures(1, &slot.gl_texture);
      state.gl_context->doneCurrent();
    }
    slot.gl_texture = 0;
  }
#ifdef HAVE_CUDA
  if (slot.mipmapped_array) {
    cudaFreeMipmappedArray(slot.mipmapped_array);
    slot.mipmapped_array = nullptr;
  }
  slot.image_array = nullptr;
  if (slot.external_memory) {
    cudaDestroyExternalMemory(slot.external_memory);
    slot.external_memory = nullptr;
  }
#endif
  if (slot.shared_handle) {
    CloseHandle(slot.shared_handle);
    slot.shared_handle = nullptr;
  }
  slot.texture.Reset();
  slot.resource.Reset();
  slot.width                     = 0;
  slot.height                    = 0;
  slot.texture_handle            = 0;
  slot.d3d12_state               = D3D12_RESOURCE_STATE_COMMON;
  slot.pending_cuda_signal_value = 0;
  slot.ready_cuda_signal_value   = 0;
  slot.active_cuda_signal_value  = 0;
}

auto HasDirectPresentResource(const RhiEditViewerSurface::PlatformState::DirectPresentSlot& slot,
                              RhiEditViewerSurface::PlatformState::DirectPresentBackend     backend)
    -> bool {
  switch (backend) {
    case RhiEditViewerSurface::PlatformState::DirectPresentBackend::OpenGL:
      return slot.gl_texture != 0 && (false
#ifdef HAVE_OPENCL
                                      || slot.opencl_image != nullptr
#endif
                                     );
    case RhiEditViewerSurface::PlatformState::DirectPresentBackend::D3D11:
      return slot.texture != nullptr && (false
#ifdef HAVE_CUDA
                                         || slot.image_array != nullptr
#endif
#ifdef HAVE_OPENCL
                                         || slot.opencl_image != nullptr
#endif
                                        );
    case RhiEditViewerSurface::PlatformState::DirectPresentBackend::D3D12:
      return slot.resource != nullptr && (false
#ifdef HAVE_CUDA
                                          || slot.image_array != nullptr
#endif
                                         );
    case RhiEditViewerSurface::PlatformState::DirectPresentBackend::None:
      return false;
  }
  return false;
}

#ifdef HAVE_OPENCL
template <typename Fn>
auto GetOpenClD3D11Extension(cl_platform_id platform, const char* name) -> Fn {
  if (platform == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<Fn>(clGetExtensionFunctionAddressForPlatform(platform, name));
}

auto EnqueueOpenClD3D11Acquire(cl_command_queue queue, cl_mem image) -> bool {
  auto& context = OpenClContext::Instance();
  auto  acquire = GetOpenClD3D11Extension<clEnqueueAcquireD3D11ObjectsKHR_fn>(
      context.Platform(), "clEnqueueAcquireD3D11ObjectsKHR");
  if (acquire == nullptr || image == nullptr) {
    return false;
  }
  const cl_int error = acquire(queue, 1, &image, 0, nullptr, nullptr);
  if (error != CL_SUCCESS) {
    qWarning("RhiEditViewerSurface: clEnqueueAcquireD3D11ObjectsKHR failed with error %d.", error);
    return false;
  }
  return true;
}

auto EnqueueOpenClD3D11Release(cl_command_queue queue, cl_mem image) -> bool {
  auto& context = OpenClContext::Instance();
  auto  release = GetOpenClD3D11Extension<clEnqueueReleaseD3D11ObjectsKHR_fn>(
      context.Platform(), "clEnqueueReleaseD3D11ObjectsKHR");
  if (release == nullptr || image == nullptr) {
    return false;
  }
  const cl_int error = release(queue, 1, &image, 0, nullptr, nullptr);
  if (error != CL_SUCCESS) {
    qWarning("RhiEditViewerSurface: clEnqueueReleaseD3D11ObjectsKHR failed with error %d.", error);
    return false;
  }
  return true;
}

auto EnqueueOpenClGLAcquire(cl_command_queue queue, cl_mem image) -> bool {
  if (queue == nullptr || image == nullptr) {
    return false;
  }
  const cl_int error = clEnqueueAcquireGLObjects(queue, 1, &image, 0, nullptr, nullptr);
  if (error != CL_SUCCESS) {
    qWarning("RhiEditViewerSurface: clEnqueueAcquireGLObjects failed with error %d.", error);
    return false;
  }
  return true;
}

auto EnqueueOpenClGLRelease(cl_command_queue queue, cl_mem image) -> bool {
  if (queue == nullptr || image == nullptr) {
    return false;
  }
  const cl_int error = clEnqueueReleaseGLObjects(queue, 1, &image, 0, nullptr, nullptr);
  if (error != CL_SUCCESS) {
    qWarning("RhiEditViewerSurface: clEnqueueReleaseGLObjects failed with error %d.", error);
    return false;
  }
  return true;
}
#endif

auto EnsureD3D12TransitionObjects(RhiEditViewerSurface::PlatformState& state) -> bool {
  if (!state.d3d12_device || !state.d3d12_queue) {
    return false;
  }

  if (!state.d3d12_transition_allocator &&
      FAILED(state.d3d12_device->CreateCommandAllocator(
          D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(state.d3d12_transition_allocator.GetAddressOf())))) {
    qWarning("RhiEditViewerSurface: failed to create D3D12 transition command allocator.");
    return false;
  }

  if (!state.d3d12_transition_list) {
    if (FAILED(state.d3d12_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, state.d3d12_transition_allocator.Get(), nullptr,
            IID_PPV_ARGS(state.d3d12_transition_list.GetAddressOf())))) {
      qWarning("RhiEditViewerSurface: failed to create D3D12 transition command list.");
      return false;
    }
    state.d3d12_transition_list->Close();
  }

  if (!state.d3d12_transition_fence &&
      FAILED(state.d3d12_device->CreateFence(
          0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(state.d3d12_transition_fence.GetAddressOf())))) {
    qWarning("RhiEditViewerSurface: failed to create D3D12 transition fence.");
    return false;
  }

  if (!state.d3d12_transition_event) {
    state.d3d12_transition_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!state.d3d12_transition_event) {
      qWarning("RhiEditViewerSurface: failed to create D3D12 transition fence event.");
      return false;
    }
  }

  return true;
}

#ifdef HAVE_CUDA
auto EnsureD3D12CudaSemaphore(RhiEditViewerSurface::PlatformState& state) -> bool {
  if (!state.d3d12_device || !state.d3d12_queue) {
    return false;
  }
  if (state.d3d12_cuda_fence && state.cuda_signal_semaphore) {
    return true;
  }

  if (!state.d3d12_cuda_fence &&
      FAILED(state.d3d12_device->CreateFence(
          0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(state.d3d12_cuda_fence.GetAddressOf())))) {
    qWarning("RhiEditViewerSurface: failed to create D3D12/CUDA fence.");
    return false;
  }

  if (!state.d3d12_cuda_fence_shared_handle &&
      (FAILED(state.d3d12_device->CreateSharedHandle(state.d3d12_cuda_fence.Get(), nullptr,
                                                     GENERIC_ALL, nullptr,
                                                     &state.d3d12_cuda_fence_shared_handle)) ||
       !state.d3d12_cuda_fence_shared_handle)) {
    qWarning("RhiEditViewerSurface: failed to create shared handle for D3D12/CUDA fence.");
    return false;
  }

  if (!state.cuda_signal_semaphore) {
    cudaExternalSemaphoreHandleDesc semaphore_desc{};
    semaphore_desc.type                = cudaExternalSemaphoreHandleTypeD3D12Fence;
    semaphore_desc.handle.win32.handle = state.d3d12_cuda_fence_shared_handle;
    const cudaError_t import_err =
        cudaImportExternalSemaphore(&state.cuda_signal_semaphore, &semaphore_desc);
    if (import_err != cudaSuccess) {
      qWarning("RhiEditViewerSurface: cudaImportExternalSemaphore(D3D12 fence) failed: %s",
               cudaGetErrorString(import_err));
      return false;
    }
  }

  return true;
}
#endif

auto TransitionD3D12Slot(RhiEditViewerSurface::PlatformState&                    state,
                         RhiEditViewerSurface::PlatformState::DirectPresentSlot& slot,
                         D3D12_RESOURCE_STATES target_state) -> bool {
  if (state.backend != RhiEditViewerSurface::PlatformState::DirectPresentBackend::D3D12 ||
      !slot.resource || slot.d3d12_state == target_state) {
    return true;
  }

  if (!EnsureD3D12TransitionObjects(state)) {
    return false;
  }

  if (FAILED(state.d3d12_transition_allocator->Reset())) {
    qWarning("RhiEditViewerSurface: failed to reset D3D12 transition command allocator.");
    return false;
  }
  if (FAILED(state.d3d12_transition_list->Reset(state.d3d12_transition_allocator.Get(), nullptr))) {
    qWarning("RhiEditViewerSurface: failed to reset D3D12 transition command list.");
    return false;
  }

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource   = slot.resource.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = slot.d3d12_state;
  barrier.Transition.StateAfter  = target_state;
  state.d3d12_transition_list->ResourceBarrier(1, &barrier);

  if (FAILED(state.d3d12_transition_list->Close())) {
    qWarning("RhiEditViewerSurface: failed to close D3D12 transition command list.");
    return false;
  }

  ID3D12CommandList* command_lists[] = {state.d3d12_transition_list.Get()};
  state.d3d12_queue->ExecuteCommandLists(1, command_lists);

  const UINT64 fence_value = ++state.d3d12_transition_fence_value;
  if (FAILED(state.d3d12_queue->Signal(state.d3d12_transition_fence.Get(), fence_value))) {
    qWarning("RhiEditViewerSurface: failed to signal D3D12 transition fence.");
    return false;
  }
  if (state.d3d12_transition_fence->GetCompletedValue() < fence_value) {
    if (FAILED(state.d3d12_transition_fence->SetEventOnCompletion(fence_value,
                                                                  state.d3d12_transition_event))) {
      qWarning("RhiEditViewerSurface: failed to arm D3D12 transition fence event.");
      return false;
    }
    WaitForSingleObject(state.d3d12_transition_event, INFINITE);
  }

  slot.d3d12_state = target_state;
  return true;
}
#endif

RhiImageRenderer::~RhiImageRenderer() { releaseResources(); }

void RhiImageRenderer::initialize(QRhi* rhi, QRhiRenderTarget* render_target,
                                  QRhiCommandBuffer* command_buffer) {
  if (rhi_ != rhi) {
    releaseResources();
    rhi_ = rhi;
  }

  ensureStaticResources(render_target, command_buffer);
}

void RhiImageRenderer::releaseResources() {
  destroyResource(pipeline_);
  destroyResource(shader_resource_bindings_);
  destroyResource(vertex_buffer_);
  destroyResource(uniform_buffer_);
  destroyResource(primary_sampler_);
  destroyResource(detail_sampler_);
  destroyResource(interactive_texture_);
  destroyResource(quality_base_texture_);
  destroyResource(detail_patch_texture_);
  destroyResource(interactive_imported_texture_);
  destroyResource(quality_base_imported_texture_);
  destroyResource(detail_patch_imported_texture_);
  destroyResource(placeholder_texture_);
  bound_render_target_                 = nullptr;
  bound_primary_texture_               = nullptr;
  bound_detail_texture_                = nullptr;
  interactive_texture_width_           = 0;
  interactive_texture_height_          = 0;
  quality_base_texture_width_          = 0;
  quality_base_texture_height_         = 0;
  detail_patch_texture_width_          = 0;
  detail_patch_texture_height_         = 0;
  interactive_imported_width_          = 0;
  interactive_imported_height_         = 0;
  quality_base_imported_width_         = 0;
  quality_base_imported_height_        = 0;
  detail_patch_imported_width_         = 0;
  detail_patch_imported_height_        = 0;
  interactive_imported_native_object_  = 0;
  quality_base_imported_native_object_ = 0;
  detail_patch_imported_native_object_ = 0;
  static_upload_pending_               = false;
  for (auto& pending : pending_layers_) {
    pending = {};
  }
  for (auto& state : layer_states_) {
    state = {};
  }
  rhi_ = nullptr;
}

void RhiImageRenderer::queueFrame(const ViewerFrame& frame) {
  const LayerId layer    = layerIdForRole(frame.preview_metadata.frame_role);
  auto&         pending  = pendingLayer(layer);
  pending.host_frame     = frame;
  pending.pending_upload = std::make_unique<ViewerGpuFrameUpload>(
      ViewerGpuFrameUpload{frame.width, frame.height, frame.row_bytes, frame.pixels,
                           frame.display_config, frame.presentation_mode, frame.preview_metadata});
  pending.imported_frame = {};
  pending.imported_owner.reset();
  pending.has_update = true;

  if (layer == LayerId::QualityBase) {
    auto& detail_state = layerState(LayerId::DetailPatch);
    if (detail_state.valid && detail_state.preview_metadata.preview_generation !=
                                  frame.preview_metadata.preview_generation) {
      detail_state = {};
    }
  }
}

void RhiImageRenderer::queueImportedFrame(const ImportedTextureFrame& frame,
                                          std::shared_ptr<const void> owner) {
  const LayerId layer   = layerIdForRole(frame.preview_metadata.frame_role);
  auto&         pending = pendingLayer(layer);
  pending.host_frame    = {};
  pending.pending_upload.reset();
  pending.imported_frame = frame;
  pending.imported_owner = std::move(owner);
  pending.has_update     = true;

  if (layer == LayerId::QualityBase) {
    auto& detail_state = layerState(LayerId::DetailPatch);
    if (detail_state.valid && detail_state.preview_metadata.preview_generation !=
                                  frame.preview_metadata.preview_generation) {
      detail_state = {};
    }
  }
}

void RhiImageRenderer::releaseImportedTexture(std::uintptr_t texture_handle) {
  if (texture_handle == 0) {
    return;
  }

  const quint64 native_object = static_cast<quint64>(texture_handle);
  auto          release_layer = [&](LayerId layer, QRhiTexture*& texture, int& width, int& height,
                           quint64& imported_native_object) {
    if (imported_native_object != native_object) {
      return;
    }

    if (bound_primary_texture_ == texture || bound_detail_texture_ == texture) {
      destroyResource(shader_resource_bindings_);
      if (bound_primary_texture_ == texture) {
        bound_primary_texture_ = nullptr;
      }
      if (bound_detail_texture_ == texture) {
        bound_detail_texture_ = nullptr;
      }
    }

    destroyResource(texture);
    width                  = 0;
    height                 = 0;
    imported_native_object = 0;

    auto& state            = layerState(layer);
    if (state.source_is_imported) {
      state = {};
    }

    auto& pending = pendingLayer(layer);
    if (pending.imported_frame.texture_handle == texture_handle) {
      pending = {};
    }
  };

  release_layer(LayerId::InteractivePrimary, interactive_imported_texture_,
                interactive_imported_width_, interactive_imported_height_,
                interactive_imported_native_object_);
  release_layer(LayerId::QualityBase, quality_base_imported_texture_, quality_base_imported_width_,
                quality_base_imported_height_, quality_base_imported_native_object_);
  release_layer(LayerId::DetailPatch, detail_patch_imported_texture_, detail_patch_imported_width_,
                detail_patch_imported_height_, detail_patch_imported_native_object_);
}

auto RhiImageRenderer::currentRenderState(const ViewerViewState& view_state) const
    -> EditViewerRenderTargetState {
  return selectedRenderState(view_state);
}

void RhiImageRenderer::render(QRhiCommandBuffer* command_buffer, QRhiRenderTarget* render_target,
                              const ViewerViewState&    view_state,
                              const ViewportWidgetInfo& widget_info) {
  if (!command_buffer || !render_target || !rhi_) {
    return;
  }

  ensureStaticResources(render_target, command_buffer);

  QRhiResourceUpdateBatch* resource_updates = rhi_->nextResourceUpdateBatch();
  uploadPendingLayer(LayerId::InteractivePrimary, resource_updates, command_buffer);
  uploadPendingLayer(LayerId::QualityBase, resource_updates, command_buffer);
  uploadPendingLayer(LayerId::DetailPatch, resource_updates, command_buffer);

  QRhiTexture* primary_texture = selectedPrimaryTexture(view_state);
  QRhiTexture* detail_texture  = selectedDetailTexture(view_state);
  if (!primary_texture) {
    primary_texture = placeholder_texture_;
  }
  if (!detail_texture) {
    detail_texture = placeholder_texture_;
  }
  if (bound_primary_texture_ != primary_texture || bound_detail_texture_ != detail_texture ||
      !shader_resource_bindings_) {
    bound_primary_texture_ = primary_texture;
    bound_detail_texture_  = detail_texture;
    recreateShaderResources();
  }

  if (!pipeline_ && shader_resource_bindings_) {
    pipeline_ = rhi_->newGraphicsPipeline();
    QRhiVertexInputLayout input_layout;
    input_layout.setBindings({QRhiVertexInputBinding(sizeof(VertexData))});
    input_layout.setAttributes(
        {QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
         QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float))});
    pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipeline_->setCullMode(QRhiGraphicsPipeline::None);
    pipeline_->setSampleCount(render_target->sampleCount());
    pipeline_->setShaderStages(
        {QRhiShaderStage(QRhiShaderStage::Vertex, loadShader(kVertexShaderResource)),
         QRhiShaderStage(QRhiShaderStage::Fragment, loadShader(kFragmentShaderResource))});
    pipeline_->setVertexInputLayout(input_layout);
    pipeline_->setShaderResourceBindings(shader_resource_bindings_);
    pipeline_->setRenderPassDescriptor(render_target->renderPassDescriptor());
    pipeline_->create();
  }

  const auto  active_state = selectedRenderState(view_state);
  UniformData uniform_data;
  if (active_state.width > 0 && active_state.height > 0) {
    const auto scale = ViewportMapper::ComputeLetterboxScale(
        widget_info, ViewportImageInfo{active_state.width, active_state.height});
    float zoom  = view_state.snapshot.view_transform.zoom;
    float pan_x = view_state.snapshot.view_transform.pan.x();
    float pan_y = view_state.snapshot.view_transform.pan.y();
    if (active_state.presentation_mode == FramePresentationMode::RoiFrame) {
      zoom  = 1.0f;
      pan_x = 0.0f;
      pan_y = 0.0f;
    }

    uniform_data.scale_zoom[0] = scale.x;
    uniform_data.scale_zoom[1] = scale.y;
    uniform_data.scale_zoom[2] = zoom;
    uniform_data.pan_mode[0]   = pan_x;
    uniform_data.pan_mode[1]   = pan_y;
    uniform_data.pan_mode[2] =
        active_state.presentation_mode == FramePresentationMode::RoiFrame ? 1.0f : 0.0f;
  }

  if (hasVisibleDetailPatch(view_state)) {
    const auto& detail_state     = layerState(LayerId::DetailPatch);
    uniform_data.detail_roi[0]   = detail_state.preview_metadata.source_roi_norm.x;
    uniform_data.detail_roi[1]   = detail_state.preview_metadata.source_roi_norm.y;
    uniform_data.detail_roi[2]   = detail_state.preview_metadata.source_roi_norm.width;
    uniform_data.detail_roi[3]   = detail_state.preview_metadata.source_roi_norm.height;
    uniform_data.detail_flags[0] = 1.0f;
  }
  resource_updates->updateDynamicBuffer(uniform_buffer_, 0, sizeof(UniformData), &uniform_data);

  command_buffer->beginPass(render_target, Qt::black, {1.0f, 0}, resource_updates);
  if (active_state.width > 0 && active_state.height > 0 && pipeline_ && shader_resource_bindings_ &&
      vertex_buffer_) {
    const QSize                          rt_size        = render_target->pixelSize();
    const QRhiCommandBuffer::VertexInput vertex_input[] = {{vertex_buffer_, 0}};
    command_buffer->setGraphicsPipeline(pipeline_);
    command_buffer->setViewport(QRhiViewport(0, 0, rt_size.width(), rt_size.height()));
    command_buffer->setShaderResources(shader_resource_bindings_);
    command_buffer->setVertexInput(0, 1, vertex_input);
    command_buffer->draw(4);
  }
  command_buffer->endPass();
}

auto RhiImageRenderer::loadShader(const char* resource_path) const -> QShader {
  QFile shader_file(QString::fromUtf8(resource_path));
  if (!shader_file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return QShader::fromSerialized(shader_file.readAll());
}

void RhiImageRenderer::destroyResource(QRhiTexture*& resource) {
  if (!resource) {
    return;
  }
  resource->destroy();
  delete resource;
  resource = nullptr;
}

void RhiImageRenderer::destroyResource(QRhiSampler*& resource) {
  if (!resource) {
    return;
  }
  resource->destroy();
  delete resource;
  resource = nullptr;
}

void RhiImageRenderer::destroyResource(QRhiBuffer*& resource) {
  if (!resource) {
    return;
  }
  resource->destroy();
  delete resource;
  resource = nullptr;
}

void RhiImageRenderer::destroyResource(QRhiShaderResourceBindings*& resource) {
  if (!resource) {
    return;
  }
  resource->destroy();
  delete resource;
  resource = nullptr;
}

void RhiImageRenderer::destroyResource(QRhiGraphicsPipeline*& resource) {
  if (!resource) {
    return;
  }
  resource->destroy();
  delete resource;
  resource = nullptr;
}

auto RhiImageRenderer::layerIdForRole(FrameRole role) const -> LayerId {
  switch (role) {
    case FrameRole::InteractivePrimary:
      return LayerId::InteractivePrimary;
    case FrameRole::QualityBase:
      return LayerId::QualityBase;
    case FrameRole::DetailPatch:
      return LayerId::DetailPatch;
  }
  return LayerId::InteractivePrimary;
}

auto RhiImageRenderer::pendingLayer(LayerId layer) -> PendingLayerFrame& {
  return pending_layers_[static_cast<size_t>(layer)];
}

auto RhiImageRenderer::pendingLayer(LayerId layer) const -> const PendingLayerFrame& {
  return pending_layers_[static_cast<size_t>(layer)];
}

auto RhiImageRenderer::layerState(LayerId layer) -> LayerTextureState& {
  return layer_states_[static_cast<size_t>(layer)];
}

auto RhiImageRenderer::layerState(LayerId layer) const -> const LayerTextureState& {
  return layer_states_[static_cast<size_t>(layer)];
}

void RhiImageRenderer::ensureStaticResources(QRhiRenderTarget*  render_target,
                                             QRhiCommandBuffer* command_buffer) {
  if (!rhi_ || !render_target) {
    return;
  }

  if (bound_render_target_ != render_target) {
    destroyResource(pipeline_);
    bound_render_target_ = render_target;
  }

  if (!placeholder_texture_) {
    placeholder_texture_ = rhi_->newTexture(QRhiTexture::RGBA32F, QSize(1, 1), 1);
    placeholder_texture_->create();
    static_upload_pending_ = true;
  }

  if (!primary_sampler_) {
    primary_sampler_ = rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    primary_sampler_->create();
  }

  if (!detail_sampler_) {
    detail_sampler_ =
        rhi_->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                         QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    detail_sampler_->create();
  }

  if (!uniform_buffer_) {
    uniform_buffer_ =
        rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(UniformData));
    uniform_buffer_->create();
  }

  if (!vertex_buffer_) {
    vertex_buffer_ =
        rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, 4 * sizeof(VertexData));
    vertex_buffer_->create();
    static_upload_pending_ = true;
  }

  if (command_buffer && vertex_buffer_ && placeholder_texture_ && static_upload_pending_) {
    static constexpr std::array<VertexData, 4> kVertices = {
        VertexData{{-1.0f, -1.0f}, {0.0f, 1.0f}},
        VertexData{{1.0f, -1.0f}, {1.0f, 1.0f}},
        VertexData{{-1.0f, 1.0f}, {0.0f, 0.0f}},
        VertexData{{1.0f, 1.0f}, {1.0f, 0.0f}},
    };
    static constexpr std::array<float, 4> kBlackPixel = {0.0f, 0.0f, 0.0f, 1.0f};

    QRhiResourceUpdateBatch*              updates     = rhi_->nextResourceUpdateBatch();
    updates->uploadStaticBuffer(vertex_buffer_, kVertices.data());

    QByteArray black_upload(reinterpret_cast<const char*>(kBlackPixel.data()),
                            static_cast<int>(sizeof(kBlackPixel)));
    QRhiTextureSubresourceUploadDescription black_desc(black_upload);
    black_desc.setDataStride(static_cast<quint32>(sizeof(kBlackPixel)));
    black_desc.setSourceSize(QSize(1, 1));
    updates->uploadTexture(placeholder_texture_,
                           QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, black_desc)));
    command_buffer->resourceUpdate(updates);
    static_upload_pending_ = false;
  }
}

void RhiImageRenderer::ensureTexture(QRhiTexture*& texture, int& width, int& height,
                                     const QSize& size) {
  if (!rhi_ || size.width() <= 0 || size.height() <= 0) {
    return;
  }
  if (texture && width == size.width() && height == size.height()) {
    return;
  }

  if (bound_primary_texture_ == texture || bound_detail_texture_ == texture) {
    destroyResource(shader_resource_bindings_);
    if (bound_primary_texture_ == texture) {
      bound_primary_texture_ = nullptr;
    }
    if (bound_detail_texture_ == texture) {
      bound_detail_texture_ = nullptr;
    }
  }

  destroyResource(texture);
  texture = rhi_->newTexture(QRhiTexture::RGBA32F, size, 1);
  texture->create();
  width  = size.width();
  height = size.height();
}

void RhiImageRenderer::ensureImportedTexture(QRhiTexture*& texture, int& width, int& height,
                                             quint64&                    native_object,
                                             const ImportedTextureFrame& frame) {
  if (!rhi_ || !frame) {
    return;
  }

  const quint64 next_native_object = static_cast<quint64>(frame.texture_handle);
  if (texture && width == frame.width && height == frame.height &&
      native_object == next_native_object) {
    texture->setNativeLayout(frame.native_layout);
    return;
  }

  if (bound_primary_texture_ == texture || bound_detail_texture_ == texture) {
    destroyResource(shader_resource_bindings_);
    if (bound_primary_texture_ == texture) {
      bound_primary_texture_ = nullptr;
    }
    if (bound_detail_texture_ == texture) {
      bound_detail_texture_ = nullptr;
    }
  }

  destroyResource(texture);
  texture = rhi_->newTexture(QRhiTexture::RGBA32F, QSize(frame.width, frame.height), 1);
  if (!texture->createFrom({next_native_object, frame.native_layout})) {
    qWarning(
        "RhiImageRenderer: failed to import native texture object=0x%llx size=%dx%d layout=%d "
        "backend=%d.",
        static_cast<unsigned long long>(next_native_object), frame.width, frame.height,
        frame.native_layout, static_cast<int>(rhi_->backend()));
    destroyResource(texture);
    width         = 0;
    height        = 0;
    native_object = 0;
    return;
  }
  texture->setNativeLayout(frame.native_layout);

  width         = frame.width;
  height        = frame.height;
  native_object = next_native_object;
}

void RhiImageRenderer::uploadPendingLayer(LayerId layer, QRhiResourceUpdateBatch* resource_updates,
                                          QRhiCommandBuffer* command_buffer) {
  auto& pending = pendingLayer(layer);
  if (!pending.has_update || !resource_updates) {
    return;
  }

  auto& state = layerState(layer);
  if (pending.host_frame && pending.pending_upload) {
    QRhiTexture** target_texture = nullptr;
    int*          target_width   = nullptr;
    int*          target_height  = nullptr;
    switch (layer) {
      case LayerId::InteractivePrimary:
        target_texture = &interactive_texture_;
        target_width   = &interactive_texture_width_;
        target_height  = &interactive_texture_height_;
        break;
      case LayerId::QualityBase:
        target_texture = &quality_base_texture_;
        target_width   = &quality_base_texture_width_;
        target_height  = &quality_base_texture_height_;
        break;
      case LayerId::DetailPatch:
        target_texture = &detail_patch_texture_;
        target_width   = &detail_patch_texture_width_;
        target_height  = &detail_patch_texture_height_;
        break;
    }

    ensureTexture(*target_texture, *target_width, *target_height,
                  QSize(pending.pending_upload->width, pending.pending_upload->height));
    const size_t upload_bytes =
        pending.pending_upload->row_bytes * static_cast<size_t>(pending.pending_upload->height);
    if (*target_texture && upload_bytes <= static_cast<size_t>((std::numeric_limits<int>::max)())) {
      QByteArray upload_data =
          QByteArray::fromRawData(static_cast<const char*>(pending.pending_upload->pixels.get()),
                                  static_cast<int>(upload_bytes));
      QRhiTextureSubresourceUploadDescription upload_desc(upload_data);
      upload_desc.setDataStride(static_cast<quint32>(pending.pending_upload->row_bytes));
      upload_desc.setSourceSize(
          QSize(pending.pending_upload->width, pending.pending_upload->height));
      resource_updates->uploadTexture(
          *target_texture, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, upload_desc)));

      state.width              = pending.host_frame.width;
      state.height             = pending.host_frame.height;
      state.presentation_mode  = pending.host_frame.presentation_mode;
      state.preview_metadata   = pending.host_frame.preview_metadata;
      state.imported_owner     = pending.pending_upload->pixels;
      state.valid              = true;
      state.source_is_imported = false;
    }
  } else if (pending.imported_frame) {
    switch (layer) {
      case LayerId::InteractivePrimary: {
        ensureImportedTexture(interactive_imported_texture_, interactive_imported_width_,
                              interactive_imported_height_, interactive_imported_native_object_,
                              pending.imported_frame);
        if (interactive_imported_texture_) {
          state.width              = pending.imported_frame.width;
          state.height             = pending.imported_frame.height;
          state.presentation_mode  = pending.imported_frame.presentation_mode;
          state.preview_metadata   = pending.imported_frame.preview_metadata;
          state.imported_owner     = pending.imported_owner;
          state.valid              = true;
          state.source_is_imported = true;
        }
        break;
      }
      case LayerId::QualityBase: {
        ensureImportedTexture(quality_base_imported_texture_, quality_base_imported_width_,
                              quality_base_imported_height_, quality_base_imported_native_object_,
                              pending.imported_frame);
        ensureTexture(quality_base_texture_, quality_base_texture_width_,
                      quality_base_texture_height_,
                      QSize(pending.imported_frame.width, pending.imported_frame.height));
        if (quality_base_imported_texture_ && quality_base_texture_) {
          QRhiTextureCopyDescription copy_desc;
          copy_desc.setPixelSize(
              QSize(pending.imported_frame.width, pending.imported_frame.height));
          resource_updates->copyTexture(quality_base_texture_, quality_base_imported_texture_,
                                        copy_desc);
          state.width              = pending.imported_frame.width;
          state.height             = pending.imported_frame.height;
          state.presentation_mode  = pending.imported_frame.presentation_mode;
          state.preview_metadata   = pending.imported_frame.preview_metadata;
          state.imported_owner     = pending.imported_owner;
          state.valid              = true;
          state.source_is_imported = false;
        }
        break;
      }
      case LayerId::DetailPatch: {
        ensureImportedTexture(detail_patch_imported_texture_, detail_patch_imported_width_,
                              detail_patch_imported_height_, detail_patch_imported_native_object_,
                              pending.imported_frame);
        ensureTexture(detail_patch_texture_, detail_patch_texture_width_,
                      detail_patch_texture_height_,
                      QSize(pending.imported_frame.width, pending.imported_frame.height));
        if (detail_patch_imported_texture_ && detail_patch_texture_) {
          QRhiTextureCopyDescription copy_desc;
          copy_desc.setPixelSize(
              QSize(pending.imported_frame.width, pending.imported_frame.height));
          resource_updates->copyTexture(detail_patch_texture_, detail_patch_imported_texture_,
                                        copy_desc);
          state.width              = pending.imported_frame.width;
          state.height             = pending.imported_frame.height;
          state.presentation_mode  = pending.imported_frame.presentation_mode;
          state.preview_metadata   = pending.imported_frame.preview_metadata;
          state.imported_owner     = pending.imported_owner;
          state.valid              = true;
          state.source_is_imported = false;
        }
        break;
      }
    }
  }

  pending = {};
  (void)command_buffer;
}

auto RhiImageRenderer::selectedPrimaryTexture(const ViewerViewState& view_state) const
    -> QRhiTexture* {
  const auto& interactive = layerState(LayerId::InteractivePrimary);
  const auto& quality     = layerState(LayerId::QualityBase);
  const auto  current_viewport_roi =
      BuildNormalizedViewportRoi(view_state.snapshot.viewport_render_region_cache);
  const bool interactive_matches_viewport =
      interactive.presentation_mode != FramePresentationMode::RoiFrame ||
      (current_viewport_roi.has_value() &&
       RoiRectsMatch(interactive.preview_metadata.source_roi_norm, *current_viewport_roi));

  bool use_interactive = false;
  if (interactive.valid) {
    if (!quality.valid) {
      use_interactive = true;
    } else {
      const auto interactive_generation = interactive.preview_metadata.preview_generation;
      const auto quality_generation     = quality.preview_metadata.preview_generation;
      if (interactive_generation > quality_generation) {
        use_interactive = true;
      } else if (interactive_generation == quality_generation &&
                 view_state.prefer_interactive_primary && interactive_matches_viewport) {
        use_interactive = true;
      }
    }
  }

  if (use_interactive) {
    if (interactive.source_is_imported && interactive_imported_texture_) {
      return interactive_imported_texture_;
    }
    if (interactive_texture_) {
      return interactive_texture_;
    }
  }

  if (quality.valid && quality_base_texture_) {
    return quality_base_texture_;
  }

  if (interactive.valid) {
    if (interactive.source_is_imported && interactive_imported_texture_) {
      return interactive_imported_texture_;
    }
    if (interactive_texture_) {
      return interactive_texture_;
    }
  }

  return placeholder_texture_;
}

auto RhiImageRenderer::hasVisibleDetailPatch(const ViewerViewState& view_state) const -> bool {
  if (!view_state.allow_detail_patch || !view_state.has_expected_detail_token) {
    return false;
  }

  const auto& quality = layerState(LayerId::QualityBase);
  const auto& detail  = layerState(LayerId::DetailPatch);
  if (!quality.valid || !detail.valid || !detail_patch_texture_) {
    return false;
  }

  const bool token_matches =
      detail.preview_metadata.preview_generation == view_state.expected_detail_generation &&
      detail.preview_metadata.detail_serial == view_state.expected_detail_serial &&
      quality.preview_metadata.preview_generation == detail.preview_metadata.preview_generation;
  if (!token_matches) {
    return false;
  }

  const auto& roi = detail.preview_metadata.source_roi_norm;
  if (quality.width <= 0 || quality.height <= 0 || detail.width <= 0 || detail.height <= 0 ||
      roi.width <= 1.0e-4f || roi.height <= 1.0e-4f) {
    return false;
  }

  const auto current_viewport_roi =
      BuildNormalizedViewportRoi(view_state.snapshot.viewport_render_region_cache);
  if (!current_viewport_roi.has_value() || !RoiRectsMatch(roi, *current_viewport_roi)) {
    return false;
  }

  const float expected_aspect = (static_cast<float>(quality.width) * roi.width) /
                                (static_cast<float>(quality.height) * roi.height);
  const float actual_aspect = static_cast<float>(detail.width) / static_cast<float>(detail.height);
  if (expected_aspect <= 1.0e-4f || actual_aspect <= 1.0e-4f) {
    return false;
  }

  return std::abs(expected_aspect - actual_aspect) <=
         (kDetailPatchAspectTolerance * expected_aspect);
}

auto RhiImageRenderer::selectedDetailTexture(const ViewerViewState& view_state) const
    -> QRhiTexture* {
  if (hasVisibleDetailPatch(view_state)) {
    return detail_patch_texture_;
  }
  return placeholder_texture_;
}

auto RhiImageRenderer::selectedRenderState(const ViewerViewState& view_state) const
    -> EditViewerRenderTargetState {
  EditViewerRenderTargetState state{};
  const auto&                 interactive = layerState(LayerId::InteractivePrimary);
  const auto&                 quality     = layerState(LayerId::QualityBase);
  const auto                  current_viewport_roi =
      BuildNormalizedViewportRoi(view_state.snapshot.viewport_render_region_cache);
  const bool interactive_matches_viewport =
      interactive.presentation_mode != FramePresentationMode::RoiFrame ||
      (current_viewport_roi.has_value() &&
       RoiRectsMatch(interactive.preview_metadata.source_roi_norm, *current_viewport_roi));

  bool use_interactive = false;
  if (interactive.valid) {
    if (!quality.valid) {
      use_interactive = true;
    } else {
      const auto interactive_generation = interactive.preview_metadata.preview_generation;
      const auto quality_generation     = quality.preview_metadata.preview_generation;
      if (interactive_generation > quality_generation) {
        use_interactive = true;
      } else if (interactive_generation == quality_generation &&
                 view_state.prefer_interactive_primary && interactive_matches_viewport) {
        use_interactive = true;
      }
    }
  }

  const LayerTextureState* selected = nullptr;
  if (use_interactive) {
    selected = &interactive;
  } else if (quality.valid) {
    selected = &quality;
  } else if (interactive.valid) {
    selected = &interactive;
  }

  if (!selected) {
    return state;
  }

  state.width             = selected->width;
  state.height            = selected->height;
  state.presentation_mode = selected->presentation_mode;
  state.preview_metadata  = selected->preview_metadata;
  return state;
}

void RhiImageRenderer::recreateShaderResources() {
  destroyResource(shader_resource_bindings_);
  if (!rhi_ || !uniform_buffer_ || !primary_sampler_ || !detail_sampler_ ||
      !bound_primary_texture_ || !bound_detail_texture_) {
    return;
  }

  shader_resource_bindings_ = rhi_->newShaderResourceBindings();
  shader_resource_bindings_->setBindings(
      {QRhiShaderResourceBinding::uniformBuffer(
           0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
           uniform_buffer_),
       QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                 bound_primary_texture_, primary_sampler_),
       QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                 bound_detail_texture_, detail_sampler_)});
  shader_resource_bindings_->create();
  destroyResource(pipeline_);
}

RhiEditViewerSurface::RhiEditViewerSurface(QWidget* parent)
    : QRhiWidget(parent), platform_state_(std::make_unique<PlatformState>()) {
  setAutoFillBackground(false);
  setMouseTracking(false);
#if defined(Q_OS_WIN)
  setApi(ShouldUseOpenClRhiBackend() ? QRhiWidget::Api::OpenGL : QRhiWidget::Api::Direct3D11);
#elif defined(HAVE_METAL)
  setApi(QRhiWidget::Api::Metal);
#endif
  setColorBufferFormat(QRhiWidget::TextureFormat::RGBA32F);
}

RhiEditViewerSurface::~RhiEditViewerSurface() {
  renderer_.releaseResources();
  releasePlatformTargets();
}

auto RhiEditViewerSurface::widget() -> QWidget* { return this; }

void RhiEditViewerSurface::submitFrame(const ViewerFrame& frame) { renderer_.queueFrame(frame); }

#ifdef HAVE_METAL
void RhiEditViewerSurface::submitMetalFrame(const ViewerMetalFrame& frame) {
  if (!frame) {
    return;
  }
  ImportedTextureFrame imported_frame;
  imported_frame.width             = frame.width;
  imported_frame.height            = frame.height;
  imported_frame.texture_handle    = frame.texture_handle;
  imported_frame.presentation_mode = frame.presentation_mode;
  imported_frame.preview_metadata  = frame.preview_metadata;
  renderer_.queueImportedFrame(imported_frame, frame.owner);
}
#endif

void RhiEditViewerSurface::setDisplayConfig(const ViewerDisplayConfig& config) {
  if (config == display_config_ && !display_config_dirty_) {
    return;
  }
  display_config_       = config;
  display_config_dirty_ = true;
  applyDisplayConfig();
}

void RhiEditViewerSurface::setViewState(const ViewerViewState& state) { view_state_ = state; }

void RhiEditViewerSurface::requestRedraw() { update(); }

auto RhiEditViewerSurface::supportsDirectGpuPresent() const -> bool {
  return platform_state_ && platform_state_->supports_direct_present;
}

auto RhiEditViewerSurface::supportsDirectCudaPresent() const -> bool {
#if defined(Q_OS_WIN) && defined(HAVE_CUDA)
  return supportsDirectGpuPresent() && platform_state_->cuda_device >= 0;
#else
  return false;
#endif
}

auto RhiEditViewerSurface::prepareRenderTarget(int width, int height)
    -> EditViewerRenderTargetResizeDecision {
  EditViewerRenderTargetResizeDecision decision{};
#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
  if (!supportsDirectGpuPresent()) {
    return decision;
  }

  std::lock_guard<std::mutex>                                        lock(platform_state_->mutex);
  std::array<DirectPresentSlotAvailability, kDirectPresentSlotCount> slot_infos{};
  for (size_t i = 0; i < platform_state_->targets.size(); ++i) {
    const auto& target = platform_state_->targets[i];
    slot_infos[i]      = DirectPresentSlotAvailability{
        target.width,
        target.height,
        HasDirectPresentResource(target, platform_state_->backend),
        static_cast<int>(i) == platform_state_->active_idx ||
            platform_state_->pending_frames.ContainsSlot(static_cast<int>(i)) ||
            static_cast<int>(i) == platform_state_->ready_slot_idx ||
            static_cast<int>(i) == platform_state_->mapped_slot_idx,
    };
  }

  const DirectPresentTargetSelection selection = SelectDirectPresentWriteSlot(
      slot_infos.data(), slot_infos.size(), platform_state_->write_idx, width, height);
  if (slot_infos[selection.slot_index].unavailable &&
      !IsReusableDirectPresentSlot(slot_infos[selection.slot_index], width, height)) {
    platform_state_->render_target_idx = -1;
    decision.slot_index                = -1;
    decision.need_resize               = false;
    return decision;
  }
  platform_state_->write_idx         = selection.slot_index;
  platform_state_->render_target_idx = selection.slot_index;
  decision.slot_index                = selection.slot_index;
  decision.need_resize               = selection.need_resize;
#else
  (void)width;
  (void)height;
#endif
  return decision;
}

void RhiEditViewerSurface::commitRenderTargetResize(int slot_index, int width, int height) {
#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
  if (!supportsDirectGpuPresent() ||
      !IsValidSlotIndex(slot_index, platform_state_->targets.size())) {
    return;
  }

  std::lock_guard<std::mutex> lock(platform_state_->mutex);
  platform_state_->targets[slot_index].width  = width;
  platform_state_->targets[slot_index].height = height;
  platform_state_->render_target_idx          = slot_index;
#else
  (void)slot_index;
  (void)width;
  (void)height;
#endif
}

auto RhiEditViewerSurface::mapResourceForWrite(FrameMemoryDomain preferred_domain)
    -> FrameWriteMapping {
#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
  if (!supportsDirectGpuPresent()) {
    return {};
  }

  if (preferred_domain == FrameMemoryDomain::CudaDevice) {
#ifdef HAVE_CUDA
    if (!BindCudaDeviceOnCurrentThread(platform_state_->cuda_device, "mapResourceForWrite")) {
      return {};
    }
#else
    return {};
#endif
  }

  platform_state_->mutex.lock();
  if (!IsValidSlotIndex(platform_state_->render_target_idx, platform_state_->targets.size())) {
    platform_state_->mutex.unlock();
    return {};
  }
  auto& slot = platform_state_->targets[platform_state_->render_target_idx];

  if (preferred_domain == FrameMemoryDomain::OpenClDevice) {
#ifdef HAVE_OPENCL
    if (!slot.opencl_image || slot.width <= 0 || slot.height <= 0 ||
        !OpenClContext::Instance().IsInitialized()) {
      platform_state_->mutex.unlock();
      return {};
    }
    bool acquired = false;
    if (platform_state_->backend == PlatformState::DirectPresentBackend::OpenGL) {
      acquired = EnqueueOpenClGLAcquire(OpenClContext::Instance().Queue(), slot.opencl_image);
    } else if (platform_state_->backend == PlatformState::DirectPresentBackend::D3D11) {
      acquired = EnqueueOpenClD3D11Acquire(OpenClContext::Instance().Queue(), slot.opencl_image);
    }
    if (!acquired) {
      platform_state_->mutex.unlock();
      return {};
    }
    slot.opencl_acquired             = true;
    platform_state_->mapped_slot_idx = platform_state_->render_target_idx;
    FrameWriteMapping mapping{};
    mapping.data          = slot.opencl_image;
    mapping.row_bytes     = static_cast<size_t>(slot.width) * kRgba32fPixelBytes;
    mapping.pixel_format  = FramePixelFormat::RGBA32F;
    mapping.memory_domain = FrameMemoryDomain::OpenClDevice;
    mapping.target_type   = FrameWriteTargetType::OpenClImage;
    mapping.native_object = slot.texture_handle;
    return mapping;
#else
    platform_state_->mutex.unlock();
    return {};
#endif
  }

#ifdef HAVE_CUDA
  if (platform_state_->backend == PlatformState::DirectPresentBackend::D3D12 &&
      !TransitionD3D12Slot(*platform_state_, slot, D3D12_RESOURCE_STATE_COMMON)) {
    qWarning("RhiEditViewerSurface: failed to transition D3D12 present target for CUDA write.");
    platform_state_->mutex.unlock();
    return {};
  }
  if (platform_state_->backend == PlatformState::DirectPresentBackend::D3D12 &&
      !EnsureD3D12CudaSemaphore(*platform_state_)) {
    qWarning("RhiEditViewerSurface: failed to prepare D3D12/CUDA synchronization fence.");
    platform_state_->mutex.unlock();
    return {};
  }
  if (!slot.image_array || slot.width <= 0 || slot.height <= 0) {
    qWarning("RhiEditViewerSurface: no valid D3D/CUDA present target is available.");
    platform_state_->mutex.unlock();
    return {};
  }

  platform_state_->mapped_slot_idx = platform_state_->render_target_idx;
  FrameWriteMapping mapping{};
  mapping.image_array   = slot.image_array;
  mapping.row_bytes     = static_cast<size_t>(slot.width) * kRgba32fPixelBytes;
  mapping.pixel_format  = FramePixelFormat::RGBA32F;
  mapping.memory_domain = FrameMemoryDomain::CudaDevice;
  mapping.target_type   = FrameWriteTargetType::CudaArray;
  mapping.native_object = slot.texture_handle;
  if (platform_state_->backend == PlatformState::DirectPresentBackend::D3D12 &&
      platform_state_->cuda_signal_semaphore) {
    slot.pending_cuda_signal_value = ++platform_state_->d3d12_cuda_fence_value;
    mapping.cuda_signal_semaphore = reinterpret_cast<void*>(platform_state_->cuda_signal_semaphore);
    mapping.cuda_signal_value     = slot.pending_cuda_signal_value;
  }
  return mapping;
#else
  platform_state_->mutex.unlock();
  return {};
#endif
#else
  (void)preferred_domain;
  return {};
#endif
}

void RhiEditViewerSurface::unmapResource() {
#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
  if (!supportsDirectGpuPresent()) {
    return;
  }

  if (platform_state_->mapped_slot_idx >= 0 &&
      IsValidSlotIndex(platform_state_->mapped_slot_idx, platform_state_->targets.size())) {
    auto& slot = platform_state_->targets[platform_state_->mapped_slot_idx];
#ifdef HAVE_OPENCL
    if (slot.opencl_acquired) {
      if (platform_state_->backend == PlatformState::DirectPresentBackend::OpenGL) {
        (void)EnqueueOpenClGLRelease(OpenClContext::Instance().Queue(), slot.opencl_image);
      } else if (platform_state_->backend == PlatformState::DirectPresentBackend::D3D11) {
        (void)EnqueueOpenClD3D11Release(OpenClContext::Instance().Queue(), slot.opencl_image);
      }
      slot.opencl_acquired = false;
    }
#endif
    slot.ready_cuda_signal_value    = slot.pending_cuda_signal_value;
    slot.pending_cuda_signal_value  = 0;
    platform_state_->ready_slot_idx = platform_state_->mapped_slot_idx;
  }
  platform_state_->mapped_slot_idx = -1;
  platform_state_->mutex.unlock();
#endif
}

void RhiEditViewerSurface::notifyFrameReady() {
#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
  if (!supportsDirectGpuPresent()) {
    return;
  }

  std::lock_guard<std::mutex> lock(platform_state_->mutex);
  if (platform_state_->ready_slot_idx >= 0) {
    auto& slot = platform_state_->targets[platform_state_->ready_slot_idx];
    if (platform_state_->pending_presentation_mode_valid.exchange(false,
                                                                  std::memory_order_acq_rel)) {
      slot.presentation_mode =
          platform_state_->pending_presentation_mode.load(std::memory_order_acquire);
    } else {
      slot.presentation_mode =
          platform_state_->active_presentation_mode.load(std::memory_order_acquire);
    }
    if (platform_state_->pending_preview_metadata_valid) {
      slot.preview_metadata                           = platform_state_->pending_preview_metadata;
      platform_state_->pending_preview_metadata_valid = false;
    } else {
      slot.preview_metadata = platform_state_->active_preview_metadata;
    }
    platform_state_->pending_frames.MarkReadySlot(platform_state_->ready_slot_idx);
    platform_state_->ready_slot_idx = -1;
  }
#endif
}

void RhiEditViewerSurface::setNextFramePresentationMode(FramePresentationMode mode) {
#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
  if (!supportsDirectGpuPresent()) {
    return;
  }
  platform_state_->pending_presentation_mode.store(mode, std::memory_order_release);
  platform_state_->pending_presentation_mode_valid.store(true, std::memory_order_release);
#else
  (void)mode;
#endif
}

void RhiEditViewerSurface::setNextFramePreviewMetadata(const FramePreviewMetadata& metadata) {
#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
  if (!supportsDirectGpuPresent()) {
    return;
  }
  std::lock_guard<std::mutex> lock(platform_state_->mutex);
  platform_state_->pending_preview_metadata       = metadata;
  platform_state_->pending_preview_metadata_valid = true;
#else
  (void)metadata;
#endif
}

auto RhiEditViewerSurface::activeRenderTargetState() const -> EditViewerRenderTargetState {
  auto state = renderer_.currentRenderState(view_state_);
#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
  if (!supportsDirectGpuPresent()) {
    return state;
  }

  std::lock_guard<std::mutex> lock(platform_state_->mutex);
  state.slot_index = platform_state_->active_idx;
  const auto pending_slots = platform_state_->pending_frames.PendingSlotsSnapshot();
  for (int pending_slot : pending_slots) {
    if (!IsValidSlotIndex(pending_slot, platform_state_->targets.size())) {
      continue;
    }
    const auto& slot = platform_state_->targets[pending_slot];
    if (slot.width <= 0 || slot.height <= 0 ||
        !IsRenderReferenceFrame(slot.presentation_mode, slot.preview_metadata.frame_role)) {
      continue;
    }
    if (state.width > 0 && state.height > 0 &&
        state.preview_metadata.preview_generation > slot.preview_metadata.preview_generation) {
      continue;
    }
    state.slot_index         = pending_slot;
    state.width              = slot.width;
    state.height             = slot.height;
    state.presentation_mode  = slot.presentation_mode;
    state.preview_metadata   = slot.preview_metadata;
  }
#endif
  return state;
}

auto RhiEditViewerSurface::hasRenderTarget(int slot_index, int width, int height) const -> bool {
#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
  if (!supportsDirectGpuPresent() ||
      !IsValidSlotIndex(slot_index, platform_state_->targets.size())) {
    return false;
  }

  std::lock_guard<std::mutex> lock(platform_state_->mutex);
  const auto&                 slot = platform_state_->targets[slot_index];
  return HasDirectPresentResource(slot, platform_state_->backend) && slot.width == width &&
         slot.height == height;
#else
  (void)slot_index;
  (void)width;
  (void)height;
  return false;
#endif
}

auto RhiEditViewerSurface::ensureRenderTarget(int slot_index, int width, int height) -> bool {
#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
  if (!supportsDirectGpuPresent() ||
      !IsValidSlotIndex(slot_index, platform_state_->targets.size()) ||
      platform_state_->backend == PlatformState::DirectPresentBackend::None || width <= 0 ||
      height <= 0) {
    return false;
  }

#ifdef HAVE_CUDA
  if (supportsDirectCudaPresent() &&
      !BindCudaDeviceOnCurrentThread(platform_state_->cuda_device, "ensureRenderTarget")) {
    return false;
  }
#endif

  std::lock_guard<std::mutex> lock(platform_state_->mutex);
  auto&                       slot = platform_state_->targets[slot_index];
  if (HasDirectPresentResource(slot, platform_state_->backend) && slot.width == width &&
      slot.height == height) {
    return true;
  }

  if (slot.texture_handle != 0) {
    renderer_.releaseImportedTexture(slot.texture_handle);
  }
  ReleaseDirectPresentSlot(*platform_state_, slot);

#ifdef HAVE_CUDA
  cudaExternalMemoryHandleType handle_type = cudaExternalMemoryHandleTypeOpaqueWin32;
  unsigned long long           handle_size = 0;
#endif

  if (platform_state_->backend == PlatformState::DirectPresentBackend::OpenGL) {
#ifdef HAVE_OPENCL
    if (!OpenClContext::Instance().IsInitialized() ||
        !OpenClContext::Instance().GLSharingEnabled() ||
        !EnsureSharedOpenGlContext(*platform_state_) ||
        !platform_state_->gl_context->makeCurrent(platform_state_->gl_surface.get())) {
      ReleaseDirectPresentSlot(*platform_state_, slot);
      return false;
    }

    QOpenGLFunctions* functions = platform_state_->gl_context->functions();
    functions->initializeOpenGLFunctions();
    functions->glGenTextures(1, &slot.gl_texture);
    functions->glBindTexture(GL_TEXTURE_2D, slot.gl_texture);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    functions->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT,
                            nullptr);
    functions->glBindTexture(GL_TEXTURE_2D, 0);
    functions->glFlush();
    platform_state_->gl_context->doneCurrent();

    if (slot.gl_texture == 0) {
      qWarning("RhiEditViewerSurface: failed to create OpenGL present texture %dx%d.", width,
               height);
      ReleaseDirectPresentSlot(*platform_state_, slot);
      return false;
    }

    cl_int error = CL_SUCCESS;
    slot.opencl_image =
        clCreateFromGLTexture(OpenClContext::Instance().Context(), CL_MEM_WRITE_ONLY, GL_TEXTURE_2D,
                              0, slot.gl_texture, &error);
    if (error != CL_SUCCESS || slot.opencl_image == nullptr) {
      qWarning("RhiEditViewerSurface: clCreateFromGLTexture failed with error %d.", error);
      ReleaseDirectPresentSlot(*platform_state_, slot);
      return false;
    }
#else
    return false;
#endif
  } else if (platform_state_->backend == PlatformState::DirectPresentBackend::D3D11) {
    if (!platform_state_->device) {
      return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width            = static_cast<UINT>(width);
    desc.Height           = static_cast<UINT>(height);
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags   = 0;
    desc.MiscFlags        = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    if (FAILED(platform_state_->device->CreateTexture2D(&desc, nullptr,
                                                        slot.texture.GetAddressOf()))) {
      qWarning("RhiEditViewerSurface: failed to create D3D11 present target %dx%d.", width, height);
      ReleaseDirectPresentSlot(*platform_state_, slot);
      return false;
    }

    ComPtr<IDXGIResource1> dxgi_resource;
    if (FAILED(slot.texture.As(&dxgi_resource)) || !dxgi_resource) {
      qWarning("RhiEditViewerSurface: failed to query IDXGIResource1 for D3D11 present target.");
      ReleaseDirectPresentSlot(*platform_state_, slot);
      return false;
    }

    if (FAILED(dxgi_resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                                 &slot.shared_handle)) ||
        !slot.shared_handle) {
      qWarning("RhiEditViewerSurface: CreateSharedHandle failed for D3D11 present target.");
      ReleaseDirectPresentSlot(*platform_state_, slot);
      return false;
    }

#ifdef HAVE_OPENCL
    if (platform_state_->supports_opencl_d3d11 && OpenClContext::Instance().IsInitialized()) {
      auto create_from_d3d11 = GetOpenClD3D11Extension<clCreateFromD3D11Texture2DKHR_fn>(
          OpenClContext::Instance().Platform(), "clCreateFromD3D11Texture2DKHR");
      if (create_from_d3d11 != nullptr) {
        cl_int error      = CL_SUCCESS;
        slot.opencl_image = create_from_d3d11(OpenClContext::Instance().Context(),
                                              CL_MEM_WRITE_ONLY, slot.texture.Get(), 0, &error);
        if (error != CL_SUCCESS || slot.opencl_image == nullptr) {
          qWarning("RhiEditViewerSurface: clCreateFromD3D11Texture2DKHR failed with error %d.",
                   error);
        }
      }
    }
#endif
#ifdef HAVE_CUDA
    handle_type = cudaExternalMemoryHandleTypeD3D11Resource;
    handle_size = static_cast<unsigned long long>(width) * static_cast<unsigned long long>(height) *
                  static_cast<unsigned long long>(kRgba32fPixelBytes);
#endif
  } else if (platform_state_->backend == PlatformState::DirectPresentBackend::D3D12) {
#ifdef HAVE_CUDA
    if (!platform_state_->d3d12_device) {
      return false;
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type                 = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_props.CreationNodeMask     = 1;
    heap_props.VisibleNodeMask      = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment        = 0;
    desc.Width            = static_cast<UINT64>(width);
    desc.Height           = static_cast<UINT>(height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format      = DXGI_FORMAT_R32G32B32A32_FLOAT;
    clear_value.Color[0]    = 0.0f;
    clear_value.Color[1]    = 0.0f;
    clear_value.Color[2]    = 0.0f;
    clear_value.Color[3]    = 1.0f;
    const HRESULT create_hr = platform_state_->d3d12_device->CreateCommittedResource(
        &heap_props, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, &clear_value,
        IID_PPV_ARGS(slot.resource.GetAddressOf()));
    if (FAILED(create_hr)) {
      qWarning("RhiEditViewerSurface: failed to create D3D12 present target %dx%d (hr=0x%08lx).",
               width, height, static_cast<unsigned long>(create_hr));
      ReleaseDirectPresentSlot(*platform_state_, slot);
      return false;
    }

    if (FAILED(platform_state_->d3d12_device->CreateSharedHandle(
            slot.resource.Get(), nullptr, GENERIC_ALL, nullptr, &slot.shared_handle)) ||
        !slot.shared_handle) {
      qWarning("RhiEditViewerSurface: CreateSharedHandle failed for D3D12 present target.");
      ReleaseDirectPresentSlot(*platform_state_, slot);
      return false;
    }

    const D3D12_RESOURCE_ALLOCATION_INFO allocation_info =
        platform_state_->d3d12_device->GetResourceAllocationInfo(0, 1, &desc);
    handle_type = cudaExternalMemoryHandleTypeD3D12Resource;
    handle_size = allocation_info.SizeInBytes;
#else
    return false;
#endif
  } else {
    return false;
  }

#ifdef HAVE_CUDA
  if (supportsDirectCudaPresent()) {
    const cudaExternalMemoryHandleDesc handle_desc =
        MakeDedicatedCudaExternalMemoryHandleDesc(slot.shared_handle, handle_type, handle_size);
    const cudaError_t import_err = cudaImportExternalMemory(&slot.external_memory, &handle_desc);
    if (import_err != cudaSuccess) {
      qWarning("RhiEditViewerSurface: cudaImportExternalMemory failed: %s",
               cudaGetErrorString(import_err));
      ReleaseDirectPresentSlot(*platform_state_, slot);
      return false;
    }

    cudaExternalMemoryMipmappedArrayDesc array_desc{};
    array_desc.offset     = 0;
    array_desc.formatDesc = cudaCreateChannelDesc(32, 32, 32, 32, cudaChannelFormatKindFloat);
    array_desc.extent     = cudaExtent{static_cast<size_t>(width), static_cast<size_t>(height), 0};
    array_desc.flags      = cudaArrayColorAttachment;
    array_desc.numLevels  = 1;
    const cudaError_t map_err = cudaExternalMemoryGetMappedMipmappedArray(
        &slot.mipmapped_array, slot.external_memory, &array_desc);
    if (map_err != cudaSuccess) {
      qWarning("RhiEditViewerSurface: cudaExternalMemoryGetMappedMipmappedArray failed: %s",
               cudaGetErrorString(map_err));
      ReleaseDirectPresentSlot(*platform_state_, slot);
      return false;
    }

    const cudaError_t level_err =
        cudaGetMipmappedArrayLevel(&slot.image_array, slot.mipmapped_array, 0);
    if (level_err != cudaSuccess) {
      qWarning("RhiEditViewerSurface: cudaGetMipmappedArrayLevel failed: %s",
               cudaGetErrorString(level_err));
      ReleaseDirectPresentSlot(*platform_state_, slot);
      return false;
    }
  }
#endif

  slot.width          = width;
  slot.height         = height;
  slot.texture_handle = platform_state_->backend == PlatformState::DirectPresentBackend::OpenGL
                            ? static_cast<std::uintptr_t>(slot.gl_texture)
                        : platform_state_->backend == PlatformState::DirectPresentBackend::D3D12
                            ? reinterpret_cast<std::uintptr_t>(slot.resource.Get())
                            : reinterpret_cast<std::uintptr_t>(slot.texture.Get());
  {
#ifdef HAVE_CUDA
    if (supportsDirectCudaPresent()) {
      size_t free_bytes  = 0;
      size_t total_bytes = 0;
      if (cudaMemGetInfo(&free_bytes, &total_bytes) == cudaSuccess) {
        const size_t used_bytes = total_bytes - free_bytes;
        const size_t slot_bytes =
            static_cast<size_t>(width) * static_cast<size_t>(height) * kRgba32fPixelBytes;
        const char* backend_name =
            platform_state_->backend == PlatformState::DirectPresentBackend::D3D12 ? "D3D12"
                                                                                   : "D3D11";
        qInfo(
            "[VRAM] %s present slot[%d] allocated (%dx%d, %zu MB): free=%zu MB / total=%zu MB "
            "(used=%zu MB)",
            backend_name, slot_index, width, height, slot_bytes >> 20, free_bytes >> 20,
            total_bytes >> 20, used_bytes >> 20);
      }
    } else {
      const size_t slot_bytes =
          static_cast<size_t>(width) * static_cast<size_t>(height) * kRgba32fPixelBytes;
      qInfo("[VRAM] OpenCL/OpenGL present slot[%d] allocated (%dx%d, %zu MB).", slot_index, width,
            height, slot_bytes >> 20);
    }
#else
    const size_t slot_bytes =
        static_cast<size_t>(width) * static_cast<size_t>(height) * kRgba32fPixelBytes;
    qInfo("[VRAM] OpenCL/OpenGL present slot[%d] allocated (%dx%d, %zu MB).", slot_index, width,
          height, slot_bytes >> 20);
#endif
  }
  return HasDirectPresentResource(slot, platform_state_->backend);
#else
  (void)slot_index;
  (void)width;
  (void)height;
  return false;
#endif
}

void RhiEditViewerSurface::initialize(QRhiCommandBuffer* command_buffer) {
  applyDisplayConfig();
  renderer_.initialize(rhi(), renderTarget(), command_buffer);

#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
  platform_state_->supports_direct_present = false;
#ifdef HAVE_CUDA
  platform_state_->cuda_device = -1;
#endif
#ifdef HAVE_OPENCL
  platform_state_->supports_opencl_d3d11 = false;
#endif
  platform_state_->device       = nullptr;
  platform_state_->d3d12_device = nullptr;
  platform_state_->d3d12_queue  = nullptr;
  platform_state_->backend      = PlatformState::DirectPresentBackend::None;
  const char* backend_name      = "<none>";
  if (rhi()) {
    switch (rhi()->backend()) {
      case QRhi::D3D11:
        backend_name = "D3D11";
        break;
      case QRhi::D3D12:
        backend_name = "D3D12";
        break;
      case QRhi::Vulkan:
        backend_name = "Vulkan";
        break;
      case QRhi::OpenGLES2:
        backend_name = "OpenGLES2";
        break;
      case QRhi::Metal:
        backend_name = "Metal";
        break;
      case QRhi::Null:
        backend_name = "Null";
        break;
      default:
        backend_name = "<other>";
        break;
    }
  }
  bool cuda_bind_ok   = false;
  bool opencl_bind_ok = false;
  if (rhi() && rhi()->backend() == QRhi::D3D11) {
    const auto* native_handles = static_cast<const QRhiD3D11NativeHandles*>(rhi()->nativeHandles());
    platform_state_->device =
        native_handles ? static_cast<ID3D11Device*>(native_handles->dev) : nullptr;
#ifdef HAVE_CUDA
    platform_state_->cuda_device =
        platform_state_->device ? ResolveCudaDeviceForD3D11Device(platform_state_->device) : -1;
    cuda_bind_ok = platform_state_->device && platform_state_->cuda_device >= 0 &&
                   BindCudaDeviceOnCurrentThread(platform_state_->cuda_device, "initialize");
#endif
    platform_state_->supports_direct_present = cuda_bind_ok;
    if (platform_state_->supports_direct_present) {
      platform_state_->backend = PlatformState::DirectPresentBackend::D3D11;
    }
  } else if (rhi() && rhi()->backend() == QRhi::OpenGLES2) {
#ifdef HAVE_OPENCL
    opencl_bind_ok = OpenClContext::Instance().IsInitialized() &&
                     OpenClContext::Instance().GLSharingEnabled() &&
                     EnsureSharedOpenGlContext(*platform_state_);
    platform_state_->supports_direct_present = opencl_bind_ok;
    if (opencl_bind_ok) {
      platform_state_->backend = PlatformState::DirectPresentBackend::OpenGL;
    }
#endif
  } else if (rhi() && rhi()->backend() == QRhi::D3D12) {
#ifdef HAVE_CUDA
    const auto* native_handles = static_cast<const QRhiD3D12NativeHandles*>(rhi()->nativeHandles());
    platform_state_->d3d12_device =
        native_handles ? static_cast<ID3D12Device*>(native_handles->dev) : nullptr;
    platform_state_->d3d12_queue =
        native_handles ? static_cast<ID3D12CommandQueue*>(native_handles->commandQueue) : nullptr;
    platform_state_->cuda_device =
        platform_state_->d3d12_device
            ? ResolveCudaDeviceForD3D12Device(platform_state_->d3d12_device)
            : -1;
    cuda_bind_ok = platform_state_->d3d12_device && platform_state_->d3d12_queue &&
                   platform_state_->cuda_device >= 0 &&
                   BindCudaDeviceOnCurrentThread(platform_state_->cuda_device, "initialize");
    platform_state_->supports_direct_present = cuda_bind_ok;
    if (cuda_bind_ok) {
      platform_state_->backend = PlatformState::DirectPresentBackend::D3D12;
    }
#endif
  }
  qInfo(
      "[DirectPresent] RHI backend=%s, d3d11_device=%p, d3d12_device=%p, d3d12_queue=%p, "
      "cuda_bind_ok=%d, opencl_bind_ok=%d, supports_direct_present=%d",
      backend_name, static_cast<void*>(platform_state_->device),
      static_cast<void*>(platform_state_->d3d12_device),
      static_cast<void*>(platform_state_->d3d12_queue), cuda_bind_ok ? 1 : 0,
      opencl_bind_ok ? 1 : 0, platform_state_->supports_direct_present ? 1 : 0);
#else
  qInfo("[DirectPresent] GPU/D3D interop unavailable at compile time.");
#endif
}

void RhiEditViewerSurface::render(QRhiCommandBuffer* command_buffer) {
  if (display_config_dirty_) {
    applyDisplayConfig();
  }

#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
  if (supportsDirectGpuPresent()) {
    ImportedTextureFrame direct_present_frame{};
    bool                 has_pending_frame       = false;
    bool                 has_more_pending_frames = false;
    {
      std::lock_guard<std::mutex> lock(platform_state_->mutex);
      const auto                  pending_slot_opt = platform_state_->pending_frames.PopNextSlot();
      const int                   pending_slot     = pending_slot_opt.value_or(-1);
      int                         slot_to_show     = platform_state_->active_idx;
      if (IsValidSlotIndex(pending_slot, platform_state_->targets.size())) {
        slot_to_show                = pending_slot;
        platform_state_->active_idx = pending_slot;
        platform_state_->targets[pending_slot].active_cuda_signal_value =
            platform_state_->targets[pending_slot].ready_cuda_signal_value;
        platform_state_->targets[pending_slot].ready_cuda_signal_value = 0;
        std::array<DirectPresentSlotAvailability, kDirectPresentSlotCount> slot_infos{};
        for (size_t i = 0; i < platform_state_->targets.size(); ++i) {
          const auto& target = platform_state_->targets[i];
          slot_infos[i]      = DirectPresentSlotAvailability{
              target.width,
              target.height,
              HasDirectPresentResource(target, platform_state_->backend),
              static_cast<int>(i) == platform_state_->active_idx ||
                  platform_state_->pending_frames.ContainsSlot(static_cast<int>(i)) ||
                  static_cast<int>(i) == platform_state_->ready_slot_idx ||
                  static_cast<int>(i) == platform_state_->mapped_slot_idx,
          };
        }
        const int preferred_write_slot =
            (pending_slot + 1) % static_cast<int>(platform_state_->targets.size());
        const int target_width =
            slot_to_show >= 0 ? platform_state_->targets[slot_to_show].width : 0;
        const int target_height =
            slot_to_show >= 0 ? platform_state_->targets[slot_to_show].height : 0;
        platform_state_->write_idx =
            SelectDirectPresentWriteSlot(slot_infos.data(), slot_infos.size(), preferred_write_slot,
                                         target_width, target_height)
                .slot_index;
        platform_state_->active_presentation_mode.store(
            platform_state_->targets[pending_slot].presentation_mode, std::memory_order_release);
        platform_state_->active_preview_metadata =
            platform_state_->targets[pending_slot].preview_metadata;
      }
      has_more_pending_frames = !platform_state_->pending_frames.Empty();

      const auto& slot        = platform_state_->targets[slot_to_show];
      if (HasDirectPresentResource(slot, platform_state_->backend) && slot.width > 0 &&
          slot.height > 0 && IsValidSlotIndex(pending_slot, platform_state_->targets.size())) {
        if (platform_state_->backend == PlatformState::DirectPresentBackend::D3D12 &&
#ifdef HAVE_CUDA
            platform_state_->d3d12_queue && platform_state_->d3d12_cuda_fence &&
            slot.active_cuda_signal_value != 0
#else
            false
#endif
        ) {
          const HRESULT wait_hr = platform_state_->d3d12_queue->Wait(
              platform_state_->d3d12_cuda_fence.Get(), slot.active_cuda_signal_value);
          if (FAILED(wait_hr)) {
            qWarning(
                "RhiEditViewerSurface: D3D12 queue wait for CUDA fence value %llu failed "
                "(hr=0x%08lx).",
                static_cast<unsigned long long>(slot.active_cuda_signal_value),
                static_cast<unsigned long>(wait_hr));
          }
        }
        direct_present_frame.width          = slot.width;
        direct_present_frame.height         = slot.height;
        direct_present_frame.texture_handle = slot.texture_handle;
#if defined(Q_OS_WIN)
        direct_present_frame.native_layout =
            platform_state_->backend == PlatformState::DirectPresentBackend::D3D12
                ? static_cast<int>(D3D12_RESOURCE_STATE_COMMON)
                : 0;
#endif
        direct_present_frame.presentation_mode = slot.presentation_mode;
        direct_present_frame.preview_metadata  = slot.preview_metadata;
        has_pending_frame                      = true;
      }
    }

    if (has_pending_frame) {
      renderer_.queueImportedFrame(direct_present_frame);
    }
    if (has_more_pending_frames) {
      update();
    }
  }
#endif

  renderer_.render(command_buffer, renderTarget(), view_state_,
                   ViewportWidgetInfo{width(), height(), static_cast<float>(devicePixelRatioF())});

#if defined(Q_OS_WIN) && defined(HAVE_CUDA)
  if (supportsDirectGpuPresent() &&
      platform_state_->backend == PlatformState::DirectPresentBackend::D3D12) {
    std::lock_guard<std::mutex> lock(platform_state_->mutex);
    auto&                       slot = platform_state_->targets[platform_state_->active_idx];
    if (slot.resource) {
      slot.d3d12_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
  }
#endif
}

void RhiEditViewerSurface::releaseResources() {
  renderer_.releaseResources();
  releasePlatformTargets();
}

void RhiEditViewerSurface::applyDisplayConfig() {
  QWidget* host_window = window();
  if (!host_window) {
    return;
  }

  const auto native_handle = reinterpret_cast<void*>(host_window->effectiveWinId());
  if (!native_handle) {
    return;
  }

  if (ColorManager::ApplyWindowColorSpace(native_handle, display_config_)) {
    display_config_dirty_ = false;
  }
}

void RhiEditViewerSurface::releasePlatformTargets() {
#if defined(Q_OS_WIN) && (defined(HAVE_CUDA) || defined(HAVE_OPENCL))
  if (!platform_state_) {
    return;
  }

#ifdef HAVE_CUDA
  if (platform_state_->cuda_device >= 0) {
    (void)BindCudaDeviceOnCurrentThread(platform_state_->cuda_device, "releasePlatformTargets");
  }
#endif

  std::lock_guard<std::mutex> lock(platform_state_->mutex);
  for (auto& slot : platform_state_->targets) {
    ReleaseDirectPresentSlot(*platform_state_, slot);
  }
  platform_state_->supports_direct_present = false;
  platform_state_->device                  = nullptr;
  platform_state_->d3d12_device            = nullptr;
  platform_state_->d3d12_queue             = nullptr;
  platform_state_->d3d12_transition_list.Reset();
  platform_state_->d3d12_transition_allocator.Reset();
  platform_state_->d3d12_transition_fence.Reset();
  platform_state_->gl_surface.reset();
  platform_state_->gl_context.reset();
#ifdef HAVE_CUDA
  platform_state_->d3d12_cuda_fence.Reset();
  if (platform_state_->cuda_signal_semaphore) {
    cudaDestroyExternalSemaphore(platform_state_->cuda_signal_semaphore);
    platform_state_->cuda_signal_semaphore = nullptr;
  }
  if (platform_state_->d3d12_cuda_fence_shared_handle) {
    CloseHandle(platform_state_->d3d12_cuda_fence_shared_handle);
    platform_state_->d3d12_cuda_fence_shared_handle = nullptr;
  }
#endif
  if (platform_state_->d3d12_transition_event) {
    CloseHandle(platform_state_->d3d12_transition_event);
    platform_state_->d3d12_transition_event = nullptr;
  }
  platform_state_->d3d12_transition_fence_value = 0;
#ifdef HAVE_CUDA
  platform_state_->d3d12_cuda_fence_value = 0;
#endif
  platform_state_->backend = PlatformState::DirectPresentBackend::None;
#ifdef HAVE_CUDA
  platform_state_->cuda_device = -1;
#endif
#ifdef HAVE_OPENCL
  platform_state_->supports_opencl_d3d11 = false;
#endif
  platform_state_->pending_frames.Clear();
  platform_state_->active_presentation_mode.store(FramePresentationMode::FullFrame,
                                                  std::memory_order_release);
  platform_state_->pending_presentation_mode.store(FramePresentationMode::FullFrame,
                                                   std::memory_order_release);
  platform_state_->pending_presentation_mode_valid.store(false, std::memory_order_release);
  platform_state_->active_idx                     = 0;
  platform_state_->write_idx                      = 1;
  platform_state_->render_target_idx              = 0;
  platform_state_->mapped_slot_idx                = -1;
  platform_state_->ready_slot_idx                 = -1;
  platform_state_->active_preview_metadata        = {};
  platform_state_->pending_preview_metadata       = {};
  platform_state_->pending_preview_metadata_valid = false;
#endif
}

}  // namespace alcedo
