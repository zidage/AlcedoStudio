//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QtGui/rhi/qrhi.h>
#include <QtGui/rhi/qshader.h>

#include <QSize>
#include <QString>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "ui/edit_viewer/display_color_transform.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo::editor_rhi {

/**
 * @brief Presentation state of the coverage composite (SM8 C10, section 5.3).
 *
 * The tint is display-encoded RGB in [0, 1], the same encoding as the presented photo frame.
 * The style changes only the viewport draw: no photo DAG request, no export pixel, no scope input,
 * and no history entry. The default values are those of the AppTheme roles
 * `maskCoverageTintColor` and `maskCoverageTintOpacity`.
 */
struct MaskCoverageDisplayStyle {
  bool  visible                                                         = true;
  float red                                                             = 1.0f;
  float green                                                           = 80.0f / 255.0f;
  float blue                                                            = 80.0f / 255.0f;
  float opacity                                                         = 0.5f;

  auto  operator==(const MaskCoverageDisplayStyle& other) const -> bool = default;
};

/// Affine map from photo frame UV to coverage frame UV: `u' = u_row · (u, v, 1)`,
/// `v' = v_row · (u, v, 1)`.
struct CoverageUvTransform {
  float u_row[3] = {1.0f, 0.0f, 0.0f};
  float v_row[3] = {0.0f, 1.0f, 0.0f};
};

/**
 * @brief Map the UV of a presented photo frame to the UV of a coverage frame through their common
 *        reference space.
 *
 * `photo UV × photo render extent → photo.render_to_reference → coverage.reference_to_render
 * → ÷ coverage render extent`. The two frames may differ in crop, rotation, render scale, and
 * viewport ROI: the map uses only their ResolveRenderGeometry results, so no coordinate is rounded
 * again. Identical geometry gives the identity map.
 *
 * @return std::nullopt when either geometry has an empty render or reference extent, or when the
 *         reference extents differ (the frames are of different source images). The coverage is
 *         then not composited.
 */
[[nodiscard]] auto MapPhotoUvToCoverageUv(const ResolvedRenderGeometry& photo,
                                          const ResolvedRenderGeometry& coverage)
    -> std::optional<CoverageUvTransform>;

/// std140 uniform block `ViewerUniforms` of editor_viewport.vert and editor_viewport.frag.
struct ViewportCompositeUniforms {
  float scale_zoom[4]    = {1.0f, 1.0f, 1.0f, 0.0f};
  float pan_mode[4]      = {0.0f, 0.0f, 0.0f, 0.0f};
  float detail_roi[4]    = {0.0f, 0.0f, 1.0f, 1.0f};
  float detail_flags[4]  = {0.0f, 0.0f, 0.0f, 0.0f};
  /// xyz: CoverageUvTransform::u_row; w: 1 when coverage is composited.
  float coverage_u[4]    = {1.0f, 0.0f, 0.0f, 0.0f};
  /// xyz: CoverageUvTransform::v_row.
  float coverage_v[4]    = {0.0f, 1.0f, 0.0f, 0.0f};
  /// rgb: display-encoded tint; a: overlay opacity.
  float coverage_tint[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  /// xyz: CoverageUvTransform::u_row of the detail coverage frame; w: 1 when it is composited.
  float detail_coverage_u[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  /// xyz: CoverageUvTransform::v_row of the detail coverage frame.
  float detail_coverage_v[4] = {0.0f, 1.0f, 0.0f, 0.0f};
  /// x: 1 when the display table converts the output; y: nodes per axis; z: input exponent.
  /// @ref ViewportCompositePass::Record writes it from the table of the pass.
  float display_lut[4]       = {0.0f, 0.0f, 1.0f, 0.0f};
};
static_assert(sizeof(ViewportCompositeUniforms) == 10 * 4 * sizeof(float),
              "ViewportCompositeUniforms must match the std140 block ViewerUniforms");

/// The selected photo layer of one viewport draw.
struct ViewportCompositePhoto {
  int                    width  = 0;
  int                    height = 0;
  FramePresentationMode  mode   = FramePresentationMode::FullFrame;
  /// ResolveRenderGeometry result of the frame (FrameCompletionSubmission::geometry).
  ResolvedRenderGeometry geometry{};
};

/// Inputs of @ref BuildViewportCompositeUniforms.
struct ViewportCompositeLayout {
  /// Pixel size of the viewport render target.
  QSize                         target_size{};
  ViewportCompositePhoto        primary{};
  float                         zoom  = 1.0f;
  float                         pan_x = 0.0f;
  float                         pan_y = 0.0f;
  /// Source ROI of a visible DetailPatch in primary UV, or none.
  std::optional<FrameRoiRect>   detail_roi{};
  /// Geometry of the displayed coverage frame, or null when no coverage is displayed.
  const ResolvedRenderGeometry* coverage_geometry = nullptr;
  /// Geometry of the displayed detail coverage frame (the coverage of a detail ROI render, Mask
  /// ranges D10), or null. Used only inside a visible DetailPatch ROI.
  const ResolvedRenderGeometry* detail_coverage_geometry = nullptr;
  MaskCoverageDisplayStyle      coverage_style{};
};

/**
 * @brief Uniforms of one viewport draw: letterbox, zoom and pan of the photo quad, the DetailPatch
 *        ROI, and the coverage maps and tint.
 *
 * A RoiFrame primary is drawn without zoom and pan; its own geometry maps it to the coverage.
 * A coverage frame is composited only when its geometry is given, the style is visible with a
 * positive opacity, and @ref MapPhotoUvToCoverageUv has a map. The detail coverage frame needs a
 * visible DetailPatch ROI as well: inside that ROI the shader samples it where it has texels, and
 * the base coverage frame elsewhere.
 */
[[nodiscard]] auto BuildViewportCompositeUniforms(const ViewportCompositeLayout& layout)
    -> ViewportCompositeUniforms;

/// Outcome of @ref ViewportCompositePass::Record.
enum class ViewportCompositeOutcome : std::uint8_t {
  /// The photo, optional DetailPatch, and optional coverage were drawn.
  Drawn = 0,
  /// No photo layer: the target was cleared to black.
  NoPhoto,
  /// A shader, binding, or pipeline error: the target was cleared and nothing was drawn.
  /// @ref ViewportCompositePass::LastError names the failure.
  Failed,
};

/**
 * @brief The one display composite of the editor viewport (SM8 C6, section 5.3).
 *
 * One draw selects the base photo, replaces it inside a visible DetailPatch ROI, and then applies
 * the coverage tint: `a = clamp(coverage, 0, 1) × opacity`,
 * `rgb = photo_rgb × (1 − a) + tint_rgb × a`. Pixels with `a = 0` keep their photo value, including
 * negative and above-one values, and every pixel keeps its photo alpha. Candidate coverage, a
 * committed Mask, and a Grade Union all use this draw. With a display table
 * (@ref SetDisplayTransform) the last step samples the table at the clamped result, so the photo
 * and the tint are converted to the monitor profile together.
 *
 * Owns the QRhi resources of the draw: samplers, uniform and vertex buffers, the shader resource
 * bindings, and the graphics pipeline. Input textures belong to the caller; call @ref ForgetTexture
 * before destroying one that the last draw used.
 *
 * Thread: the thread of the QRhi (the scene-graph render thread). Call @ref Release before the
 * QRhi is destroyed.
 */
class ViewportCompositePass {
 public:
  /// The production shaders of the viewport.
  ViewportCompositePass();
  /// Shaders from the serialized QShader files @p vertex_shader and @p fragment_shader.
  ViewportCompositePass(QString vertex_shader, QString fragment_shader);
  ~ViewportCompositePass();
  ViewportCompositePass(const ViewportCompositePass&)                    = delete;
  auto operator=(const ViewportCompositePass&) -> ViewportCompositePass& = delete;

  /// Input textures of one draw. Null detail or coverage textures bind a black placeholder.
  struct Textures {
    QRhiTexture* primary         = nullptr;
    QRhiTexture* detail          = nullptr;
    QRhiTexture* coverage        = nullptr;
    QRhiTexture* detail_coverage = nullptr;
  };

  /**
   * @brief Record one render pass on @p render_target: clear to black, then draw the composite.
   *
   * @pre A frame of @p rhi is being recorded on @p command_buffer.
   * @return Drawn, NoPhoto when @p textures has no primary, or Failed with @ref LastError.
   */
  auto Record(QRhi* rhi, QRhiCommandBuffer* command_buffer, QRhiRenderTarget* render_target,
              const Textures& textures, const ViewportCompositeUniforms& uniforms)
      -> ViewportCompositeOutcome;

  /**
   * @brief Use @p lut as the display table of the next draws, or no table when it is null.
   *
   * The pass uploads a table once: the next @ref Record uploads it when the pointer differs from
   * the uploaded one, and creates the 3D texture again when the node count changes. Without a
   * table the draw output keeps the photo encoding.
   */
  void               SetDisplayTransform(std::shared_ptr<const DisplayTransformLut> lut);

  /// Drop the shader resource bindings when they use @p texture. Call before destroying it.
  void               ForgetTexture(const QRhiTexture* texture);
  /// Destroy every QRhi resource of the pass. Idempotent.
  void               Release();
  /// Error of the last failed @ref Record, or empty.
  [[nodiscard]] auto LastError() const -> const std::string& { return last_error_; }

 private:
  auto                        EnsureResources(QRhi* rhi, QRhiResourceUpdateBatch* updates) -> bool;
  auto                        EnsureBindings(QRhi* rhi, const Textures& textures) -> bool;
  auto                        EnsurePipeline(QRhi* rhi, QRhiRenderTarget* render_target) -> bool;
  auto EnsureDisplayTable(QRhi* rhi, QRhiResourceUpdateBatch* updates) -> bool;
  auto                        Fail(std::string error) -> ViewportCompositeOutcome;

  QString                     vertex_shader_path_;
  QString                     fragment_shader_path_;
  QRhi*                       rhi_                 = nullptr;
  QRhiTexture*                placeholder_texture_ = nullptr;
  QRhiSampler*                linear_sampler_      = nullptr;
  QRhiSampler*                nearest_sampler_     = nullptr;
  QRhiBuffer*                 uniform_buffer_      = nullptr;
  QRhiBuffer*                 vertex_buffer_       = nullptr;
  QRhiShaderResourceBindings* bindings_            = nullptr;
  QRhiGraphicsPipeline*       pipeline_            = nullptr;
  QRhiRenderTarget*           pipeline_target_     = nullptr;
  QRhiRenderPassDescriptor*   pipeline_pass_       = nullptr;
  int                         pipeline_samples_    = 0;
  QRhiTexture*                bound_primary_       = nullptr;
  QRhiTexture*                bound_detail_        = nullptr;
  QRhiTexture*                bound_coverage_      = nullptr;
  QRhiTexture*                bound_detail_coverage_ = nullptr;
  /// The 3D texture of the display table: 2 nodes per axis while there is no table.
  QRhiTexture*                display_texture_       = nullptr;
  QRhiSampler*                display_sampler_       = nullptr;
  int                         display_texture_edge_  = 0;
  /// The table of the next draws, and the table that display_texture_ holds.
  std::shared_ptr<const DisplayTransformLut> display_lut_;
  std::shared_ptr<const DisplayTransformLut> uploaded_display_lut_;
  bool                        static_upload_       = false;
  std::string                 last_error_;
};

}  // namespace alcedo::editor_rhi
