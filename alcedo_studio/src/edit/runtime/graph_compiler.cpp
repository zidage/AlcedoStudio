//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/graph_compiler.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/i_node_model.hpp"
#include "edit/graph/pipeline_graph.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_type_id.hpp"

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

auto MixU64(std::uint64_t hash, std::uint64_t value) -> std::uint64_t {
  hash ^= value;
  hash *= 1099511628211ull;
  return hash;
}

auto MixText(std::uint64_t hash, std::string_view text) -> std::uint64_t {
  hash = MixU64(hash, Fnv1a64(text));
  return MixU64(hash, 0xFFull);
}

auto HashGraphTopology(const PipelineDocument& document) -> std::uint64_t {
  std::uint64_t hash = MixU64(14695981039346656037ull, 1);

  std::vector<const INodeModel*> nodes;
  nodes.reserve(document.Graph().Nodes().size());
  for (const auto& node : document.Graph().Nodes()) {
    nodes.push_back(node.get());
  }
  std::sort(nodes.begin(), nodes.end(), [](const INodeModel* a, const INodeModel* b) {
    return a->Id() < b->Id();
  });
  for (const auto* node : nodes) {
    hash = MixText(hash, node->Id().Value());
    hash = MixText(hash, node->Type().Text());
    const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(node);
    if (grade == nullptr) {
      continue;
    }
    hash = MixU64(hash, grade->AdjustmentCount());
    for (std::size_t index = 0; index < grade->AdjustmentCount(); ++index) {
      hash = MixText(hash, grade->AdjustmentIdAt(index).Value());
      hash = MixText(hash, grade->AdjustmentAt(index).Type().Text());
    }
  }

  std::vector<GraphEdge> edges = document.Graph().Edges();
  std::sort(edges.begin(), edges.end(), [](const GraphEdge& a, const GraphEdge& b) {
    if (a.from_node != b.from_node) {
      return a.from_node < b.from_node;
    }
    if (a.from_port != b.from_port) {
      return a.from_port < b.from_port;
    }
    if (a.to_node != b.to_node) {
      return a.to_node < b.to_node;
    }
    return a.to_port < b.to_port;
  });
  for (const auto& edge : edges) {
    hash = MixText(hash, edge.from_node.Value());
    hash = MixText(hash, edge.from_port.Value());
    hash = MixText(hash, edge.to_node.Value());
    hash = MixText(hash, edge.to_port.Value());
  }
  return hash;
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

auto GraphCompiler::MakeStaticPlanKey(const PipelineDocument& document,
                                      const DevelopCompileSource& source,
                                      std::uint32_t backend_capability_version) -> StaticPlanKey {
  StaticPlanKey key;
  key.topology_hash              = HashGraphTopology(document);
  key.source_layout              = source;
  key.backend_capability_version = backend_capability_version;
  return key;
}

auto GraphCompiler::CompileStatic(const PipelineDocument& document,
                                  const DevelopCompileSource& source,
                                  std::uint32_t backend_capability_version) -> ExecutionPlan {
  RequireDefaultEndpoints(document);
  if (source.host_extent.Empty() || source.develop_output_extent.Empty() ||
      source.full_reference_extent.Empty()) {
    throw std::runtime_error("GraphCompiler: source extents must be positive");
  }

  ExecutionPlan plan;
  plan.static_key            = MakeStaticPlanKey(document, source, backend_capability_version);
  plan.source                = source;
  plan.sensor_linear_output  = GraphValueId{NodeId{"develop"}, PortId{"sensor_linear"}};
  plan.geometry_output       = GraphValueId{NodeId{"geometry"}, PortId{"scene_source"}};
  plan.develop_output        = GraphValueId{NodeId{"develop"}, PortId{"image"}};
  plan.peak_transient_bytes  = EstimatePeakTransientBytes(source);
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
  return plan;
}

void GraphCompiler::BindFrameGeometry(ExecutionPlan& plan, const PipelineDocument& document,
                                      const RenderRequest& request) {
  const auto geom_source =
      MakeSourceGeometry(plan.source.develop_output_extent, plan.source.full_reference_extent,
                         plan.source.sensor_active_area, plan.source.downsample_passes);
  plan.geometry = ResolveRenderGeometry(geom_source, ImageParamsFromDocument(document),
                                        request.view, request.resolution, request.footprint);
  plan.encode_geometry_resample = !IsIdentityResample(plan.geometry);
}

auto GraphCompiler::Compile(const PipelineDocument& document, const DevelopCompileSource& source,
                            const RenderRequest& request) -> ExecutionPlan {
  auto plan = CompileStatic(document, source);
  BindFrameGeometry(plan, document, request);
  return plan;
}

auto GraphCompiler::NeedsRecompile(const ExecutionPlan& previous, const PipelineDocument& document,
                                   const DevelopCompileSource& source) -> bool {
  return previous.static_key !=
         MakeStaticPlanKey(document, source, previous.static_key.backend_capability_version);
}

}  // namespace alcedo
