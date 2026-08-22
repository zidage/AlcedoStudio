//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <string>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "../input/prepared_raw_test_support.hpp"

namespace alcedo {
namespace {

TEST(GpuDagGraphCompiler, GraphCompilerEmitsNoLibRawOrDecodeResPass) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern,
      gpu_dag_test::DefaultLinearization(), gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  const auto plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  for (const auto& pass : plan.passes) {
    const std::string name = GpuPassKindName(pass.kind);
    EXPECT_EQ(name.find("LibRaw"), std::string::npos);
    EXPECT_EQ(name.find("DecodeRes"), std::string::npos);
    EXPECT_EQ(name.find("CameraToAp1"), std::string::npos);
    EXPECT_NE(pass.kind, static_cast<GpuPassKind>(255));
  }
  EXPECT_FALSE(plan.Contains(static_cast<GpuPassKind>(99)));
}

TEST(GpuDagGraphCompiler, GraphCompilerPlacesHighlightRecoverAfterDemosaic) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern,
      gpu_dag_test::DefaultLinearization(), gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  const auto plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  const int demosaic = plan.IndexOf(GpuPassKind::Demosaic);
  const int hlr      = plan.IndexOf(GpuPassKind::HighlightRecover);
  ASSERT_GE(demosaic, 0);
  ASSERT_GE(hlr, 0);
  EXPECT_LT(demosaic, hlr);
  EXPECT_LT(plan.IndexOf(GpuPassKind::Linearize), demosaic);
  EXPECT_TRUE(plan.Contains(GpuPassKind::GeometryResample));
}

TEST(GpuDagGraphCompiler, HighlightFlagDoesNotChangePassList) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern,
      gpu_dag_test::DefaultLinearization(), gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  document.Develop()->Params().ReplaceParams([] {
    DevelopPayload p;
    p.highlights_reconstruct = false;
    return p;
  }());
  const auto plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  EXPECT_TRUE(plan.Contains(GpuPassKind::HighlightRecover));
  EXPECT_TRUE(plan.Contains(GpuPassKind::CfaClamp));
}

}  // namespace
}  // namespace alcedo
