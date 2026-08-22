//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/graph_compiler.hpp"

#include <stdexcept>
#include <string>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"

namespace alcedo {
namespace {

constexpr std::size_t kAlign = 256;

auto                  AlignUp(std::size_t value, std::size_t alignment) -> std::size_t {
  return (value + alignment - 1) & ~(alignment - 1);
}

auto EstimatePeakTransientBytes(const DevelopCompileSource& source) -> std::size_t {
  const std::size_t w      = source.host_extent.width;
  const std::size_t h      = source.host_extent.height;
  const std::size_t pixels = w * h;
  if (source.kind == DevelopInputKind::DirectRgb) {
    return AlignUp(4096, kAlign);
  }
  // U16 CFA + F32 CFA + 5 RCD planes + merge RGB + HLR result + HLR stats + XTrans green/rgb.
  const std::size_t bytes = AlignUp(pixels * 2, kAlign) + AlignUp(pixels * 4, kAlign) +
                            5 * AlignUp(pixels * 4, kAlign) + AlignUp(pixels * 12, kAlign) +
                            AlignUp(pixels * 12, kAlign) + AlignUp(4 + 16 + 16, kAlign) +
                            AlignUp(pixels * 4, kAlign) + AlignUp(pixels * 12, kAlign);
  return bytes + 64 * kAlign;
}

auto ImageParamsFromDocument(const PipelineDocument& document) -> ImageGeometryParams {
  ImageGeometryParams params;
  params.crop_rect        = document.Geometry().CropRect();
  params.rotation_degrees = document.Geometry().RotationDegrees();
  params.expand_to_fit    = document.Geometry().ExpandToFit();
  return params;
}

void RequireDefaultEndpoints(const PipelineDocument& document) {
  const auto errors = document.Graph().Validate();
  if (!errors.empty()) {
    throw std::runtime_error("GraphCompiler: graph is invalid: " + errors.front().message);
  }
  if (document.Develop() == nullptr) {
    throw std::runtime_error("GraphCompiler: missing develop endpoint");
  }
  if (document.PrimaryGrade() == nullptr) {
    throw std::runtime_error("GraphCompiler: missing primary color grade");
  }
  if (document.Drt() == nullptr) {
    throw std::runtime_error("GraphCompiler: missing DRT endpoint");
  }
  for (const auto& node : document.Graph().Nodes()) {
    const auto& type = node->Type();
    if (type == type_ids::DevelopNode() || type == type_ids::ColorGradeNode() ||
        type == type_ids::DrtNode() || type == type_ids::AnalyticMaskNode() ||
        type == type_ids::RasterMaskNode()) {
      continue;
    }
    throw std::runtime_error("GraphCompiler: unsupported GPU DAG node");
  }
}

}  // namespace

auto GraphCompiler::Compile(const PipelineDocument& document, const DevelopCompileSource& source,
                            const RenderRequest& request) -> ExecutionPlan {
  RequireDefaultEndpoints(document);
  if (source.host_extent.Empty() || source.develop_output_extent.Empty() ||
      source.full_reference_extent.Empty()) {
    throw std::runtime_error("GraphCompiler: source extents must be positive");
  }

  ExecutionPlan plan;
  plan.source               = source;
  plan.develop_output       = GraphValueId{NodeId{"develop"}, PortId{"image"}};
  plan.peak_transient_bytes = EstimatePeakTransientBytes(source);
  const auto* grade         = document.PrimaryGrade();

  if (source.kind == DevelopInputKind::DirectRgb) {
    plan.passes.push_back(GpuPassDesc{GpuPassKind::UploadRgb});
  } else {
    plan.passes.push_back(GpuPassDesc{GpuPassKind::UploadRaw});
    plan.passes.push_back(GpuPassDesc{GpuPassKind::Linearize});
    plan.passes.push_back(GpuPassDesc{GpuPassKind::CfaClamp});
    plan.passes.push_back(GpuPassDesc{GpuPassKind::Demosaic});
    plan.passes.push_back(GpuPassDesc{GpuPassKind::HighlightRecover});
    plan.passes.push_back(GpuPassDesc{GpuPassKind::InverseCamMulPack});
  }
  plan.passes.push_back(GpuPassDesc{GpuPassKind::Lens});
  plan.passes.push_back(GpuPassDesc{GpuPassKind::GeometryResample});
  plan.passes.push_back(GpuPassDesc{GpuPassKind::CameraToAp1});
  for (const auto& edge : document.Graph().Edges()) {
    if (edge.to_node == grade->Id() && edge.to_port == PortId{"mask"}) {
      const auto* mask = document.Graph().FindNode(edge.from_node);
      if (mask == nullptr) throw std::runtime_error("GraphCompiler: mask edge has no source");
      const auto kind = mask->Type() == type_ids::RasterMaskNode() ? CompiledMaskKind::Raster
                                                                   : CompiledMaskKind::Analytic;
      plan.primary_grade_mask = CompiledMask{mask->Id(), kind};
      plan.mask_output        = GraphValueId{mask->Id(), PortId{"mask"}};
      plan.passes.push_back(GpuPassDesc{GpuPassKind::MaskEvaluate});
      plan.passes.push_back(GpuPassDesc{GpuPassKind::MaskFeather});
      break;
    }
  }
  plan.passes.push_back(GpuPassDesc{GpuPassKind::PrimaryColorGrade});
  plan.primary_grade_output = GraphValueId{grade->Id(), PortId{"image"}};
  plan.display_output       = GraphValueId{document.Drt()->Id(), PortId{"display"}};
  plan.passes.push_back(GpuPassDesc{GpuPassKind::Drt});

  for (std::size_t index = 0; index < grade->AdjustmentCount(); ++index) {
    plan.primary_grade_adjustments.push_back(
        {grade->AdjustmentIdAt(index), grade->AdjustmentAt(index).Type()});
  }

  const auto geom_source =
      MakeSourceGeometry(source.develop_output_extent, source.full_reference_extent,
                         source.sensor_active_area, source.downsample_passes);
  plan.geometry = ResolveRenderGeometry(geom_source, ImageParamsFromDocument(document),
                                        request.view, request.resolution, request.footprint);
  plan.encode_geometry_resample = !IsIdentityResample(plan.geometry);
  return plan;
}

auto GraphCompiler::NeedsRecompile(const ExecutionPlan& previous, const PipelineDocument& document,
                                   const DevelopCompileSource& source) -> bool {
  if (document.TopologyDirty()) {
    return true;
  }
  return previous.source.kind != source.kind || previous.source.host_extent != source.host_extent ||
         previous.source.develop_output_extent != source.develop_output_extent ||
         previous.source.full_reference_extent != source.full_reference_extent ||
         previous.source.downsample_passes != source.downsample_passes;
}

}  // namespace alcedo
