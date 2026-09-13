//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/result_persistence.hpp"
#include "multi_grade_runtime_test_support.hpp"
#include "utils/diagnostics/preview_performance.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kInteractiveMaxLongEdge = 2560;

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto PreviewDumpDirectory() -> std::filesystem::path {
  auto cwd = std::filesystem::current_path();
  for (int i = 0; i < 8; ++i) {
    if (std::filesystem::exists(cwd / "CMakePresets.json")) {
      auto dir = cwd / "build" / "tmp" / "preview_performance";
      std::filesystem::create_directories(dir);
      return dir;
    }
    const auto parent = cwd.parent_path();
    if (parent == cwd) {
      break;
    }
    cwd = parent;
  }
  auto dir = std::filesystem::current_path() / "build" / "tmp" / "preview_performance";
  std::filesystem::create_directories(dir);
  return dir;
}

auto FindPass(const diag::PreviewRequestRecord& record, std::string_view owner,
              diag::PreviewPassKind kind) -> const diag::PreviewPassRecord* {
  for (const auto& pass : record.passes) {
    if (pass.owner == owner && pass.kind == kind) {
      return &pass;
    }
  }
  return nullptr;
}

auto InteractiveRequest() -> RenderRequest {
  RenderRequest request;
  request.resolution.max_edge = kInteractiveMaxLongEdge;
  request.resolution.quality  = RenderQuality::Preview;
  return request;
}

auto LongEdge(const Extent2D& extent) -> std::uint32_t {
  return std::max(extent.width, extent.height);
}

auto LoadEncodedFile(const std::filesystem::path& path) -> PreparedRawInput {
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(static_cast<bool>(input)) << path.string();
  const std::vector<char> chars((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
  std::vector<std::byte>  bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  return RawInputLoader::LoadEncoded(bytes, DecodeRes::FULL);
}

auto FindFirstCiRaw() -> std::filesystem::path {
#if defined(ALCEDO_CI_RAW_FIXTURE_ROOT)
  const std::filesystem::path root{ALCEDO_CI_RAW_FIXTURE_ROOT};
  if (std::filesystem::exists(root)) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
      const auto ext = entry.path().extension().string();
      if (ext == ".ARW" || ext == ".arw" || ext == ".DNG" || ext == ".dng") {
        return entry.path();
      }
    }
  }
#endif
  return {};
}

auto FindXTransRaw() -> std::filesystem::path {
#if defined(ALCEDO_XTRANS_RAW_FIXTURE)
  const std::filesystem::path path{ALCEDO_XTRANS_RAW_FIXTURE};
  if (std::filesystem::exists(path)) {
    return path;
  }
#endif
  return {};
}

void DumpRecord(std::string_view name, const diag::PreviewRequestRecord& record) {
  const auto    path = PreviewDumpDirectory() / "cuda_interactive_2560_pass_table.txt";
  std::ofstream out(path, std::ios::app);
  out << name << " request=" << record.request_id
      << " gpu=" << diag::PreviewGpuTimeStatusName(record.gpu_status)
      << " gpu_ms=" << (static_cast<double>(record.gpu_ns) / 1.0e6)
      << " encode_ms=" << (static_cast<double>(record.cpu.encode_ns) / 1.0e6)
      << " render=" << record.render_width << "x" << record.render_height;
  if (record.has_develop_decode) {
    out << " develop=" << record.develop.develop_width << "x" << record.develop.develop_height
        << " cfa=" << diag::PreviewCfaKindName(record.develop.cfa) << " demosaic="
        << diag::PreviewDemosaicMethodName(record.develop.demosaic);
  }
  out << '\n';
  for (const auto& pass : record.passes) {
    out << "  " << pass.owner << " " << diag::PreviewPassKindName(pass.kind)
        << " state=" << diag::PreviewExecutionStateName(pass.state)
        << " cpu_ms=" << (static_cast<double>(pass.cpu_ns) / 1.0e6)
        << " gpu=" << diag::PreviewGpuTimeStatusName(pass.gpu_status)
        << " gpu_ms=" << (static_cast<double>(pass.gpu_ns) / 1.0e6) << '\n';
  }
}

void PopulateHeavyGrade(PipelineDocument& document, const NodeId& node, float ev) {
  multi_grade_test::GradeAdjustment<ExposureModel>(document, node, type_ids::Exposure())
      .SetValue(ev);
  multi_grade_test::GradeAdjustment<ContrastModel>(document, node, type_ids::Contrast())
      .SetValue(12.0f);
  multi_grade_test::GradeAdjustment<WhiteModel>(document, node, type_ids::White()).SetValue(8.0f);
  multi_grade_test::GradeAdjustment<BlackModel>(document, node, type_ids::Black()).SetValue(-6.0f);
  multi_grade_test::GradeAdjustment<ShadowsModel>(document, node, type_ids::Shadows())
      .SetValue(18.0f);
  multi_grade_test::GradeAdjustment<HighlightsModel>(document, node, type_ids::Highlights())
      .SetValue(-12.0f);
  multi_grade_test::GradeAdjustment<SaturationModel>(document, node, type_ids::Saturation())
      .SetValue(1.15f);
  multi_grade_test::GradeAdjustment<VibranceModel>(document, node, type_ids::Vibrance())
      .SetValue(8.0f);
  multi_grade_test::GradeAdjustment<CurveModel>(document, node, type_ids::Curve())
      .SetPoints({{0.0f, 0.0f}, {0.5f, 0.55f}, {1.0f, 1.0f}});
  HlsUpdate hls;
  hls.hls_adj = HlsVec3{2.0f, 0.0f, 0.05f};
  multi_grade_test::GradeAdjustment<HlsModel>(document, node, type_ids::Hls()).ApplyUpdate(hls);
  ColorWheelUpdate wheel;
  ColorWheelControlUpdate lift;
  lift.disc = Vec2f{0.08f, 0.02f};
  wheel.lift = lift;
  multi_grade_test::GradeAdjustment<ColorWheelModel>(document, node, type_ids::ColorWheel())
      .ApplyUpdate(wheel);
}

class CudaPreviewInteractiveBaselineFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
    diag::PreviewPerformance::ResetForTesting();
    diag::PreviewPerformance::SetMode(diag::PreviewPerformanceMode::Detail);
    prepared_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                              gpu_dag_test::FullSensor(16, 12));
  }

  void TearDown() override { diag::PreviewPerformance::ResetForTesting(); }

  auto CompileInteractive(PipelineDocument& document) -> ExecutionPlan {
    return GraphCompiler::Compile(document, prepared_.CompileSource(), InteractiveRequest());
  }

  auto CaptureHot(std::uint64_t request_id, PipelineDocument& document, ExecutionPlan& plan)
      -> diag::PreviewRequestRecord {
    GraphCompiler::BindFrameGeometry(plan, document, InteractiveRequest());
    std::vector<diag::PreviewRequestRecord> records;
    diag::PreviewPerformance::InstallRecordSink(
        [&](const diag::PreviewRequestRecord& record) { records.push_back(record); });
    diag::PreviewPerformance::NoteSubmit(request_id, diag::PreviewFrameRole::InteractivePrimary,
                                         diag::PreviewQuality::Interactive, "InteractiveAdjustment",
                                         true);
    diag::PreviewPerformance::BindCurrentRequest(request_id);
    diag::PreviewPerformance::NoteRenderExtent(plan.geometry.render_extent.width,
                                               plan.geometry.render_extent.height);
    {
      diag::PreviewCpuInterval encode(diag::PreviewCpuStage::Encode);
      (void)device_.Execute(plan, prepared_, document, true,
                            TransientAllocationPolicy::SessionPacked,
                            ResultPersistenceScope::AllCurrentResults);
    }
    device_.WaitIdle();
    diag::PreviewPerformance::NoteResourceSnapshot(device_.Workspace().CaptureResourceSnapshot());
    diag::PreviewPerformance::NoteDisplayed(request_id);
    diag::PreviewPerformance::ClearCurrentRequest();
    diag::PreviewPerformance::FlushWriter();
    diag::PreviewPerformance::InstallRecordSink({});
    EXPECT_EQ(records.size(), 1u);
    if (records.empty()) {
      return {};
    }
    return records.front();
  }

  void Warmup(PipelineDocument& document, ExecutionPlan& plan) {
    GraphCompiler::BindFrameGeometry(plan, document, InteractiveRequest());
    (void)device_.Execute(plan, prepared_, document, true, TransientAllocationPolicy::SessionPacked,
                          ResultPersistenceScope::AllCurrentResults);
    device_.WaitIdle();
  }

  PreparedRawInput prepared_;
  CudaRenderDevice device_;
};

TEST_F(CudaPreviewInteractiveBaselineFixture,
       InteractiveCompileClampsLongEdgeToFastPreviewMaximum) {
  prepared_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(3840, 2560),
                                            gpu_dag_test::FullSensor(3840, 2560));
  auto       document = multi_grade_test::MakeIdentityGradeDocument();
  const auto plan     = CompileInteractive(document);
  EXPECT_EQ(LongEdge(plan.geometry.render_extent), kInteractiveMaxLongEdge);
  EXPECT_TRUE(plan.encode_geometry_resample);
}

TEST_F(CudaPreviewInteractiveBaselineFixture, EightGradeLastExposureSkipsDevelopAndUpstreamGrades) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(
      document, {"g1", "g2", "g3", "g4", "g5", "g6", "g7"});
  const std::array<const char*, 8> ids = {"grade.primary", "g1", "g2", "g3", "g4", "g5", "g6", "g7"};
  for (int i = 0; i < 8; ++i) {
    PopulateHeavyGrade(document, NodeId{ids[static_cast<std::size_t>(i)]},
                       0.15f * static_cast<float>(i + 1));
  }
  document.MarkTopologyDirty();
  auto plan = CompileInteractive(document);
  ASSERT_EQ(plan.grade_nodes.size(), 8u);
  Warmup(document, plan);

  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"g7"}, type_ids::Exposure())
      .SetValue(1.25f);
  const auto record = CaptureHot(2101, document, plan);

  const auto* develop = FindPass(record, "develop", diag::PreviewPassKind::UploadRgb);
  const auto* first   = FindPass(record, "grade.primary", diag::PreviewPassKind::PrimaryColorGrade);
  const auto* last    = FindPass(record, "g7", diag::PreviewPassKind::PrimaryColorGrade);
  ASSERT_NE(last, nullptr);
  EXPECT_EQ(last->state, diag::PreviewExecutionState::Executed);
  if (develop != nullptr) {
    EXPECT_NE(develop->state, diag::PreviewExecutionState::Executed);
  }
  if (first != nullptr) {
    EXPECT_NE(first->state, diag::PreviewExecutionState::Executed);
  }
}

TEST_F(CudaPreviewInteractiveBaselineFixture, EightGradeMidCurveKeepsOwnLlfAndInvalidatesDownstream) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(
      document, {"g1", "g2", "g3", "g4", "g5", "g6", "g7"});
  const std::array<const char*, 8> ids = {"grade.primary", "g1", "g2", "g3", "g4", "g5", "g6", "g7"};
  for (int i = 0; i < 8; ++i) {
    PopulateHeavyGrade(document, NodeId{ids[static_cast<std::size_t>(i)]}, 0.2f);
  }
  document.MarkTopologyDirty();
  auto plan = CompileInteractive(document);
  Warmup(document, plan);

  multi_grade_test::GradeAdjustment<CurveModel>(document, NodeId{"g3"}, type_ids::Curve())
      .SetPoints({{0.0f, 0.0f}, {0.4f, 0.5f}, {1.0f, 1.0f}});
  const auto record = CaptureHot(2102, document, plan);

  const auto* mid  = FindPass(record, "g3", diag::PreviewPassKind::PrimaryColorGrade);
  const auto* last = FindPass(record, "g7", diag::PreviewPassKind::PrimaryColorGrade);
  ASSERT_NE(mid, nullptr);
  ASSERT_NE(last, nullptr);
  EXPECT_EQ(mid->state, diag::PreviewExecutionState::Executed);
  EXPECT_EQ(last->state, diag::PreviewExecutionState::Executed);
  const auto* first = FindPass(record, "grade.primary", diag::PreviewPassKind::PrimaryColorGrade);
  if (first != nullptr) {
    EXPECT_NE(first->state, diag::PreviewExecutionState::Executed);
  }
}

TEST_F(CudaPreviewInteractiveBaselineFixture, EightGradeOwnShadowsRebuildsLlfResultOnly) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(
      document, {"g1", "g2", "g3", "g4", "g5", "g6", "g7"});
  PopulateHeavyGrade(document, NodeId{"g4"}, 0.3f);
  document.MarkTopologyDirty();
  auto plan = CompileInteractive(document);
  Warmup(document, plan);

  multi_grade_test::GradeAdjustment<ShadowsModel>(document, NodeId{"g4"}, type_ids::Shadows())
      .SetValue(40.0f);
  const auto record = CaptureHot(2103, document, plan);
  const auto* grade = FindPass(record, "g4", diag::PreviewPassKind::PrimaryColorGrade);
  ASSERT_NE(grade, nullptr);
  EXPECT_EQ(grade->state, diag::PreviewExecutionState::Executed);
}

TEST_F(CudaPreviewInteractiveBaselineFixture, EightGradeMaskFeatherKeepsOwnLlfAndRunsMaskEvaluate) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"g1"});
  auto* look = multi_grade_test::GradeNode(document, "g1");
  ASSERT_NE(look, nullptr);
  grade_mask_test::AddMask(*look, grade_mask_test::MakeRadialMask(MaskId{"mask.g1.radial"}));
  look->SetMix(0.7f);
  PopulateHeavyGrade(document, NodeId{"g1"}, 0.4f);
  document.MarkTopologyDirty();
  auto plan = CompileInteractive(document);
  Warmup(document, plan);

  auto* mask = look->FindMask(MaskId{"mask.g1.radial"});
  ASSERT_NE(mask, nullptr);
  auto* radial = std::get_if<RadialMaskSource>(&mask->source);
  ASSERT_NE(radial, nullptr);
  RadialMaskSource updated = *radial;
  updated.center_x         = 0.42f;
  updated.outer_feather    = 0.18f;
  look->ReplaceMaskSource(MaskId{"mask.g1.radial"}, updated);
  const auto record = CaptureHot(2104, document, plan);
  const auto* evaluate = FindPass(record, "g1", diag::PreviewPassKind::MaskEvaluate);
  ASSERT_NE(evaluate, nullptr);
  EXPECT_EQ(evaluate->state, diag::PreviewExecutionState::Executed);
}

TEST_F(CudaPreviewInteractiveBaselineFixture, EightGradeDrtClaritySkipsGrades) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(
      document, {"g1", "g2", "g3", "g4", "g5", "g6", "g7"});
  PopulateHeavyGrade(document, NodeId{"grade.primary"}, 0.2f);
  document.MarkTopologyDirty();
  auto plan = CompileInteractive(document);
  Warmup(document, plan);

  auto* drt = document.Drt();
  ASSERT_NE(drt, nullptr);
  auto* clarity = dynamic_cast<ClarityModel*>(drt->FindAdjustmentByType(type_ids::Clarity()));
  ASSERT_NE(clarity, nullptr);
  clarity->SetValue(0.35f);
  const auto record = CaptureHot(2105, document, plan);
  const auto* drt_pass = FindPass(record, "drt", diag::PreviewPassKind::Drt);
  ASSERT_NE(drt_pass, nullptr);
  EXPECT_EQ(drt_pass->state, diag::PreviewExecutionState::Executed);
  const auto* grade = FindPass(record, "grade.primary", diag::PreviewPassKind::PrimaryColorGrade);
  if (grade != nullptr) {
    EXPECT_NE(grade->state, diag::PreviewExecutionState::Executed);
  }
}

#ifndef _DEBUG
TEST_F(CudaPreviewInteractiveBaselineFixture,
       Interactive2560SliderBaselinesDumpCurrentExecutionGpuTimes) {
  const auto bayer = FindFirstCiRaw();
  if (bayer.empty()) {
    GTEST_SKIP() << "No CI RAW fixture";
  }
  prepared_ = LoadEncodedFile(bayer);
  EXPECT_EQ(prepared_.downsample_passes, 0);
  EXPECT_GT(LongEdge(prepared_.develop_output_extent), kInteractiveMaxLongEdge);

  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(
      document, {"g1", "g2", "g3", "g4", "g5", "g6", "g7"});
  const std::array<const char*, 8> ids = {"grade.primary", "g1", "g2", "g3", "g4", "g5", "g6", "g7"};
  for (int i = 0; i < 8; ++i) {
    PopulateHeavyGrade(document, NodeId{ids[static_cast<std::size_t>(i)]}, 0.2f);
  }
  document.MarkTopologyDirty();
  auto plan = CompileInteractive(document);
  ASSERT_EQ(LongEdge(plan.geometry.render_extent), kInteractiveMaxLongEdge);
  ASSERT_TRUE(plan.encode_geometry_resample);

  const auto cold = CaptureHot(2201, document, plan);
  DumpRecord("bayer_2560_eight_grade_cold", cold);
  EXPECT_EQ(cold.render_width == 0 ? LongEdge(plan.geometry.render_extent) : std::max(cold.render_width, cold.render_height),
            kInteractiveMaxLongEdge);

  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"g7"}, type_ids::Exposure())
      .SetValue(0.55f);
  const auto hot = CaptureHot(2202, document, plan);
  DumpRecord("bayer_2560_eight_grade_last_exposure_hot", hot);
  const auto* develop = FindPass(hot, "develop", diag::PreviewPassKind::UploadRaw);
  ASSERT_NE(develop, nullptr);
  EXPECT_NE(develop->state, diag::PreviewExecutionState::Executed);

  multi_grade_test::GradeAdjustment<ContrastModel>(document, NodeId{"grade.primary"},
                                                   type_ids::Contrast())
      .SetValue(22.0f);
  DumpRecord("bayer_2560_eight_grade_first_contrast_hot", CaptureHot(2203, document, plan));
  multi_grade_test::GradeAdjustment<SaturationModel>(document, NodeId{"g3"}, type_ids::Saturation())
      .SetValue(1.25f);
  DumpRecord("bayer_2560_eight_grade_mid_saturation_hot", CaptureHot(2204, document, plan));

  const auto xtrans = FindXTransRaw();
  if (!xtrans.empty()) {
    device_.Workspace().ReleaseSessionResources();
    prepared_ = LoadEncodedFile(xtrans);
    auto x_document = multi_grade_test::MakeIdentityGradeDocument();
    PopulateHeavyGrade(x_document, NodeId{"grade.primary"}, 0.3f);
    auto x_plan = CompileInteractive(x_document);
    EXPECT_EQ(LongEdge(x_plan.geometry.render_extent), kInteractiveMaxLongEdge);
    const auto x_cold = CaptureHot(2301, x_document, x_plan);
    DumpRecord("xtrans_2560_three_node_cold", x_cold);
    multi_grade_test::GradeAdjustment<ExposureModel>(x_document, NodeId{"grade.primary"},
                                                     type_ids::Exposure())
        .SetValue(0.7f);
    const auto x_hot = CaptureHot(2302, x_document, x_plan);
    DumpRecord("xtrans_2560_exposure_hot", x_hot);
    const auto* x_develop = FindPass(x_hot, "develop", diag::PreviewPassKind::UploadRaw);
    if (x_develop != nullptr) {
      EXPECT_NE(x_develop->state, diag::PreviewExecutionState::Executed);
    }
  }
}
#endif

}  // namespace
}  // namespace alcedo
