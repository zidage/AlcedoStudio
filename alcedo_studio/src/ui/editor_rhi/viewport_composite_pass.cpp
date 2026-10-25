//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/viewport_composite_pass.hpp"

#include <QByteArray>
#include <QFile>
#include <array>
#include <cmath>
#include <utility>

#include "ui/edit_viewer/viewport_mapper.hpp"

namespace alcedo::editor_rhi {
namespace {

constexpr const char* kVertexShaderResource   = ":/shaders/editor_rhi/editor_viewport.vert.qsb";
constexpr const char* kFragmentShaderResource = ":/shaders/editor_rhi/editor_viewport.frag.qsb";

struct VertexData {
  float position[2];
  float uv[2];
};

template <typename T>
void DestroyRhi(T*& resource) {
  if (resource == nullptr) {
    return;
  }
  resource->destroy();
  delete resource;
  resource = nullptr;
}

/// Row-major 2x3 affine in double precision: [a b c; d e f].
struct Affine {
  double m[6] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
};

auto FromMatrix(const Matrix3x3& matrix) -> Affine {
  return Affine{{matrix.m[0], matrix.m[1], matrix.m[2], matrix.m[3], matrix.m[4], matrix.m[5]}};
}

/// lhs ∘ rhs: apply @p rhs first.
auto Compose(const Affine& lhs, const Affine& rhs) -> Affine {
  const auto* a = lhs.m;
  const auto* b = rhs.m;
  return Affine{{a[0] * b[0] + a[1] * b[3], a[0] * b[1] + a[1] * b[4],
                 a[0] * b[2] + a[1] * b[5] + a[2], a[3] * b[0] + a[4] * b[3],
                 a[3] * b[1] + a[4] * b[4], a[3] * b[2] + a[4] * b[5] + a[5]}};
}

auto Scale(double sx, double sy) -> Affine { return Affine{{sx, 0.0, 0.0, 0.0, sy, 0.0}}; }

auto LoadShader(const QString& path) -> QShader {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return QShader::fromSerialized(file.readAll());
}

}  // namespace

auto MapPhotoUvToCoverageUv(const ResolvedRenderGeometry& photo,
                            const ResolvedRenderGeometry& coverage)
    -> std::optional<CoverageUvTransform> {
  if (photo.render_extent.Empty() || photo.full_reference_extent.Empty() ||
      coverage.render_extent.Empty() || coverage.full_reference_extent.Empty() ||
      photo.full_reference_extent != coverage.full_reference_extent) {
    return std::nullopt;
  }
  const Affine photo_uv_to_render =
      Scale(static_cast<double>(photo.render_extent.width), photo.render_extent.height);
  const Affine render_to_coverage_uv =
      Scale(1.0 / static_cast<double>(coverage.render_extent.width),
            1.0 / static_cast<double>(coverage.render_extent.height));
  const Affine map =
      Compose(render_to_coverage_uv,
              Compose(FromMatrix(coverage.reference_to_render),
                      Compose(FromMatrix(photo.render_to_reference), photo_uv_to_render)));
  for (const double value : map.m) {
    if (!std::isfinite(value)) {
      return std::nullopt;
    }
  }
  CoverageUvTransform transform;
  for (int i = 0; i < 3; ++i) {
    transform.u_row[i] = static_cast<float>(map.m[i]);
    transform.v_row[i] = static_cast<float>(map.m[3 + i]);
  }
  return transform;
}

auto BuildViewportCompositeUniforms(const ViewportCompositeLayout& layout)
    -> ViewportCompositeUniforms {
  ViewportCompositeUniforms uniforms{};
  const auto&               primary = layout.primary;
  if (primary.width <= 0 || primary.height <= 0) {
    return uniforms;
  }
  const auto scale = ViewportMapper::ComputeLetterboxScale(
      ViewportWidgetInfo{layout.target_size.width(), layout.target_size.height(), 1.0f},
      ViewportImageInfo{primary.width, primary.height});
  uniforms.scale_zoom[0] = scale.x;
  uniforms.scale_zoom[1] = scale.y;
  uniforms.scale_zoom[2] = layout.zoom;
  uniforms.pan_mode[0]   = layout.pan_x;
  uniforms.pan_mode[1]   = layout.pan_y;
  if (primary.mode == FramePresentationMode::RoiFrame) {
    // The ROI frame already holds the visible part of the view.
    uniforms.scale_zoom[2] = 1.0f;
    uniforms.pan_mode[0]   = 0.0f;
    uniforms.pan_mode[1]   = 0.0f;
    uniforms.pan_mode[2]   = 1.0f;
  }
  if (layout.detail_roi.has_value()) {
    const auto& roi          = *layout.detail_roi;
    uniforms.detail_roi[0]   = roi.x;
    uniforms.detail_roi[1]   = roi.y;
    uniforms.detail_roi[2]   = roi.width;
    uniforms.detail_roi[3]   = roi.height;
    uniforms.detail_flags[0] = 1.0f;
  }
  const auto& style = layout.coverage_style;
  if (layout.coverage_geometry == nullptr || !style.visible || !(style.opacity > 0.0f)) {
    return uniforms;
  }
  const auto map = MapPhotoUvToCoverageUv(primary.geometry, *layout.coverage_geometry);
  if (!map.has_value()) {
    return uniforms;
  }
  for (int i = 0; i < 3; ++i) {
    uniforms.coverage_u[i] = map->u_row[i];
    uniforms.coverage_v[i] = map->v_row[i];
  }
  uniforms.coverage_u[3]    = 1.0f;
  uniforms.coverage_tint[0] = style.red;
  uniforms.coverage_tint[1] = style.green;
  uniforms.coverage_tint[2] = style.blue;
  uniforms.coverage_tint[3] = std::fmin(style.opacity, 1.0f);
  return uniforms;
}

ViewportCompositePass::ViewportCompositePass()
    : ViewportCompositePass(QString::fromUtf8(kVertexShaderResource),
                            QString::fromUtf8(kFragmentShaderResource)) {}

ViewportCompositePass::ViewportCompositePass(QString vertex_shader, QString fragment_shader)
    : vertex_shader_path_(std::move(vertex_shader)),
      fragment_shader_path_(std::move(fragment_shader)) {}

ViewportCompositePass::~ViewportCompositePass() { Release(); }

void ViewportCompositePass::ForgetTexture(const QRhiTexture* texture) {
  if (texture == nullptr) {
    return;
  }
  if (texture == bound_primary_ || texture == bound_detail_ || texture == bound_coverage_) {
    // The bindings keep the texture pointer; a later texture may reuse the address.
    DestroyRhi(bindings_);
    bound_primary_  = nullptr;
    bound_detail_   = nullptr;
    bound_coverage_ = nullptr;
  }
}

void ViewportCompositePass::Release() {
  DestroyRhi(pipeline_);
  DestroyRhi(bindings_);
  DestroyRhi(vertex_buffer_);
  DestroyRhi(uniform_buffer_);
  DestroyRhi(linear_sampler_);
  DestroyRhi(nearest_sampler_);
  DestroyRhi(placeholder_texture_);
  pipeline_target_  = nullptr;
  pipeline_pass_    = nullptr;
  pipeline_samples_ = 0;
  bound_primary_    = nullptr;
  bound_detail_     = nullptr;
  bound_coverage_   = nullptr;
  static_upload_    = false;
  rhi_              = nullptr;
}

auto ViewportCompositePass::Fail(std::string error) -> ViewportCompositeOutcome {
  last_error_ = std::move(error);
  return ViewportCompositeOutcome::Failed;
}

auto ViewportCompositePass::EnsureResources(QRhi* rhi, QRhiResourceUpdateBatch* updates) -> bool {
  if (rhi_ != rhi) {
    Release();
    rhi_ = rhi;
  }
  if (placeholder_texture_ == nullptr) {
    placeholder_texture_ = rhi->newTexture(QRhiTexture::RGBA32F, QSize(1, 1), 1);
    if (!placeholder_texture_->create()) {
      DestroyRhi(placeholder_texture_);
      last_error_ = "viewport composite: the placeholder texture could not be created";
      return false;
    }
    static_upload_ = true;
  }
  if (linear_sampler_ == nullptr) {
    linear_sampler_ = rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                      QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    if (!linear_sampler_->create()) {
      DestroyRhi(linear_sampler_);
      last_error_ = "viewport composite: the linear sampler could not be created";
      return false;
    }
  }
  if (nearest_sampler_ == nullptr) {
    nearest_sampler_ =
        rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    if (!nearest_sampler_->create()) {
      DestroyRhi(nearest_sampler_);
      last_error_ = "viewport composite: the nearest sampler could not be created";
      return false;
    }
  }
  if (uniform_buffer_ == nullptr) {
    uniform_buffer_ = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                     sizeof(ViewportCompositeUniforms));
    if (!uniform_buffer_->create()) {
      DestroyRhi(uniform_buffer_);
      last_error_ = "viewport composite: the uniform buffer could not be created";
      return false;
    }
  }
  if (vertex_buffer_ == nullptr) {
    vertex_buffer_ =
        rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, 4 * sizeof(VertexData));
    if (!vertex_buffer_->create()) {
      DestroyRhi(vertex_buffer_);
      last_error_ = "viewport composite: the vertex buffer could not be created";
      return false;
    }
    static_upload_ = true;
  }
  if (static_upload_) {
    static constexpr std::array<VertexData, 4> kVertices = {
        VertexData{{-1.0f, -1.0f}, {0.0f, 1.0f}},
        VertexData{{1.0f, -1.0f}, {1.0f, 1.0f}},
        VertexData{{-1.0f, 1.0f}, {0.0f, 0.0f}},
        VertexData{{1.0f, 1.0f}, {1.0f, 0.0f}},
    };
    // Black with zero coverage in red: an absent detail or coverage input changes nothing.
    static constexpr std::array<float, 4> kBlack = {0.0f, 0.0f, 0.0f, 1.0f};
    updates->uploadStaticBuffer(vertex_buffer_, kVertices.data());
    QRhiTextureSubresourceUploadDescription description(
        QByteArray(reinterpret_cast<const char*>(kBlack.data()), static_cast<int>(sizeof(kBlack))));
    description.setDataStride(static_cast<quint32>(sizeof(kBlack)));
    description.setSourceSize(QSize(1, 1));
    updates->uploadTexture(placeholder_texture_,
                           QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, description)));
    static_upload_ = false;
  }
  return true;
}

auto ViewportCompositePass::EnsureBindings(QRhi* rhi, const Textures& textures) -> bool {
  auto* detail   = textures.detail != nullptr ? textures.detail : placeholder_texture_;
  auto* coverage = textures.coverage != nullptr ? textures.coverage : placeholder_texture_;
  if (bindings_ != nullptr && bound_primary_ == textures.primary && bound_detail_ == detail &&
      bound_coverage_ == coverage) {
    return true;
  }
  DestroyRhi(bindings_);
  bound_primary_  = nullptr;
  bound_detail_   = nullptr;
  bound_coverage_ = nullptr;
  bindings_       = rhi->newShaderResourceBindings();
  bindings_->setBindings(
      {QRhiShaderResourceBinding::uniformBuffer(
           0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
           uniform_buffer_),
       QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                 textures.primary, linear_sampler_),
       QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                 detail, nearest_sampler_),
       QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                 coverage, linear_sampler_)});
  if (!bindings_->create()) {
    DestroyRhi(bindings_);
    last_error_ = "viewport composite: the shader resource bindings could not be created";
    return false;
  }
  // Every draw uses the same binding layout, so the pipeline stays compatible with new bindings.
  bound_primary_  = textures.primary;
  bound_detail_   = detail;
  bound_coverage_ = coverage;
  return true;
}

auto ViewportCompositePass::EnsurePipeline(QRhi* rhi, QRhiRenderTarget* render_target) -> bool {
  auto*     pass    = render_target->renderPassDescriptor();
  const int samples = render_target->sampleCount();
  // A new window render target (resize, device change) gets a new pipeline, as before SM8.6.
  if (pipeline_ != nullptr && pipeline_target_ == render_target && pipeline_pass_ == pass &&
      pipeline_samples_ == samples) {
    return true;
  }
  DestroyRhi(pipeline_);
  pipeline_target_       = nullptr;
  pipeline_pass_         = nullptr;
  const QShader vertex   = LoadShader(vertex_shader_path_);
  const QShader fragment = LoadShader(fragment_shader_path_);
  if (!vertex.isValid() || !fragment.isValid()) {
    last_error_ = "viewport composite: the shaders " + vertex_shader_path_.toStdString() + " and " +
                  fragment_shader_path_.toStdString() + " could not be loaded";
    return false;
  }
  pipeline_ = rhi->newGraphicsPipeline();
  QRhiVertexInputLayout layout;
  layout.setBindings({QRhiVertexInputBinding(sizeof(VertexData))});
  layout.setAttributes(
      {QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
       QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float))});
  pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
  pipeline_->setCullMode(QRhiGraphicsPipeline::None);
  pipeline_->setSampleCount(samples);
  pipeline_->setShaderStages({QRhiShaderStage(QRhiShaderStage::Vertex, vertex),
                              QRhiShaderStage(QRhiShaderStage::Fragment, fragment)});
  pipeline_->setVertexInputLayout(layout);
  pipeline_->setShaderResourceBindings(bindings_);
  pipeline_->setRenderPassDescriptor(pass);
  if (!pipeline_->create()) {
    DestroyRhi(pipeline_);
    last_error_ = "viewport composite: the graphics pipeline could not be created";
    return false;
  }
  pipeline_target_  = render_target;
  pipeline_pass_    = pass;
  pipeline_samples_ = samples;
  return true;
}

auto ViewportCompositePass::Record(QRhi* rhi, QRhiCommandBuffer* command_buffer,
                                   QRhiRenderTarget* render_target, const Textures& textures,
                                   const ViewportCompositeUniforms& uniforms)
    -> ViewportCompositeOutcome {
  auto* updates = rhi->nextResourceUpdateBatch();
  bool  ready   = EnsureResources(rhi, updates);
  if (ready && textures.primary != nullptr) {
    ready = EnsureBindings(rhi, textures) && EnsurePipeline(rhi, render_target);
    if (ready) {
      updates->updateDynamicBuffer(uniform_buffer_, 0, sizeof(uniforms), &uniforms);
    }
  }
  command_buffer->beginPass(render_target, Qt::black, {1.0f, 0}, updates);
  if (ready && textures.primary != nullptr) {
    const QRhiCommandBuffer::VertexInput vertex_input[] = {{vertex_buffer_, 0}};
    const QSize                          size           = render_target->pixelSize();
    command_buffer->setGraphicsPipeline(pipeline_);
    command_buffer->setViewport(QRhiViewport(0, 0, size.width(), size.height()));
    command_buffer->setShaderResources(bindings_);
    command_buffer->setVertexInput(0, 1, vertex_input);
    command_buffer->draw(4);
  }
  command_buffer->endPass();
  if (!ready) {
    return Fail(last_error_);
  }
  if (textures.primary == nullptr) {
    return ViewportCompositeOutcome::NoPhoto;
  }
  last_error_.clear();
  return ViewportCompositeOutcome::Drawn;
}

}  // namespace alcedo::editor_rhi
