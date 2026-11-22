//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX2 experiment: viewport item. Experiment code: it does not merge.
#include "vx2_item.hpp"

#include <vulkan/vulkan.h>

#include <QFile>
#include <QtGui/rhi/qrhi.h>
#include <QtGui/rhi/qrhi_platform.h>
#include <QtGui/rhi/qshader.h>

#include <cstring>
#include <map>

#include "vx2_common.hpp"
#include "vx2_vulkan.hpp"

namespace vx2 {

std::atomic<std::uint64_t> Vx2Item::submitted_frames{0};
FrameMarks                 Vx2Item::marks{};
std::atomic<bool>          Vx2Item::y_up_in_ndc{true};
std::atomic<int>           Vx2Item::errors{0};

namespace {

auto LoadShader(const QString& path) -> QShader {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return QShader::fromSerialized(file.readAll());
}

struct VertexData {
  float position[2];
  float uv[2];
};

/// The block ViewerUniforms of editor_viewport.vert and .frag (ViewportCompositeUniforms).
struct Uniforms {
  float scale_zoom[4]        = {1, 1, 1, 0};
  float pan_mode[4]          = {0, 0, 0, 0};
  float detail_roi[4]        = {0, 0, 1, 1};
  float detail_flags[4]      = {0, 0, 0, 0};
  float coverage_u[4]        = {1, 0, 0, 0};
  float coverage_v[4]        = {0, 1, 0, 0};
  float coverage_tint[4]     = {0, 0, 0, 0};
  float detail_coverage_u[4] = {1, 0, 0, 0};
  float detail_coverage_v[4] = {0, 1, 0, 0};
  float display_lut[4]       = {0, 0, 1, 0};
};
static_assert(sizeof(Uniforms) == 160);

void Fail(const char* what) {
  Vx2Item::errors++;
  Log("ERROR renderer: %s", what);
}

class Vx2Renderer final : public QQuickRhiItemRenderer {
 public:
  void initialize(QRhiCommandBuffer*) override {
    rhi_ = rhi();
    Vx2Item::y_up_in_ndc = rhi_->isYUpInNDC();
    if (!ubo_) {
      ubo_.reset(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Uniforms)));
      vbuf_.reset(rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                  4 * sizeof(VertexData)));
      linear_.reset(rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                     QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
      nearest_.reset(rhi_->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                      QRhiSampler::None, QRhiSampler::ClampToEdge,
                                      QRhiSampler::ClampToEdge));
      lut_sampler_.reset(rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                          QRhiSampler::None, QRhiSampler::ClampToEdge,
                                          QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
      placeholder_.reset(rhi_->newTexture(QRhiTexture::RGBA32F, QSize(1, 1), 1));
      if (!ubo_->create() || !vbuf_->create() || !linear_->create() || !nearest_->create() ||
          !lut_sampler_->create() || !placeholder_->create()) {
        Fail("static resources");
      }
    }
    if (pipeline_ && pipeline_target_ != renderTarget()) {
      pipeline_.reset();
    }
  }

  void synchronize(QQuickRhiItem* item) override {
    auto* v = static_cast<Vx2Item*>(item);
    if (!configured_) {
      config_     = v->config();
      configured_ = true;
    }
    if (config_.source == PhotoSource::VulkanProducer) {
      auto frame = config_.producer->TakeLatest(Vx2Item::submitted_frames.load());
      if (frame) {
        if (wrapped_.size() < static_cast<std::size_t>(config_.producer->SlotCount())) {
          wrapped_.resize(config_.producer->SlotCount());
        }
        auto& tex = wrapped_[frame->slot];
        if (!tex) {
          tex.reset(rhi_->newTexture(QRhiTexture::RGBA32F, QSize(config_.width, config_.height), 1));
          if (!tex->createFrom({static_cast<quint64>(reinterpret_cast<std::uintptr_t>(frame->image)),
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL})) {
            Fail("QRhiTexture::createFrom of the producer VkImage");
          }
        } else {
          tex->setNativeLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        photo_                         = tex.get();
        if (config_.producer->Split()) acquire_image_ = frame->image;
        Vx2Item::marks.new_id          = frame->id;
        Vx2Item::marks.new_request_ns  = frame->request_ns;
      }
    } else {
      float         gain = 1;
      std::uint64_t id   = 0;
      std::int64_t  ns   = 0;
      if (v->TakeRequest(gain, id, ns)) {
        compute_pending_ = true;
        gain_            = gain;
        pending_id_      = id;
        pending_ns_      = ns;
      }
    }
  }

  void render(QRhiCommandBuffer* cb) override {
    QRhiResourceUpdateBatch* updates = rhi_->nextResourceUpdateBatch();
    if (!static_uploaded_) {
      static constexpr VertexData kVertices[4] = {{{-1, -1}, {0, 1}},
                                                  {{1, -1}, {1, 1}},
                                                  {{-1, 1}, {0, 0}},
                                                  {{1, 1}, {1, 0}}};
      updates->uploadStaticBuffer(vbuf_.get(), kVertices);
      const float zero[4] = {0, 0, 0, 0};
      updates->uploadTexture(placeholder_.get(),
                             QRhiTextureUploadDescription({0, 0, QRhiTextureSubresourceUploadDescription(zero, sizeof(zero))}));
      if (config_.lut) UploadLut(updates);
      if (config_.source == PhotoSource::RhiCompute) CreateCompute(updates);
      static_uploaded_ = true;
    }
    if (config_.source == PhotoSource::RhiCompute && compute_pending_ && cpipe_) {
      const std::int32_t params[4] = {config_.width, config_.height, config_.work, 0};
      const float        grade[4]  = {gain_, 0, 0, 0};
      updates->updateDynamicBuffer(cparams_.get(), 0, 16, params);
      updates->updateDynamicBuffer(cparams_.get(), 16, 16, grade);
      cb->beginComputePass(updates);
      updates = nullptr;
      cb->setComputePipeline(cpipe_.get());
      cb->setShaderResources(csrb_.get());
      cb->dispatch((config_.width + 15) / 16, (config_.height + 15) / 16, 1);
      cb->endComputePass();
      compute_pending_              = false;
      photo_                        = target_.get();
      Vx2Item::marks.new_id         = pending_id_;
      Vx2Item::marks.new_request_ns = pending_ns_;
    }
    if (updates == nullptr) updates = rhi_->nextResourceUpdateBatch();

    if (acquire_image_ != VK_NULL_HANDLE) {
      // Split device: acquire the image from the external owner (the pipeline device released
      // it). The layout stays SHADER_READ_ONLY_OPTIMAL, the layout that QRhi tracks.
      cb->beginExternal();
      const auto* native =
          static_cast<const QRhiVulkanCommandBufferNativeHandles*>(cb->nativeHandles());
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.srcAccessMask       = 0;
      b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
      b.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
      b.dstQueueFamilyIndex = config_.producer->Family();
      b.image               = acquire_image_;
      b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(native->commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      cb->endExternal();
      acquire_image_ = VK_NULL_HANDLE;
    }

    Uniforms u;
    if (config_.flip_correction && !rhi_->isYUpInNDC()) u.scale_zoom[1] = -1.0f;
    if (lut_) {
      u.display_lut[0] = 1.0f;
      u.display_lut[1] = static_cast<float>(config_.lut->edge);
      u.display_lut[2] = 1.0f;
    }
    updates->updateDynamicBuffer(ubo_.get(), 0, sizeof(u), &u);

    const bool ready = photo_ != nullptr && EnsureBindings() && EnsurePipeline();
    cb->beginPass(renderTarget(), Qt::black, {1.0f, 0}, updates);
    if (ready) {
      const QRhiCommandBuffer::VertexInput input[] = {{vbuf_.get(), 0}};
      const QSize                          size    = renderTarget()->pixelSize();
      cb->setGraphicsPipeline(pipeline_.get());
      cb->setViewport(QRhiViewport(0, 0, static_cast<float>(size.width()),
                                   static_cast<float>(size.height())));
      cb->setShaderResources(srb_);
      cb->setVertexInput(0, 1, input);
      cb->draw(4);
    }
    cb->endPass();
  }

 private:
  void UploadLut(QRhiResourceUpdateBatch* updates) {
    const int edge = config_.lut->edge;
    lut_.reset(rhi_->newTexture(QRhiTexture::RGBA32F, edge, edge, edge, 1,
                                QRhiTexture::ThreeDimensional));
    if (!lut_->create()) {
      Fail("display table texture");
      lut_.reset();
      return;
    }
    const std::size_t slice = static_cast<std::size_t>(edge) * edge * 4;
    std::vector<QRhiTextureUploadEntry> entries;
    for (int s = 0; s < edge; ++s) {
      QRhiTextureSubresourceUploadDescription d(QByteArray(
          reinterpret_cast<const char*>(config_.lut->rgba.data() + s * slice),
          static_cast<int>(slice * sizeof(float))));
      d.setDataStride(static_cast<quint32>(edge * 4 * sizeof(float)));
      d.setSourceSize(QSize(edge, edge));
      entries.emplace_back(s, 0, d);
    }
    QRhiTextureUploadDescription upload;
    upload.setEntries(entries.cbegin(), entries.cend());
    updates->uploadTexture(lut_.get(), upload);
  }

  void CreateCompute(QRhiResourceUpdateBatch* updates) {
    const auto bytes = static_cast<quint32>(config_.fixture->size() * sizeof(float));
    src_.reset(rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::StorageBuffer, bytes));
    cparams_.reset(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 32));
    target_.reset(rhi_->newTexture(QRhiTexture::RGBA32F, QSize(config_.width, config_.height), 1,
                                   QRhiTexture::UsedWithLoadStore));
    if (!src_->create() || !cparams_->create() || !target_->create()) {
      Fail("compute resources");
      return;
    }
    updates->uploadStaticBuffer(src_.get(), config_.fixture->data());
    csrb_.reset(rhi_->newShaderResourceBindings());
    csrb_->setBindings(
        {QRhiShaderResourceBinding::bufferLoad(0, QRhiShaderResourceBinding::ComputeStage, src_.get()),
         QRhiShaderResourceBinding::imageStore(1, QRhiShaderResourceBinding::ComputeStage,
                                               target_.get(), 0),
         QRhiShaderResourceBinding::uniformBuffer(2, QRhiShaderResourceBinding::ComputeStage,
                                                  cparams_.get())});
    if (!csrb_->create()) {
      Fail("compute bindings");
      return;
    }
    const QShader shader = LoadShader(QStringLiteral(":/vx2/vx2_fill.comp.qsb"));
    cpipe_.reset(rhi_->newComputePipeline());
    cpipe_->setShaderStage({QRhiShaderStage::Compute, shader});
    cpipe_->setShaderResourceBindings(csrb_.get());
    if (!shader.isValid() || !cpipe_->create()) {
      Fail("compute pipeline");
      cpipe_.reset();
    }
  }

  /// One binding set for each photo texture (at most one for each producer slot). The first set
  /// stays alive as the layout of the pipeline.
  auto EnsureBindings() -> bool {
    auto found = srbs_.find(photo_);
    if (found != srbs_.end()) {
      srb_ = found->second.get();
      return true;
    }
    std::unique_ptr<QRhiShaderResourceBindings> srb(rhi_->newShaderResourceBindings());
    const auto frag = QRhiShaderResourceBinding::FragmentStage;
    const auto both = QRhiShaderResourceBinding::VertexStage | frag;
    srb->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, both, ubo_.get()),
                      QRhiShaderResourceBinding::sampledTexture(1, frag, photo_, linear_.get()),
                      QRhiShaderResourceBinding::sampledTexture(2, frag, placeholder_.get(), nearest_.get()),
                      QRhiShaderResourceBinding::sampledTexture(3, frag, placeholder_.get(), linear_.get()),
                      QRhiShaderResourceBinding::sampledTexture(4, frag, placeholder_.get(), linear_.get()),
                      QRhiShaderResourceBinding::sampledTexture(5, frag, lut_ ? lut_.get() : placeholder3d(),
                                                                lut_sampler_.get())});
    if (!srb->create()) {
      Fail("shader resource bindings");
      return false;
    }
    srb_ = srb.get();
    srbs_.emplace(photo_, std::move(srb));
    return true;
  }

  auto placeholder3d() -> QRhiTexture* {
    if (!placeholder3d_) {
      placeholder3d_.reset(rhi_->newTexture(QRhiTexture::RGBA32F, 2, 2, 2, 1,
                                            QRhiTexture::ThreeDimensional));
      placeholder3d_->create();
    }
    return placeholder3d_.get();
  }

  auto EnsurePipeline() -> bool {
    if (pipeline_) return true;
    const QShader vs = LoadShader(QStringLiteral(":/vx2/editor_viewport.vert.qsb"));
    const QShader fs = LoadShader(QStringLiteral(":/vx2/editor_viewport.frag.qsb"));
    if (!vs.isValid() || !fs.isValid()) {
      Fail("composite shaders not found");
      return false;
    }
    pipeline_.reset(rhi_->newGraphicsPipeline());
    QRhiVertexInputLayout layout;
    layout.setBindings({QRhiVertexInputBinding(sizeof(VertexData))});
    layout.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
                          QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2,
                                                   2 * sizeof(float))});
    pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipeline_->setCullMode(QRhiGraphicsPipeline::None);
    pipeline_->setSampleCount(renderTarget()->sampleCount());
    pipeline_->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
    pipeline_->setVertexInputLayout(layout);
    pipeline_->setShaderResourceBindings(srb_);
    pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!pipeline_->create()) {
      Fail("graphics pipeline");
      pipeline_.reset();
      return false;
    }
    pipeline_target_ = renderTarget();
    return true;
  }

  QRhi*              rhi_ = nullptr;
  Vx2Item::Config    config_;
  bool               configured_      = false;
  bool               static_uploaded_ = false;
  std::unique_ptr<QRhiBuffer>   ubo_, vbuf_, src_, cparams_;
  std::unique_ptr<QRhiSampler>  linear_, nearest_, lut_sampler_;
  std::unique_ptr<QRhiTexture>  placeholder_, placeholder3d_, lut_, target_;
  std::vector<std::unique_ptr<QRhiTexture>> wrapped_;
  std::unique_ptr<QRhiShaderResourceBindings> csrb_;
  std::map<QRhiTexture*, std::unique_ptr<QRhiShaderResourceBindings>> srbs_;
  QRhiShaderResourceBindings*                 srb_ = nullptr;
  std::unique_ptr<QRhiGraphicsPipeline>       pipeline_;
  std::unique_ptr<QRhiComputePipeline>        cpipe_;
  QRhiRenderTarget*  pipeline_target_ = nullptr;
  QRhiTexture*       photo_ = nullptr;
  VkImage            acquire_image_ = VK_NULL_HANDLE;
  bool               compute_pending_ = false;
  float              gain_            = 1;
  std::uint64_t      pending_id_      = 0;
  std::int64_t       pending_ns_      = 0;
};

}  // namespace

Vx2Item::Vx2Item(QQuickItem* parent) : QQuickRhiItem(parent) {
  setColorBufferFormat(QQuickRhiItem::TextureFormat::RGBA32F);
  setMirrorVertically(false);
  setAlphaBlending(false);
}

QQuickRhiItemRenderer* Vx2Item::createRenderer() { return new Vx2Renderer(); }

void Vx2Item::RequestGain(float gain, std::int64_t request_ns) {
  {
    std::lock_guard<std::mutex> lock(request_mutex_);
    request_pending_ = true;
    request_gain_    = gain;
    request_ns_      = request_ns;
    ++request_id_;
  }
  update();
}

auto Vx2Item::TakeRequest(float& gain, std::uint64_t& id, std::int64_t& request_ns) -> bool {
  std::lock_guard<std::mutex> lock(request_mutex_);
  if (!request_pending_) return false;
  request_pending_ = false;
  gain             = request_gain_;
  id               = request_id_;
  request_ns       = request_ns_;
  return true;
}

}  // namespace vx2
