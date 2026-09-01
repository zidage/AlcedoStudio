//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/graph_compiler.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/i_node_model.hpp"
#include "edit/graph/pipeline_graph.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_type_id.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"

namespace alcedo {
namespace {

constexpr std::size_t kAlign = 256;

auto                  AlignUp(std::size_t value, std::size_t alignment) -> std::size_t {
  return (value + alignment - 1) & ~(alignment - 1);
}

auto EstimateMaskSdfTransientBytes(Extent2D extent) -> std::size_t {
  const std::size_t pixels =
      static_cast<std::size_t>(std::max(extent.width, 1u)) * std::max(extent.height, 1u);
  return 5 * AlignUp(pixels * sizeof(float), kAlign);
}

auto PlaneBytes(std::size_t pixels, std::size_t bytes_per_pixel) -> std::size_t {
  return AlignUp(pixels * bytes_per_pixel, kAlign);
}

auto EstimateDevelopTransientBytes(const DevelopCompileSource& source) -> std::size_t {
  if (source.kind == DevelopInputKind::DirectRgb) {
    const auto pixels =
        static_cast<std::size_t>(source.host_extent.width) * source.host_extent.height;
    // Uploaded RGBA + linear RGB + HLR RGB and aligned reduction scalars.
    return PlaneBytes(pixels, 16) + 2 * PlaneBytes(pixels, 12) + 4 * kAlign;
  }
  const std::size_t pixels =
      static_cast<std::size_t>(source.host_extent.width) * source.host_extent.height;
  const std::size_t cfa_u16 = PlaneBytes(pixels, 2);
  const std::size_t cfa_f32 = PlaneBytes(pixels, 4);
  const std::size_t stats   = AlignUp(4 + 16 + 16, kAlign);
  const std::size_t pad     = 64 * kAlign;
  if (source.kind == DevelopInputKind::XTransCfa) {
    // U16 CFA + F32 CFA + green + RGB + HLR RGB. HLR coexists with the RGB plane.
    return cfa_u16 + cfa_f32 + PlaneBytes(pixels, 4) + PlaneBytes(pixels, 12) +
           PlaneBytes(pixels, 12) + stats + pad;
  }
  // Bayer / Neural-on-Bayer: U16 CFA + F32 CFA + 5 RCD planes + merge-or-HLR RGB.
  // Merge and HLR are exclusive; both are 12 bytes/pixel.
  return cfa_u16 + cfa_f32 + 5 * PlaneBytes(pixels, 4) + PlaneBytes(pixels, 12) + stats + pad;
}

auto EstimatePeakTransientBytes(const DevelopCompileSource& source) -> std::size_t {
  const std::size_t llf = local_tone_mapping::EstimateLlfTransientBytes(
      static_cast<int>(source.full_reference_extent.width),
      static_cast<int>(source.full_reference_extent.height), kAlign);
  return (std::max)(EstimateDevelopTransientBytes(source), llf);
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

auto CompileAdjustmentAlgorithm(const OperatorTypeId& type) -> CompiledAdjustmentAlgorithm {
  if (type == type_ids::Shadows() || type == type_ids::Highlights()) {
    return CompiledAdjustmentAlgorithm::LocalLaplacian;
  }
  if (type == type_ids::Clarity() || type == type_ids::Sharpen() || type == type_ids::Halation() ||
      type == type_ids::FilmGrain()) {
    return CompiledAdjustmentAlgorithm::Neighborhood;
  }
  return CompiledAdjustmentAlgorithm::Pointwise;
}

auto StageKindFor(CompiledAdjustmentAlgorithm algorithm) -> CompiledGradeStageKind {
  switch (algorithm) {
    case CompiledAdjustmentAlgorithm::LocalLaplacian:
      return CompiledGradeStageKind::LocalLaplacian;
    case CompiledAdjustmentAlgorithm::Neighborhood:
      return CompiledGradeStageKind::Neighborhood;
    case CompiledAdjustmentAlgorithm::Pointwise:
      return CompiledGradeStageKind::Pointwise;
  }
  return CompiledGradeStageKind::Pointwise;
}

void AppendGradeStage(CompiledGradeNode& grade, CompiledAdjustmentAlgorithm algorithm) {
  const auto kind  = StageKindFor(algorithm);
  const auto index = static_cast<std::uint32_t>(grade.adjustments.size() - 1);
  if (!grade.stages.empty()) {
    auto& last = grade.stages.back();
    if (last.kind == kind && kind != CompiledGradeStageKind::Neighborhood) {
      ++last.count;
      return;
    }
  }
  grade.stages.push_back(CompiledGradeStage{kind, index, 1});
}

auto HashGraphTopology(const PipelineDocument& document) -> std::uint64_t {
  std::uint64_t                  hash = MixU64(14695981039346656037ull, 1);

  std::vector<const INodeModel*> nodes;
  nodes.reserve(document.Graph().Nodes().size());
  for (const auto& node : document.Graph().Nodes()) {
    nodes.push_back(node.get());
  }
  std::sort(nodes.begin(), nodes.end(),
            [](const INodeModel* a, const INodeModel* b) { return a->Id() < b->Id(); });
  for (const auto* node : nodes) {
    hash              = MixText(hash, node->Id().Value());
    hash              = MixText(hash, node->Type().Text());
    const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(node);
    if (grade != nullptr) {
      hash = MixU64(hash, grade->AdjustmentCount());
      for (std::size_t index = 0; index < grade->AdjustmentCount(); ++index) {
        hash = MixText(hash, grade->AdjustmentIdAt(index).Value());
        hash = MixText(hash, grade->AdjustmentAt(index).Type().Text());
      }
      std::vector<const MaskModel*> masks;
      masks.reserve(grade->MaskCount());
      for (const auto& mask : grade->Masks()) {
        masks.push_back(&mask);
      }
      std::sort(masks.begin(), masks.end(), [](const MaskModel* a, const MaskModel* b) {
        return a->id < b->id;
      });
      hash = MixU64(hash, masks.size());
      for (const auto* mask : masks) {
        hash = MixText(hash, mask->id.Value());
        hash = MixText(hash, MaskSourceKindText(GetMaskSourceKind(mask->source)));
        hash = MixU64(hash, mask->color_range.has_value() ? 1 : 0);
        hash = MixU64(hash, mask->luminance_range.has_value() ? 1 : 0);
      }
      continue;
    }
    const auto* drt = dynamic_cast<const DrtNodeModel*>(node);
    if (drt == nullptr) {
      continue;
    }
    hash = MixU64(hash, drt->AdjustmentCount());
    for (std::size_t index = 0; index < drt->AdjustmentCount(); ++index) {
      hash = MixText(hash, drt->AdjustmentIdAt(index).Value());
      hash = MixText(hash, drt->AdjustmentAt(index).Type().Text());
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

void RequireValidGraph(const PipelineDocument& document) {
  const auto errors = document.Graph().Validate();
  if (!errors.empty()) {
    throw std::runtime_error("GraphCompiler: graph is invalid: " + errors.front().message);
  }
  const auto backbone = document.Graph().ValidateImageBackbone();
  if (!backbone.empty()) {
    throw std::runtime_error("GraphCompiler: image backbone is invalid: " +
                             backbone.front().message);
  }
  if (document.Develop() == nullptr) {
    throw std::runtime_error("GraphCompiler: missing develop endpoint");
  }
  if (document.Drt() == nullptr) {
    throw std::runtime_error("GraphCompiler: missing DRT endpoint");
  }
  for (const auto& node : document.Graph().Nodes()) {
    const auto& type = node->Type();
    if (type == type_ids::DevelopNode() || type == type_ids::ColorGradeNode() ||
        type == type_ids::DrtNode()) {
      continue;
    }
    throw std::runtime_error("GraphCompiler: unsupported GPU DAG node");
  }
}

auto NextOrdinal(const ExecutionPlan& plan, GpuPassKind kind) -> std::uint32_t {
  std::uint32_t ordinal = 0;
  for (const auto& pass : plan.passes) {
    if (pass.kind == kind) {
      ++ordinal;
    }
  }
  return ordinal;
}

void PushPass(ExecutionPlan& plan, GpuPassKind kind, NodeId owner,
              std::vector<CompiledPassInput> inputs, std::vector<CompiledPassOutput> outputs,
              std::optional<AdjustmentInstanceId> adjustment = {},
              std::vector<AdjustmentInstanceId>   parameters = {}, MaskId mask_id = {}) {
  const auto ordinal = NextOrdinal(plan, kind);
  plan.passes.push_back(MakeGpuPass(kind, std::move(owner), ordinal, std::move(inputs),
                                    std::move(outputs), std::move(adjustment),
                                    std::move(parameters), std::move(mask_id)));
}

auto SceneImageIncoming(const PipelineDocument& document, const NodeId& node_id) -> GraphValueId {
  for (const auto& edge : document.Graph().Edges()) {
    if (edge.to_node == node_id && edge.to_port == PortId{"image"}) {
      return GraphValueId{edge.from_node, edge.from_port};
    }
  }
  throw std::runtime_error("GraphCompiler: missing scene-image incoming edge for " +
                           std::string{node_id.Value()});
}

auto CompileGradeOwnedMaskStack(const ColorGradeNodeModel& grade, GraphValueId scene_input,
                                const DevelopCompileSource& source, ExecutionPlan& plan)
    -> std::optional<CompiledMaskStack> {
  if (grade.MaskCount() == 0) {
    return std::nullopt;
  }

  CompiledMaskStack stack;
  stack.owner_node_id = grade.Id();
  stack.union_output  = MaskUnionValue(grade.Id());
  stack.sources.reserve(grade.MaskCount());
  for (const auto& mask : grade.Masks()) {
    CompiledMaskSource compiled;
    compiled.owner_node_id    = grade.Id();
    compiled.mask_id          = mask.id;
    compiled.source_kind      = GetMaskSourceKind(mask.source);
    compiled.source_output    = MaskSourceValue(grade.Id(), mask.id);
    compiled.feather_output   = compiled.source_output;
    compiled.effective_output = compiled.source_output;
    compiled.range_input      = scene_input;
    stack.sources.push_back(std::move(compiled));
  }
  std::sort(stack.sources.begin(), stack.sources.end(),
            [](const CompiledMaskSource& lhs, const CompiledMaskSource& rhs) {
              return lhs.mask_id < rhs.mask_id;
            });

  std::vector<CompiledPassInput> union_inputs;
  union_inputs.reserve(stack.sources.size());
  for (const auto& compiled : stack.sources) {
    PushPass(plan, GpuPassKind::MaskEvaluate, grade.Id(),
             {{PortId{"image"}, plan.geometry_output, CompiledValueKind::SceneImage},
              {PortId{"range"}, compiled.range_input, CompiledValueKind::SceneImage}},
             {{compiled.source_output, CompiledValueKind::Mask}}, {}, {}, compiled.mask_id);
    union_inputs.push_back({PortId{std::string{compiled.mask_id.Value()}}, compiled.effective_output,
                            CompiledValueKind::Mask});
    if (compiled.source_kind == MaskSourceKind::Brush) {
      const Extent2D mask_extent{
          std::max(source.full_reference_extent.width, source.host_extent.width),
          std::max(source.full_reference_extent.height, source.host_extent.height)};
      plan.peak_transient_bytes =
          (std::max)(plan.peak_transient_bytes, EstimateMaskSdfTransientBytes(mask_extent));
    }
  }
  PushPass(plan, GpuPassKind::MaskUnion, grade.Id(), std::move(union_inputs),
           {{stack.union_output, CompiledValueKind::Mask}});
  return stack;
}

auto CompileColorGrade(const PipelineDocument& document, const ColorGradeNodeModel& grade,
                       GraphValueId scene_input, const DevelopCompileSource& source,
                       ExecutionPlan& plan) -> CompiledGradeNode {
  const auto incoming = SceneImageIncoming(document, grade.Id());
  if (incoming != scene_input) {
    throw std::runtime_error(
        "GraphCompiler: Color Grade scene input does not follow the image backbone");
  }

  CompiledGradeNode compiled;
  compiled.node_id      = grade.Id();
  compiled.scene_input  = scene_input;
  compiled.scene_output = GraphValueId{grade.Id(), PortId{"image"}};
  compiled.mask_stack   = CompileGradeOwnedMaskStack(grade, scene_input, source, plan);
  if (compiled.mask_stack.has_value()) {
    compiled.mask_output = compiled.mask_stack->union_output;
  }

  std::vector<AdjustmentInstanceId> parameters;
  parameters.reserve(grade.AdjustmentCount());
  compiled.adjustments.reserve(grade.AdjustmentCount());
  for (std::size_t index = 0; index < grade.AdjustmentCount(); ++index) {
    const auto& type = grade.AdjustmentAt(index).Type();
    RequireAdjustmentOwner(type, AdjustmentParameterOwner::ColorGrade, "GraphCompiler Color Grade");
    const auto algorithm = CompileAdjustmentAlgorithm(type);
    const auto instance  = grade.AdjustmentIdAt(index);
    compiled.adjustments.push_back({instance, type, algorithm});
    AppendGradeStage(compiled, algorithm);
    parameters.push_back(instance);
  }

  std::vector<CompiledPassInput> inputs{
      {PortId{"image"}, compiled.scene_input, CompiledValueKind::SceneImage}};
  if (compiled.mask_stack.has_value()) {
    inputs.push_back({PortId{"mask"}, compiled.mask_output, CompiledValueKind::Mask});
  }
  PushPass(plan, GpuPassKind::PrimaryColorGrade, grade.Id(), std::move(inputs),
           {{compiled.scene_output, CompiledValueKind::SceneImage}}, {}, std::move(parameters));
  return compiled;
}

auto CompileDrt(const DrtNodeModel& drt, GraphValueId scene_input, ExecutionPlan& plan)
    -> CompiledDrtNode {
  CompiledDrtNode compiled;
  compiled.node_id        = drt.Id();
  compiled.scene_input    = scene_input;
  compiled.scene_output   = GraphValueId{drt.Id(), PortId{"runtime.scene_post"}};
  compiled.display_output = GraphValueId{drt.Id(), PortId{"display"}};

  std::vector<OperatorTypeId>         drt_types;
  std::vector<AdjustmentInstanceId>   parameters;
  drt_types.reserve(drt.AdjustmentCount());
  parameters.reserve(drt.AdjustmentCount());
  compiled.post_adjustments.reserve(drt.AdjustmentCount());
  GraphValueId step_input = scene_input;
  for (std::size_t index = 0; index < drt.AdjustmentCount(); ++index) {
    const auto& type = drt.AdjustmentAt(index).Type();
    drt_types.push_back(type);
    const auto algorithm = CompileAdjustmentAlgorithm(type);
    if (algorithm != CompiledAdjustmentAlgorithm::Neighborhood) {
      throw std::runtime_error("GraphCompiler: DRT/Post adjustment is not a neighborhood operation");
    }
    const auto instance = drt.AdjustmentIdAt(index);
    compiled.post_adjustments.push_back({instance, type, algorithm});
    parameters.push_back(instance);
    const GraphValueId step_output{drt.Id(), PortId{"runtime.post." + std::to_string(index)}};
    compiled.steps.push_back(CompiledDrtStep{CompiledDrtStepKind::Neighborhood, instance, type,
                                             step_input, step_output});
    step_input = step_output;
  }
  RequireCompleteDrtPostTypes(drt_types, "GraphCompiler DRT");
  compiled.steps.push_back(CompiledDrtStep{CompiledDrtStepKind::DisplayTransform, {},
                                           OperatorTypeId{}, compiled.scene_output,
                                           compiled.display_output});
  if (!compiled.steps.empty() && compiled.steps.size() >= 2) {
    compiled.steps[compiled.steps.size() - 2].output = compiled.scene_output;
    compiled.steps.back().input                      = compiled.scene_output;
  }

  PushPass(plan, GpuPassKind::Drt, drt.Id(),
           {{PortId{"image"}, compiled.scene_input, CompiledValueKind::SceneImage}},
           {{compiled.scene_output, CompiledValueKind::SceneImage},
            {compiled.display_output, CompiledValueKind::DisplayImage}},
           {}, std::move(parameters));
  return compiled;
}

void CompileDevelopPasses(ExecutionPlan& plan, const NodeId& develop_id,
                          const DevelopCompileSource& source) {
  const CompiledPassInput  sensor_in{PortId{"sensor_linear"}, plan.sensor_linear_output,
                                    CompiledValueKind::SceneImage};
  const CompiledPassOutput sensor_out{plan.sensor_linear_output, CompiledValueKind::SceneImage};
  if (source.kind == DevelopInputKind::DirectRgb) {
    PushPass(plan, GpuPassKind::UploadRgb, develop_id, {}, {sensor_out});
  } else {
    PushPass(plan, GpuPassKind::UploadRaw, develop_id, {}, {sensor_out});
    PushPass(plan, GpuPassKind::Linearize, develop_id, {sensor_in}, {sensor_out});
    PushPass(plan, GpuPassKind::CfaClamp, develop_id, {sensor_in}, {sensor_out});
    PushPass(plan, GpuPassKind::Demosaic, develop_id, {sensor_in}, {sensor_out});
    PushPass(plan, GpuPassKind::HighlightRecover, develop_id, {sensor_in}, {sensor_out});
    PushPass(plan, GpuPassKind::InverseCamMulPack, develop_id, {sensor_in}, {sensor_out});
  }
  PushPass(plan, GpuPassKind::Lens, develop_id, {sensor_in}, {sensor_out});
  PushPass(plan, GpuPassKind::GeometryResample, NodeId{"geometry"}, {sensor_in},
           {{plan.geometry_output, CompiledValueKind::SceneImage}});
  PushPass(plan, GpuPassKind::CameraToAp1, develop_id,
           {{PortId{"scene_source"}, plan.geometry_output, CompiledValueKind::SceneImage}},
           {{plan.develop_output, CompiledValueKind::SceneImage}});
}

}  // namespace

auto GraphCompiler::MakeStaticPlanKey(const PipelineDocument&     document,
                                      const DevelopCompileSource& source,
                                      std::uint32_t backend_capability_version) -> StaticPlanKey {
  StaticPlanKey key;
  key.topology_hash              = HashGraphTopology(document);
  key.source_layout              = source;
  key.backend_capability_version = backend_capability_version;
  return key;
}

auto GraphCompiler::CompileStatic(const PipelineDocument&     document,
                                  const DevelopCompileSource& source,
                                  std::uint32_t backend_capability_version) -> ExecutionPlan {
  RequireValidGraph(document);
  if (source.host_extent.Empty() || source.develop_output_extent.Empty() ||
      source.full_reference_extent.Empty()) {
    throw std::runtime_error("GraphCompiler: source extents must be positive");
  }

  ExecutionPlan plan;
  plan.static_key           = MakeStaticPlanKey(document, source, backend_capability_version);
  plan.source               = source;
  const auto* develop       = document.Develop();
  plan.sensor_linear_output = GraphValueId{develop->Id(), PortId{"sensor_linear"}};
  plan.geometry_output      = GraphValueId{NodeId{"geometry"}, PortId{"scene_source"}};
  plan.develop_output       = GraphValueId{develop->Id(), PortId{"image"}};
  plan.peak_transient_bytes = EstimatePeakTransientBytes(source);

  CompileDevelopPasses(plan, develop->Id(), source);

  GraphValueId scene = plan.develop_output;
  for (const auto& id : document.Graph().ImageBackboneNodeIds()) {
    const auto* node = document.Graph().FindNode(id);
    if (node == nullptr) {
      throw std::runtime_error("GraphCompiler: backbone node is missing");
    }
    const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(node);
    if (grade == nullptr) {
      continue;
    }
    auto compiled = CompileColorGrade(document, *grade, scene, source, plan);
    scene         = compiled.scene_output;
    plan.grade_nodes.push_back(std::move(compiled));
  }

  const auto* drt    = document.Drt();
  plan.drt           = CompileDrt(*drt, scene, plan);
  plan.display_output = plan.drt.display_output;
  if (plan.drt.scene_input != plan.SceneInputForDrt()) {
    throw std::runtime_error("GraphCompiler: DRT scene input is not the last backbone scene value");
  }

  ValidateExecutionPlan(plan);
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
