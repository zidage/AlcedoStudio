//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "prepared_raw_test_support.hpp"

namespace alcedo {
namespace {

TEST(GpuDagRawInput, RawInputLoaderUnpacksBeforePipelineBuild) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern,
      gpu_dag_test::DefaultLinearization(), gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);

  auto document = CreateDefaultPipelineDocument();
  const auto plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  EXPECT_FALSE(prepared.host_extent.Empty());
  EXPECT_FALSE(plan.Contains(GpuPassKind::UploadRgb));
  EXPECT_TRUE(plan.Contains(GpuPassKind::UploadRaw));
  for (const auto& pass : plan.passes) {
    const char* name = GpuPassKindName(pass.kind);
    EXPECT_EQ(std::string(name).find("LibRaw"), std::string::npos);
    EXPECT_EQ(std::string(name).find("DecodeRes"), std::string::npos);
  }
}

TEST(GpuDagRawInput, RawInputLoaderDownsampleUpdatesCfaPatternAndPhase) {
  const auto xtrans = gpu_dag_test::MakeXTransPattern();
  const auto full = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, xtrans), xtrans, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  const auto half = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, xtrans), xtrans, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::HALF);

  EXPECT_EQ(full.host_extent, (Extent2D{64, 64}));
  EXPECT_EQ(half.host_extent, (Extent2D{32, 32}));
  EXPECT_EQ(half.downsample_passes, 1);
  EXPECT_NE(std::memcmp(full.cfa_pattern.xtrans_pattern.raw_fc, half.cfa_pattern.xtrans_pattern.raw_fc,
                        sizeof(full.cfa_pattern.xtrans_pattern.raw_fc)),
            0);

  const auto bayer = gpu_dag_test::MakeRggbPattern();
  const auto bayer_full = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, bayer), bayer, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  const auto bayer_half = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, bayer), bayer, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::HALF);
  EXPECT_EQ(bayer_half.host_extent, (Extent2D{32, 32}));
  EXPECT_EQ(std::memcmp(bayer_full.cfa_pattern.bayer_pattern.raw_fc,
                        bayer_half.cfa_pattern.bayer_pattern.raw_fc,
                        sizeof(bayer_full.cfa_pattern.bayer_pattern.raw_fc)),
            0);
}

TEST(GpuDagRawInput, PreparedRawInputKeepsFullReferenceExtentAcrossDecodeRes) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto full = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern,
      gpu_dag_test::DefaultLinearization(), gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  const auto half = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern,
      gpu_dag_test::DefaultLinearization(), gpu_dag_test::FullSensor(64, 64), DecodeRes::HALF);

  EXPECT_EQ(full.full_reference_extent, half.full_reference_extent);
  EXPECT_NE(full.develop_output_extent, half.develop_output_extent);
  EXPECT_EQ(full.full_reference_extent, full.develop_output_extent);
}

TEST(GpuDagRawInput, DirectRgbInputBypassesLibRawAndEntersDevelopEndpoint) {
  auto prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(48, 32),
                                                gpu_dag_test::FullSensor(48, 32));
  EXPECT_EQ(prepared.input_kind, RawInputKind::DebayeredRgb);
  EXPECT_EQ(prepared.CompileSource().kind, DevelopInputKind::DirectRgb);

  auto document = CreateDefaultPipelineDocument();
  const auto plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  EXPECT_TRUE(plan.Contains(GpuPassKind::UploadRgb));
  EXPECT_FALSE(plan.Contains(GpuPassKind::UploadRaw));
  EXPECT_FALSE(plan.Contains(GpuPassKind::Linearize));
  EXPECT_FALSE(plan.Contains(GpuPassKind::Demosaic));
}

TEST(GpuDagRawInput, UnsupportedCfaDoesNotProducePreparedRawInput) {
  RawCfaPattern bad;
  bad.kind = RawCfaKind::Bayer2x2;
  bad.bayer_pattern.raw_fc[0] = 0;
  bad.bayer_pattern.raw_fc[1] = 0;
  bad.bayer_pattern.raw_fc[2] = 0;
  bad.bayer_pattern.raw_fc[3] = 0;
  EXPECT_THROW(
      {
        auto prepared = RawInputLoader::FromUnpackedCfa(
            gpu_dag_test::MakeU16CfaPlane(16, 16, bad), bad, gpu_dag_test::DefaultLinearization(),
            gpu_dag_test::FullSensor(16, 16), DecodeRes::FULL);
        (void)prepared;
      },
      std::runtime_error);

  std::byte empty{};
  EXPECT_FALSE(RawInputLoader::TryLoadEncoded(std::span<const std::byte>(&empty, 0), DecodeRes::FULL)
                   .has_value());
}

TEST(GpuDagRawInput, InputHeadersDoNotIncludeGpuOrImageBuffer) {
  const auto root = std::filesystem::path{ALCEDO_INPUT_HEADER_ROOT};
  ASSERT_TRUE(std::filesystem::exists(root));
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".hpp") {
      continue;
    }
    std::ifstream input(entry.path());
    std::string   line;
    while (std::getline(input, line)) {
      EXPECT_EQ(line.find("image_buffer.hpp"), std::string::npos) << entry.path();
      EXPECT_EQ(line.find("cuda_runtime"), std::string::npos) << entry.path();
    }
  }
}

}  // namespace
}  // namespace alcedo
