//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/pass_kind.hpp"

namespace alcedo {
namespace {

auto HasCode(const std::vector<GraphValidationError>& errors, GraphValidationCode code) -> bool {
  for (const auto& error : errors) {
    if (error.code == code) {
      return true;
    }
  }
  return false;
}

auto DocumentJson(const PipelineDocument& document) -> std::string { return document.ToJson().dump(); }

auto DummyCompileSource() -> DevelopCompileSource {
  DevelopCompileSource source;
  source.kind                   = DevelopInputKind::DirectRgb;
  source.host_extent            = Extent2D{8, 8};
  source.develop_output_extent = Extent2D{8, 8};
  source.full_reference_extent  = Extent2D{8, 8};
  return source;
}

auto SceneImagePredecessorId(const PipelineDocument& document, const NodeId& node_id) -> NodeId {
  for (const auto& edge : document.Graph().Edges()) {
    if (edge.to_node == node_id && edge.to_port == PortId{"image"}) {
      return edge.from_node;
    }
  }
  return {};
}

auto SceneImageSuccessorId(const PipelineDocument& document, const NodeId& node_id) -> NodeId {
  for (const auto& edge : document.Graph().Edges()) {
    if (edge.from_node == node_id && edge.from_port == PortId{"image"}) {
      return edge.to_node;
    }
  }
  return {};
}

}  // namespace

TEST(GpuDagModelGraph, AddCleanColorGradeKeepsSingleImageBackbone) {
  auto document = CreateDefaultPipelineDocument();
  const NodeId added{"grade.extra"};
  EXPECT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, added).empty());

  const auto* node = document.Graph().FindNode(added);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->Id(), added);
  EXPECT_EQ(node->Type(), type_ids::ColorGradeNode());

  const auto path = document.Graph().ImageBackboneNodeIds();
  ASSERT_EQ(path.size(), 4u);
  EXPECT_EQ(path[0], NodeId{"develop"});
  EXPECT_EQ(path[1], NodeId{"grade.primary"});
  EXPECT_EQ(path[2], added);
  EXPECT_EQ(path[3], NodeId{"drt"});
  EXPECT_TRUE(document.Graph().Validate().empty());
  EXPECT_TRUE(document.Graph().ValidateImageBackbone().empty());
}

TEST(GpuDagModelGraph, RemoveColorGradeAndBridgeConnectsPredecessorToSuccessor) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  const NodeId predecessor = SceneImagePredecessorId(document, NodeId{"grade.b"});
  const NodeId successor     = SceneImageSuccessorId(document, NodeId{"grade.b"});
  EXPECT_EQ(predecessor, NodeId{"grade.primary"});
  EXPECT_EQ(successor, NodeId{"drt"});

  EXPECT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.b"}).empty());
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.b"}), nullptr);
  EXPECT_EQ(SceneImageSuccessorId(document, predecessor), successor);
  EXPECT_EQ(SceneImagePredecessorId(document, successor), predecessor);
  for (const auto& edge : document.Graph().Edges()) {
    EXPECT_NE(edge.from_node, NodeId{"grade.b"});
    EXPECT_NE(edge.to_node, NodeId{"grade.b"});
  }
  EXPECT_TRUE(document.Graph().Validate().empty());
  EXPECT_TRUE(document.Graph().ValidateImageBackbone().empty());
}

TEST(GpuDagModelGraph, RemovePrimaryGradeKeepsRemainingGradesAndValidBackbone) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  EXPECT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.primary"}).empty());

  EXPECT_EQ(document.Graph().FindNode("grade.primary"), nullptr);
  const auto* remaining = document.Graph().FindNode("grade.b");
  ASSERT_NE(remaining, nullptr);
  EXPECT_EQ(remaining->Id(), NodeId{"grade.b"});

  const auto path = document.Graph().ImageBackboneNodeIds();
  ASSERT_EQ(path.size(), 3u);
  EXPECT_EQ(path[0], NodeId{"develop"});
  EXPECT_EQ(path[1], NodeId{"grade.b"});
  EXPECT_EQ(path[2], NodeId{"drt"});
  EXPECT_EQ(document.PrimaryGrade(), nullptr);
  EXPECT_TRUE(document.Graph().ValidateImageBackbone().empty());
}

TEST(GpuDagModelGraph, RemoveLastColorGradeLeavesDevelopConnectedToDrt) {
  auto document = CreateDefaultPipelineDocument();
  EXPECT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.primary"}).empty());
  EXPECT_EQ(document.Graph().FindNode("grade.primary"), nullptr);

  const auto path = document.Graph().ImageBackboneNodeIds();
  ASSERT_EQ(path.size(), 2u);
  EXPECT_EQ(path[0], NodeId{"develop"});
  EXPECT_EQ(path[1], NodeId{"drt"});
  EXPECT_EQ(SceneImageSuccessorId(document, NodeId{"develop"}), NodeId{"drt"});
  EXPECT_EQ(SceneImagePredecessorId(document, NodeId{"drt"}), NodeId{"develop"});
  EXPECT_TRUE(document.Graph().Validate().empty());
  EXPECT_TRUE(document.Graph().ValidateImageBackbone().empty());
}

TEST(GpuDagModelGraph, RemoveDevelopOrDrtIsRejectedWithoutMutation) {
  auto document = CreateDefaultPipelineDocument();
  const auto before = DocumentJson(document);
  const auto nodes  = document.Graph().NodeCount();

  const auto develop_errors = RemoveColorGradeAndBridge(document, NodeId{"develop"});
  EXPECT_TRUE(HasCode(develop_errors, GraphValidationCode::ProtectedEndpoint));
  EXPECT_EQ(DocumentJson(document), before);
  EXPECT_EQ(document.Graph().NodeCount(), nodes);

  const auto drt_errors = RemoveColorGradeAndBridge(document, NodeId{"drt"});
  EXPECT_TRUE(HasCode(drt_errors, GraphValidationCode::ProtectedEndpoint));
  EXPECT_EQ(DocumentJson(document), before);
  EXPECT_EQ(document.Graph().NodeCount(), nodes);
}

TEST(GpuDagModelGraph, InvalidReconnectLeavesDocumentHashUnchanged) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  const auto before = DocumentJson(document);

  const auto errors =
      ReconnectColorGrade(document, NodeId{"grade.primary"}, NodeId{"develop"}, NodeId{"drt"});
  EXPECT_FALSE(errors.empty());
  EXPECT_EQ(DocumentJson(document), before);
  EXPECT_EQ(document.Graph().FindNode("grade.b")->Id(), NodeId{"grade.b"});
}

TEST(GpuDagModelGraph, GraphMutationInverseRestoresCanonicalDocumentJson) {
  auto document = CreateDefaultPipelineDocument();
  const auto original = DocumentJson(document);
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  EXPECT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.b"}).empty());
  EXPECT_EQ(DocumentJson(document), original);

  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  const auto two_grades = DocumentJson(document);
  EXPECT_TRUE(ReconnectColorGrade(document, NodeId{"grade.b"}, NodeId{"develop"},
                                  NodeId{"grade.primary"})
                  .empty());
  EXPECT_TRUE(ReconnectColorGrade(document, NodeId{"grade.b"}, NodeId{"grade.primary"},
                                  NodeId{"drt"})
                  .empty());
  EXPECT_EQ(DocumentJson(document), two_grades);

  auto* grade = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode("grade.primary"));
  ASSERT_NE(grade, nullptr);
  const std::string previous_name{grade->DisplayName()};
  EXPECT_TRUE(RenameColorGrade(document, NodeId{"grade.primary"}, "Look A").empty());
  EXPECT_EQ(document.Graph().FindNode("grade.primary")->Id(), NodeId{"grade.primary"});
  EXPECT_TRUE(RenameColorGrade(document, NodeId{"grade.primary"}, previous_name).empty());
  EXPECT_EQ(DocumentJson(document), two_grades);

  EXPECT_TRUE(SetColorGradeEnabled(document, NodeId{"grade.b"}, false).empty());
  EXPECT_TRUE(SetColorGradeEnabled(document, NodeId{"grade.b"}, true).empty());
  EXPECT_EQ(DocumentJson(document), two_grades);
}

TEST(GpuDagModelGraph, AddCleanColorGradeDoesNotCopyDefaultExposureOrSaturation) {
  auto document = CreateDefaultPipelineDocument();
  const auto* primary = document.PrimaryGrade();
  ASSERT_NE(primary, nullptr);
  const auto* primary_exposure = dynamic_cast<const ExposureModel*>(
      primary->FindAdjustmentByType(type_ids::Exposure()));
  const auto* primary_saturation = dynamic_cast<const SaturationModel*>(
      primary->FindAdjustmentByType(type_ids::Saturation()));
  ASSERT_NE(primary_exposure, nullptr);
  ASSERT_NE(primary_saturation, nullptr);
  EXPECT_FLOAT_EQ(primary_exposure->Value(), 1.5f);
  EXPECT_FLOAT_EQ(primary_saturation->Value(), 1.3f);

  const NodeId added{"grade.extra"};
  EXPECT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, added).empty());
  const auto* inserted =
      dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(added));
  ASSERT_NE(inserted, nullptr);
  const auto* exposure = dynamic_cast<const ExposureModel*>(
      inserted->FindAdjustmentByType(type_ids::Exposure()));
  const auto* saturation = dynamic_cast<const SaturationModel*>(
      inserted->FindAdjustmentByType(type_ids::Saturation()));
  ASSERT_NE(exposure, nullptr);
  ASSERT_NE(saturation, nullptr);
  EXPECT_FLOAT_EQ(exposure->Value(), 0.0f);
  EXPECT_FLOAT_EQ(saturation->Value(), 1.0f);
  EXPECT_EQ(inserted->FindAdjustmentByType(type_ids::Clarity()), nullptr);
  EXPECT_EQ(inserted->FindAdjustmentByType(type_ids::Sharpen()), nullptr);
  EXPECT_EQ(inserted->FindAdjustmentByType(type_ids::Halation()), nullptr);
  EXPECT_EQ(inserted->FindAdjustmentByType(type_ids::FilmGrain()), nullptr);
}

TEST(GpuDagModelGraph, GraphCompilerSkipsGradePassWhenBackboneHasNoColorGrade) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.primary"}).empty());
  const auto plan = GraphCompiler::CompileStatic(document, DummyCompileSource());
  EXPECT_FALSE(plan.Contains(GpuPassKind::PrimaryColorGrade));
  EXPECT_TRUE(plan.Contains(GpuPassKind::Drt));
  EXPECT_TRUE(plan.primary_grade_adjustments.empty());
  EXPECT_EQ(plan.primary_grade_output.producer, NodeId{"develop"});
}

TEST(GpuDagModelGraph, GraphCompilerCompilesFirstBackboneGradeWhenPrimaryIdIsAbsent) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  ASSERT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.primary"}).empty());
  ASSERT_EQ(document.PrimaryGrade(), nullptr);

  const auto* remaining =
      dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode("grade.b"));
  ASSERT_NE(remaining, nullptr);
  const auto plan = GraphCompiler::CompileStatic(document, DummyCompileSource());
  EXPECT_TRUE(plan.Contains(GpuPassKind::PrimaryColorGrade));
  EXPECT_EQ(plan.primary_grade_output.producer, NodeId{"grade.b"});
  ASSERT_EQ(plan.primary_grade_adjustments.size(), remaining->AdjustmentCount());
  for (std::size_t i = 0; i < remaining->AdjustmentCount(); ++i) {
    EXPECT_EQ(plan.primary_grade_adjustments[i].instance_id, remaining->AdjustmentIdAt(i));
    EXPECT_EQ(plan.primary_grade_adjustments[i].type, remaining->AdjustmentAt(i).Type());
  }
}

}  // namespace alcedo
