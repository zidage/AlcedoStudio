//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QQuickRhiItem>
#include <QtGui/rhi/qrhi.h>
#include <QtGui/rhi/qshader.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/frame_presentation_lease.hpp"
#include "ui/editor_rhi/harness_fixtures.hpp"
#include "ui/editor_rhi/harness_viewport_item.hpp"

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
#endif

#if defined(HAVE_OPENCL)
#include <CL/cl.h>
#endif

namespace alcedo::editor_rhi {

class HarnessViewportRenderer final : public QQuickRhiItemRenderer {
 public:
  HarnessViewportRenderer();
  ~HarnessViewportRenderer() override;

 protected:
  void initialize(QRhiCommandBuffer* cb) override;
  void synchronize(QQuickRhiItem* item) override;
  void render(QRhiCommandBuffer* cb) override;

 private:
  struct SharedPresentTarget {
    int  width  = 0;
    int  height = 0;
    std::uintptr_t texture_handle = 0;
    std::shared_ptr<const void> lifetime_token{};

#if defined(_WIN32) && defined(HAVE_CUDA)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11_texture;
    HANDLE                                  shared_handle    = nullptr;
    cudaExternalMemory_t                    external_memory  = nullptr;
    cudaMipmappedArray_t                    mipmapped_array  = nullptr;
    cudaArray_t                             image_array      = nullptr;
#endif
#if defined(HAVE_OPENCL)
    unsigned int gl_texture   = 0;
    cl_mem       opencl_image = nullptr;
#endif
  };

  void releaseSharedTarget();
  void releasePipelineResources();
  auto ensureSharedTarget(int width, int height) -> bool;
  auto fillSharedTargetWithFixture() -> bool;
  auto ensureImportedTexture() -> bool;
  auto ensureDrawPipeline(QRhiRenderTarget* rt) -> bool;
  void requestReadbackIfNeeded(QRhiCommandBuffer* cb);

  HarnessViewportItem* item_ = nullptr;
  EditorBackend        backend_ = EditorBackend::Cuda;
  HarnessFixtureKind   fixture_kind_ = HarnessFixtureKind::Fp32Gradient;
  HarnessFixtureImage  expected_{};
  bool                 request_readback_ = true;
  bool                 readback_queued_  = false;
  bool                 readback_done_    = false;

  SharedPresentTarget present_;
  CompletedFrameLease completed_lease_{};

  QRhiTexture*                imported_texture_ = nullptr;
  QRhiBuffer*                 vbuf_             = nullptr;
  QRhiBuffer*                 ubuf_             = nullptr;
  QRhiSampler*                sampler_          = nullptr;
  QRhiShaderResourceBindings* srb_              = nullptr;
  QRhiGraphicsPipeline*       pipeline_         = nullptr;
  QShader                     vs_;
  QShader                     fs_;
  bool                        static_ready_ = false;

#if defined(_WIN32) && defined(HAVE_CUDA)
  ID3D11Device* d3d11_device_ = nullptr;
  int           cuda_device_  = -1;
#endif
};

}  // namespace alcedo::editor_rhi
