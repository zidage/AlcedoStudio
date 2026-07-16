//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/harness_viewport_renderer.hpp"

#include "ui/editor_rhi/native_resource_counters.hpp"

#include <QFile>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>
#include <QtGui/rhi/qrhi_platform.h>

#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#include <algorithm>
#include <cstring>

#if defined(HAVE_OPENCL)
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"
#include <CL/cl_gl.h>
#endif

#if defined(_WIN32) && defined(HAVE_CUDA)
#include "ui/editor_rhi/cuda_adapter_discovery.hpp"
#include "ui/edit_viewer/d3d_cuda_interop_utils.hpp"
#endif

namespace alcedo::editor_rhi {
namespace {

constexpr const char* kVertexShaderResource   = ":/shaders/editor_rhi/harness_image.vert.qsb";
constexpr const char* kFragmentShaderResource = ":/shaders/editor_rhi/harness_image.frag.qsb";
constexpr size_t      kRgba32fPixelBytes      = sizeof(float) * 4U;

struct Vertex {
  float x, y, u, v;
};

auto LoadShader(const char* resource_path) -> QShader {
  QFile shader_file(QString::fromUtf8(resource_path));
  if (!shader_file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return QShader::fromSerialized(shader_file.readAll());
}

template <typename T>
void DestroyRhi(T*& resource) {
  if (!resource) {
    return;
  }
  resource->destroy();
  delete resource;
  resource = nullptr;
}

}  // namespace

HarnessViewportRenderer::HarnessViewportRenderer() = default;

HarnessViewportRenderer::~HarnessViewportRenderer() {
  releaseSharedTarget();
  releasePipelineResources();
}

void HarnessViewportRenderer::releasePipelineResources() {
  if (imported_texture_) {
    NativeResourceCounters::Instance().OnDestroyImportedQRhiTexture();
  }
  DestroyRhi(pipeline_);
  DestroyRhi(srb_);
  DestroyRhi(sampler_);
  DestroyRhi(ubuf_);
  DestroyRhi(vbuf_);
  DestroyRhi(imported_texture_);
  static_ready_ = false;
}

void HarnessViewportRenderer::releaseSharedTarget() {
#if defined(_WIN32) && defined(HAVE_CUDA)
  if (present_.image_array || present_.mipmapped_array || present_.external_memory ||
      present_.d3d11_texture || present_.shared_handle) {
    if (present_.mipmapped_array) {
      cudaFreeMipmappedArray(present_.mipmapped_array);
      present_.mipmapped_array = nullptr;
    }
    present_.image_array = nullptr;
    if (present_.external_memory) {
      cudaDestroyExternalMemory(present_.external_memory);
      present_.external_memory = nullptr;
      NativeResourceCounters::Instance().OnDestroyExternalMemory();
    }
    if (present_.shared_handle) {
      CloseHandle(present_.shared_handle);
      present_.shared_handle = nullptr;
    }
    if (present_.d3d11_texture) {
      present_.d3d11_texture.Reset();
      NativeResourceCounters::Instance().OnDestroySharedTexture();
    }
  }
#endif
#if defined(HAVE_OPENCL)
  if (present_.opencl_image) {
    clReleaseMemObject(present_.opencl_image);
    present_.opencl_image = nullptr;
    NativeResourceCounters::Instance().OnDestroyOpenClImage();
  }
  if (present_.gl_texture != 0) {
    if (QOpenGLContext::currentContext()) {
      QOpenGLFunctions* functions = QOpenGLContext::currentContext()->functions();
      functions->glDeleteTextures(1, &present_.gl_texture);
    }
    present_.gl_texture = 0;
    NativeResourceCounters::Instance().OnDestroySharedTexture();
  }
#endif
  present_.width          = 0;
  present_.height         = 0;
  present_.texture_handle = 0;
  present_.lifetime_token.reset();
  completed_lease_ = {};
}

void HarnessViewportRenderer::initialize(QRhiCommandBuffer* /*cb*/) {
  QRhi* r = rhi();
  if (!r) {
    return;
  }

#if defined(_WIN32) && defined(HAVE_CUDA)
  d3d11_device_ = nullptr;
  cuda_device_  = -1;
  if (r->backend() == QRhi::D3D11) {
    const auto* native = static_cast<const QRhiD3D11NativeHandles*>(r->nativeHandles());
    d3d11_device_      = native ? static_cast<ID3D11Device*>(native->dev) : nullptr;
    if (d3d11_device_) {
      int current = -1;
      if (cudaGetDevice(&current) == cudaSuccess && current >= 0 &&
          D3D11DeviceMatchesCudaDevice(d3d11_device_, current)) {
        cuda_device_ = current;
        (void)BindCudaDeviceOnCurrentThread(cuda_device_, "HarnessViewportRenderer::initialize");
      } else {
        const auto discovery = DiscoverCudaAdapters();
        if (discovery.preferred.has_value() &&
            D3D11DeviceMatchesCudaDevice(d3d11_device_, discovery.preferred->device_index)) {
          cuda_device_ = discovery.preferred->device_index;
          (void)BindCudaDeviceOnCurrentThread(cuda_device_, "HarnessViewportRenderer::initialize");
        }
      }
    }
  }
#endif

  releasePipelineResources();
  static_ready_ = false;
}

void HarnessViewportRenderer::synchronize(QQuickRhiItem* item) {
  item_ = qobject_cast<HarnessViewportItem*>(item);
  if (!item_) {
    return;
  }
  backend_          = item_->backend_;
  fixture_kind_     = item_->fixture_kind_;
  expected_         = item_->expected_;
  request_readback_ = item_->request_readback_;

  if (item_->invalidate_request_.exchange(false)) {
    releaseSharedTarget();
    releasePipelineResources();
  }
}

auto HarnessViewportRenderer::ensureSharedTarget(int width, int height) -> bool {
  if (width <= 0 || height <= 0) {
    return false;
  }
  if (present_.width == width && present_.height == height && present_.texture_handle != 0) {
    return true;
  }

  releaseSharedTarget();

  if (backend_ == EditorBackend::Cuda) {
#if defined(_WIN32) && defined(HAVE_CUDA)
    if (!d3d11_device_ || cuda_device_ < 0) {
      if (item_) {
        item_->setLastError(QStringLiteral("CUDA/D3D11 device not available on render thread"));
      }
      return false;
    }
    if (!BindCudaDeviceOnCurrentThread(cuda_device_, "ensureSharedTarget")) {
      if (item_) {
        item_->setLastError(QStringLiteral("cudaSetDevice failed on render thread"));
      }
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
    desc.MiscFlags        = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr,
                                              present_.d3d11_texture.GetAddressOf()))) {
      if (item_) {
        item_->setLastError(QStringLiteral("CreateTexture2D failed for shared present target"));
      }
      return false;
    }
    NativeResourceCounters::Instance().OnCreateSharedTexture();

    Microsoft::WRL::ComPtr<IDXGIResource1> dxgi_resource;
    if (FAILED(present_.d3d11_texture.As(&dxgi_resource)) || !dxgi_resource) {
      releaseSharedTarget();
      return false;
    }
    if (FAILED(dxgi_resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                                 &present_.shared_handle)) ||
        !present_.shared_handle) {
      releaseSharedTarget();
      return false;
    }

    const unsigned long long handle_size =
        static_cast<unsigned long long>(width) * static_cast<unsigned long long>(height) *
        static_cast<unsigned long long>(kRgba32fPixelBytes);
    const cudaExternalMemoryHandleDesc handle_desc = MakeDedicatedCudaExternalMemoryHandleDesc(
        present_.shared_handle, cudaExternalMemoryHandleTypeD3D11Resource, handle_size);
    if (cudaImportExternalMemory(&present_.external_memory, &handle_desc) != cudaSuccess) {
      if (item_) {
        item_->setLastError(QStringLiteral("cudaImportExternalMemory failed"));
      }
      releaseSharedTarget();
      return false;
    }
    NativeResourceCounters::Instance().OnCreateExternalMemory();

    cudaExternalMemoryMipmappedArrayDesc array_desc{};
    array_desc.offset     = 0;
    array_desc.formatDesc = cudaCreateChannelDesc(32, 32, 32, 32, cudaChannelFormatKindFloat);
    array_desc.extent     = cudaExtent{static_cast<size_t>(width), static_cast<size_t>(height), 0};
    array_desc.flags      = cudaArrayColorAttachment;
    array_desc.numLevels  = 1;
    if (cudaExternalMemoryGetMappedMipmappedArray(&present_.mipmapped_array,
                                                  present_.external_memory, &array_desc) !=
        cudaSuccess) {
      releaseSharedTarget();
      return false;
    }
    if (cudaGetMipmappedArrayLevel(&present_.image_array, present_.mipmapped_array, 0) !=
        cudaSuccess) {
      releaseSharedTarget();
      return false;
    }

    present_.texture_handle = reinterpret_cast<std::uintptr_t>(present_.d3d11_texture.Get());
    present_.width          = width;
    present_.height         = height;
    present_.lifetime_token = std::shared_ptr<const void>(
        reinterpret_cast<const void*>(present_.texture_handle), [](const void*) {});
    return true;
#else
    return false;
#endif
  }

  if (backend_ == EditorBackend::OpenCl) {
#if defined(HAVE_OPENCL)
    if (!OpenClContext::Instance().IsInitialized() ||
        !OpenClContext::Instance().GLSharingEnabled()) {
      if (item_) {
        item_->setLastError(QStringLiteral("OpenCL GL sharing is not initialized"));
      }
      return false;
    }
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (!ctx) {
      if (item_) {
        item_->setLastError(QStringLiteral("no current OpenGL context on render thread"));
      }
      return false;
    }
    QOpenGLFunctions* functions = ctx->functions();
    functions->initializeOpenGLFunctions();
    functions->glGenTextures(1, &present_.gl_texture);
    functions->glBindTexture(GL_TEXTURE_2D, present_.gl_texture);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    functions->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT,
                            nullptr);
    functions->glBindTexture(GL_TEXTURE_2D, 0);
    functions->glFlush();
    if (present_.gl_texture == 0) {
      return false;
    }
    NativeResourceCounters::Instance().OnCreateSharedTexture();

    cl_int error = CL_SUCCESS;
    present_.opencl_image =
        clCreateFromGLTexture(OpenClContext::Instance().Context(), CL_MEM_WRITE_ONLY, GL_TEXTURE_2D,
                              0, present_.gl_texture, &error);
    if (error != CL_SUCCESS || present_.opencl_image == nullptr) {
      if (item_) {
        item_->setLastError(
            QStringLiteral("clCreateFromGLTexture failed with error %1").arg(error));
      }
      releaseSharedTarget();
      return false;
    }
    NativeResourceCounters::Instance().OnCreateOpenClImage();

    present_.texture_handle = static_cast<std::uintptr_t>(present_.gl_texture);
    present_.width          = width;
    present_.height         = height;
    present_.lifetime_token = std::shared_ptr<const void>(
        reinterpret_cast<const void*>(present_.texture_handle), [](const void*) {});
    return true;
#else
    return false;
#endif
  }

  if (item_) {
    item_->setLastError(QStringLiteral("Metal shared-texture path is Phase 8; not implemented"));
  }
  return false;
}

auto HarnessViewportRenderer::fillSharedTargetWithFixture() -> bool {
  if (!present_.texture_handle || expected_.pixels.empty()) {
    return false;
  }

  if (backend_ == EditorBackend::Cuda) {
#if defined(_WIN32) && defined(HAVE_CUDA)
    if (!present_.image_array || cuda_device_ < 0) {
      return false;
    }
    if (!BindCudaDeviceOnCurrentThread(cuda_device_, "fillSharedTargetWithFixture")) {
      return false;
    }
    const size_t row_bytes = expected_.row_bytes();
    const cudaError_t err  = cudaMemcpy2DToArray(
        present_.image_array, 0, 0, expected_.data(), row_bytes, row_bytes,
        static_cast<size_t>(expected_.height), cudaMemcpyHostToDevice);
    if (err != cudaSuccess) {
      if (item_) {
        item_->setLastError(
            QStringLiteral("cudaMemcpy2DToArray failed: %1").arg(cudaGetErrorString(err)));
      }
      return false;
    }
    cudaDeviceSynchronize();
    return true;
#else
    return false;
#endif
  }

  if (backend_ == EditorBackend::OpenCl) {
#if defined(HAVE_OPENCL)
    if (!present_.opencl_image) {
      return false;
    }
    cl_command_queue queue = OpenClContext::Instance().Queue();
    cl_int           error = clEnqueueAcquireGLObjects(queue, 1, &present_.opencl_image, 0,
                                                       nullptr, nullptr);
    if (error != CL_SUCCESS) {
      if (item_) {
        item_->setLastError(
            QStringLiteral("clEnqueueAcquireGLObjects failed: %1").arg(error));
      }
      return false;
    }
    const size_t origin[3] = {0, 0, 0};
    const size_t region[3] = {static_cast<size_t>(expected_.width),
                              static_cast<size_t>(expected_.height), 1};
    error = clEnqueueWriteImage(queue, present_.opencl_image, CL_TRUE, origin, region, 0, 0,
                                expected_.data(), 0, nullptr, nullptr);
    if (error != CL_SUCCESS) {
      clEnqueueReleaseGLObjects(queue, 1, &present_.opencl_image, 0, nullptr, nullptr);
      if (item_) {
        item_->setLastError(QStringLiteral("clEnqueueWriteImage failed: %1").arg(error));
      }
      return false;
    }
    error = clEnqueueReleaseGLObjects(queue, 1, &present_.opencl_image, 0, nullptr, nullptr);
    if (error != CL_SUCCESS) {
      if (item_) {
        item_->setLastError(
            QStringLiteral("clEnqueueReleaseGLObjects failed: %1").arg(error));
      }
      return false;
    }
    clFinish(queue);
    if (QOpenGLContext::currentContext()) {
      QOpenGLContext::currentContext()->functions()->glFlush();
    }
    return true;
#else
    return false;
#endif
  }
  return false;
}

auto HarnessViewportRenderer::ensureImportedTexture() -> bool {
  QRhi* r = rhi();
  if (!r || present_.texture_handle == 0) {
    return false;
  }

  if (imported_texture_ && imported_texture_->pixelSize() == QSize(present_.width, present_.height)) {
    // recreate if handle changed
  }

  if (imported_texture_) {
    NativeResourceCounters::Instance().OnDestroyImportedQRhiTexture();
    DestroyRhi(imported_texture_);
    DestroyRhi(srb_);
  }

  imported_texture_ =
      r->newTexture(QRhiTexture::RGBA32F, QSize(present_.width, present_.height), 1);
  if (!imported_texture_->createFrom(
          {static_cast<quint64>(present_.texture_handle), 0})) {
    if (item_) {
      item_->setLastError(QStringLiteral("QRhiTexture::createFrom failed for native handle"));
    }
    DestroyRhi(imported_texture_);
    return false;
  }
  NativeResourceCounters::Instance().OnCreateImportedQRhiTexture();
  return true;
}

auto HarnessViewportRenderer::ensureDrawPipeline(QRhiRenderTarget* rt) -> bool {
  QRhi* r = rhi();
  if (!r || !rt || !imported_texture_) {
    return false;
  }

  if (!vs_.isValid() || !fs_.isValid()) {
    vs_ = LoadShader(kVertexShaderResource);
    fs_ = LoadShader(kFragmentShaderResource);
    if (!vs_.isValid() || !fs_.isValid()) {
      if (item_) {
        item_->setLastError(QStringLiteral("failed to load harness shaders"));
      }
      return false;
    }
  }

  if (!vbuf_) {
    vbuf_ = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, sizeof(Vertex) * 4);
    if (!vbuf_->create()) {
      DestroyRhi(vbuf_);
      return false;
    }
  }

  if (!sampler_) {
    sampler_ = r->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                             QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    if (!sampler_->create()) {
      DestroyRhi(sampler_);
      return false;
    }
  }

  if (!srb_) {
    srb_ = r->newShaderResourceBindings();
    srb_->setBindings(
        {QRhiShaderResourceBinding::sampledTexture(0, QRhiShaderResourceBinding::FragmentStage,
                                                   imported_texture_, sampler_)});
    if (!srb_->create()) {
      DestroyRhi(srb_);
      return false;
    }
  }

  if (!pipeline_) {
    pipeline_ = r->newGraphicsPipeline();
    QRhiVertexInputLayout input_layout;
    input_layout.setBindings({QRhiVertexInputBinding(sizeof(Vertex))});
    input_layout.setAttributes(
        {QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
         QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float))});
    pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipeline_->setCullMode(QRhiGraphicsPipeline::None);
    pipeline_->setSampleCount(rt->sampleCount());
    pipeline_->setShaderStages({QRhiShaderStage(QRhiShaderStage::Vertex, vs_),
                                QRhiShaderStage(QRhiShaderStage::Fragment, fs_)});
    pipeline_->setVertexInputLayout(input_layout);
    pipeline_->setShaderResourceBindings(srb_);
    pipeline_->setRenderPassDescriptor(rt->renderPassDescriptor());
    if (!pipeline_->create()) {
      DestroyRhi(pipeline_);
      return false;
    }
  }
  static_ready_ = true;
  return true;
}

void HarnessViewportRenderer::requestReadbackIfNeeded(QRhiCommandBuffer* cb) {
  if (!request_readback_ || readback_done_ || !item_) {
    return;
  }
  QRhiTexture* color = colorTexture();
  if (!color || !rhi()) {
    return;
  }

  QRhiReadbackDescription desc(color);
  QRhiReadbackResult*     result = new QRhiReadbackResult;
  result->completed = [this, result]() {
    if (!item_ || result->data.isEmpty()) {
      delete result;
      return;
    }
    HarnessFixtureImage image;
    image.width  = result->pixelSize.width();
    image.height = result->pixelSize.height();
    const auto pixel_count =
        static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    image.pixels.resize(pixel_count);
    const auto expected_bytes = pixel_count * sizeof(Rgba32fPixel);
    const auto copy_bytes =
        std::min(expected_bytes, static_cast<std::size_t>(result->data.size()));
    std::memcpy(image.data(), result->data.constData(), copy_bytes);
    item_->publishReadback(std::move(image));
    readback_done_ = true;
    delete result;
  };

  QRhiResourceUpdateBatch* batch = rhi()->nextResourceUpdateBatch();
  batch->readBackTexture(desc, result);
  cb->resourceUpdate(batch);
  readback_queued_ = true;
}

void HarnessViewportRenderer::render(QRhiCommandBuffer* cb) {
  QRhiRenderTarget* rt = renderTarget();
  QRhi*             r  = rhi();
  if (!cb || !rt || !r || !item_) {
    return;
  }

  const QSize pixel_size = rt->pixelSize();
  const int   width =
      expected_.width > 0 ? expected_.width : std::max(1, pixel_size.width());
  const int height =
      expected_.height > 0 ? expected_.height : std::max(1, pixel_size.height());

  if (!ensureSharedTarget(width, height)) {
    item_->setPresentationOk(false);
    item_->setStatusText(QStringLiteral("shared target failed"));
    cb->beginPass(rt, Qt::black, {1.0f, 0});
    cb->endPass();
    return;
  }

  if (!fillSharedTargetWithFixture()) {
    item_->setPresentationOk(false);
    item_->setStatusText(QStringLiteral("fixture fill failed"));
    cb->beginPass(rt, Qt::black, {1.0f, 0});
    cb->endPass();
    return;
  }

  // Publish completed lease (producer side done).
  completed_lease_.target.backend       = backend_;
  completed_lease_.target.handle_kind   = LeaseHandleKindForBackend(backend_);
  completed_lease_.target.pixel_format  = LeasePixelFormat::Rgba32f;
  completed_lease_.target.dimensions    = {present_.width, present_.height};
  completed_lease_.target.generation    = {1, 1, 1};
  completed_lease_.target.native_handle = present_.texture_handle;
  completed_lease_.target.lifetime_token = present_.lifetime_token;
  completed_lease_.generation           = completed_lease_.target.generation;
  completed_lease_.producer_complete    = true;

  if (!ensureImportedTexture() || !ensureDrawPipeline(rt)) {
    item_->setPresentationOk(false);
    item_->setStatusText(QStringLiteral("import/pipeline failed"));
    cb->beginPass(rt, Qt::black, {1.0f, 0});
    cb->endPass();
    return;
  }

  // UV: OpenGL import is bottom-up in RHI sampling space for GL textures; D3D11
  // is top-down. Flip V for OpenCL/OpenGL so pre-composition readback matches
  // the host fixture row order used by MaxAbsPixelError.
  const bool flip_v = (backend_ == EditorBackend::OpenCl);
  const float v0    = flip_v ? 0.0f : 1.0f;
  const float v1    = flip_v ? 1.0f : 0.0f;
  const Vertex kVerts[] = {
      {-1.0f, -1.0f, 0.0f, v0},
      {1.0f, -1.0f, 1.0f, v0},
      {-1.0f, 1.0f, 0.0f, v1},
      {1.0f, 1.0f, 1.0f, v1},
  };

  QRhiResourceUpdateBatch* updates = r->nextResourceUpdateBatch();
  updates->updateDynamicBuffer(vbuf_, 0, sizeof(kVerts), kVerts);

  cb->beginPass(rt, Qt::black, {1.0f, 0}, updates);
  if (pipeline_ && srb_ && vbuf_) {
    const QRhiCommandBuffer::VertexInput vinput[] = {{vbuf_, 0}};
    cb->setGraphicsPipeline(pipeline_);
    cb->setViewport(QRhiViewport(0, 0, pixel_size.width(), pixel_size.height()));
    cb->setShaderResources(srb_);
    cb->setVertexInput(0, 1, vinput);
    cb->draw(4);
  }
  cb->endPass();

  // Dual-sided release bookkeeping: producer already complete; renderer complete after draw.
  LeaseReleaseState release;
  release.target_generation  = completed_lease_.generation.target_generation;
  release.producer_complete  = true;
  release.renderer_complete  = true;
  release.lifetime_token     = present_.lifetime_token;
  (void)release.can_destroy();  // target remains for subsequent frames until teardown

  item_->setPresentationOk(true);
  item_->setStatusText(QStringLiteral("presented"));
  item_->noteFramePresented();

  requestReadbackIfNeeded(cb);
  update();
}

}  // namespace alcedo::editor_rhi
