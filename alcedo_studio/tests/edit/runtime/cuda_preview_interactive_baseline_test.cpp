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
#include <ostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/color_grade_node_model.hpp"
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
#include "utils/diagnostics/preview_performance_format.hpp"

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

auto MakePreviewRequest(std::uint32_t max_edge) -> RenderRequest {
  RenderRequest request;
  request.resolution.max_edge = max_edge;
  request.resolution.quality  = RenderQuality::Preview;
  return request;
}

auto InteractiveRequest() -> RenderRequest {
  return MakePreviewRequest(kInteractiveMaxLongEdge);
}

auto NativeInteractiveRequest() -> RenderRequest {
  return MakePreviewRequest(0);
}

void AddExposure(PipelineDocument& document, const NodeId& node, float ev) {
  multi_grade_test::GradeAdjustment<ExposureModel>(document, node, type_ids::Exposure())
      .SetValue(ev);
}

void AddGradeMasks(ColorGradeNodeModel& grade, std::string_view radial_id,
                   std::string_view linear_id) {
  grade_mask_test::AddMask(grade, grade_mask_test::MakeRadialMask(MaskId{std::string{radial_id}}));
  grade_mask_test::AddMask(
      grade, grade_mask_test::MakeLinearGradientMask(MaskId{std::string{linear_id}}));
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
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
      const auto ext = entry.path().extension().string();
      if (ext == ".ARW" || ext == ".arw" || ext == ".DNG" || ext == ".dng") {
        paths.push_back(entry.path());
      }
    }
    std::sort(paths.begin(), paths.end());
    if (!paths.empty()) {
      return paths.front();
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

void DumpRecord(std::ostream& out, std::string_view name, const diag::PreviewRequestRecord& record,
                std::string_view param) {
  out << name << " request=" << record.request_id
      << " gpu=" << diag::PreviewGpuTimeStatusName(record.gpu_status)
      << " gpu_ms=" << diag::FormatPreviewMilliseconds(record.gpu_ns)
      << " encode_ms=" << diag::FormatPreviewMilliseconds(record.cpu.encode_ns)
      << " render=" << record.render_width << "x" << record.render_height;
  if (record.has_develop_decode) {
    out << " develop=" << record.develop.develop_width << "x" << record.develop.develop_height
        << " cfa=" << diag::PreviewCfaKindName(record.develop.cfa)
        << " decode=" << diag::PreviewDecodeResName(record.develop.decode_res) << " demosaic="
        << diag::PreviewDemosaicMethodName(record.develop.demosaic);
  }
  if (record.has_resources) {
    out << " texture_mb=" << diag::FormatPreviewMegabytes(record.resources.texture_used_bytes)
        << " peak_mb=" << diag::FormatPreviewMegabytes(record.resources.texture_peak_used_bytes);
  }
  if (!param.empty()) {
    out << " param=" << param;
  }
  out << '\n';
  for (const auto& pass : record.passes) {
    out << "  " << pass.owner << " " << diag::PreviewPassKindName(pass.kind)
        << " state=" << diag::PreviewExecutionStateName(pass.state)
        << " cpu_ms=" << diag::FormatPreviewMilliseconds(pass.cpu_ns)
        << " gpu=" << diag::PreviewGpuTimeStatusName(pass.gpu_status)
        << " gpu_ms=" << diag::FormatPreviewMilliseconds(pass.gpu_ns);
    if (!pass.mask_id.empty()) {
      out << " mask=" << pass.mask_id;
    }
    out << '\n';
    for (const auto& sub : pass.sub_stages) {
      out << "    " << diag::PreviewSubStageKindName(sub.kind)
          << " cpu_ms=" << diag::FormatPreviewMilliseconds(sub.cpu_ns)
          << " gpu=" << diag::PreviewGpuTimeStatusName(sub.gpu_status)
          << " gpu_ms=" << diag::FormatPreviewMilliseconds(sub.gpu_ns) << '\n';
    }
  }
}

void DumpQuantiles(std::ostream& out, std::string_view name,
                   const std::vector<std::int64_t>& values) {
  out << "  " << name << " n=" << values.size()
      << " p50=" << diag::FormatPreviewMilliseconds(diag::PreviewQuantileNs(values, 0.50))
      << " p95=" << diag::FormatPreviewMilliseconds(diag::PreviewQuantileNs(values, 0.95))
      << " p99=" << diag::FormatPreviewMilliseconds(diag::PreviewQuantileNs(values, 0.99))
      << " max=" << diag::FormatPreviewMilliseconds(diag::PreviewMaxNs(values)) << '\n';
}

void DumpHotP50(std::ostream& out, std::string_view name,
                const std::vector<diag::PreviewRequestRecord>& records, std::string_view param) {
  if (records.empty()) {
    return;
  }
  std::vector<std::int64_t> gpu_ns;
  std::vector<std::int64_t> encode_ns;
  gpu_ns.reserve(records.size());
  encode_ns.reserve(records.size());
  for (const auto& record : records) {
    gpu_ns.push_back(record.gpu_ns);
    encode_ns.push_back(record.cpu.encode_ns);
  }
  auto ordered = records;
  std::sort(ordered.begin(), ordered.end(),
            [](const auto& a, const auto& b) { return a.gpu_ns < b.gpu_ns; });
  const auto& median = ordered[ordered.size() / 2];
  const auto& slowest =
      *std::max_element(records.begin(), records.end(),
                        [](const auto& a, const auto& b) { return a.gpu_ns < b.gpu_ns; });
  out << name << " hot_slider n=" << records.size() << " param=" << param
      << " render=" << median.render_width << "x" << median.render_height << '\n';
  DumpQuantiles(out, "gpu_ms", gpu_ns);
  DumpQuantiles(out, "encode_ms", encode_ns);
  out << "  p50_gpu_pass_trace:\n";
  DumpRecord(out, "    median", median, param);
  out << "  slowest_gpu_pass_trace:\n";
  DumpRecord(out, "    slowest", slowest, param);
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
    return GraphCompiler::Compile(document, prepared_.CompileSource(), request_);
  }

  auto CaptureHot(std::uint64_t request_id, PipelineDocument& document, ExecutionPlan& plan)
      -> diag::PreviewRequestRecord {
    GraphCompiler::BindFrameGeometry(plan, document, request_);
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

  auto CaptureSliderHot(std::uint64_t first_id, PipelineDocument& document, ExecutionPlan& plan,
                        int repeats, const NodeId& node, float first_ev, float step)
      -> std::vector<diag::PreviewRequestRecord> {
    std::vector<diag::PreviewRequestRecord> records;
    records.reserve(static_cast<std::size_t>(repeats));
    for (int i = 0; i < repeats; ++i) {
      AddExposure(document, node, first_ev + step * static_cast<float>(i));
      records.push_back(CaptureHot(first_id + static_cast<std::uint64_t>(i), document, plan));
    }
    return records;
  }

  void Warmup(PipelineDocument& document, ExecutionPlan& plan) {
    GraphCompiler::BindFrameGeometry(plan, document, request_);
    (void)device_.Execute(plan, prepared_, document, true, TransientAllocationPolicy::SessionPacked,
                          ResultPersistenceScope::AllCurrentResults);
    device_.WaitIdle();
  }

  PreparedRawInput prepared_;
  CudaRenderDevice device_;
  RenderRequest    request_ = InteractiveRequest();
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

TEST_F(CudaPreviewInteractiveBaselineFixture, EightGradeLastExposureReexecutesGradesAndSkipsDevelop) {
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
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->state, diag::PreviewExecutionState::Executed);
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
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->state, diag::PreviewExecutionState::Executed);
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

TEST_F(CudaPreviewInteractiveBaselineFixture, EightGradeDrtClarityReexecutesGrades) {
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
  ASSERT_NE(grade, nullptr);
  EXPECT_EQ(grade->state, diag::PreviewExecutionState::Executed);
}

#ifndef _DEBUG
constexpr int kSliderHotRepeats = 11;

void WriteSliderDumpHeader(std::ostream& out, std::string_view title, const std::filesystem::path& source,
                           const PreparedRawInput& prepared, std::uint32_t max_edge) {
  out << title << '\n';
  out << "kind=slider_interactive_gpu_trace session_cache=kept decode_res=FULL\n";
  out << "source=" << source.filename().string()
      << " develop=" << prepared.develop_output_extent.width << "x"
      << prepared.develop_output_extent.height
      << " downsample_passes=" << static_cast<unsigned>(prepared.downsample_passes)
      << " max_edge=" << max_edge << '\n';
  out << "hot_repeats=" << kSliderHotRepeats
      << " note=each_hot_frame_mutates_the_named_operator_on_the_live_document\n";
}

TEST_F(CudaPreviewInteractiveBaselineFixture,
       Interactive2560SliderBaselinesDumpCurrentExecutionGpuTimes) {
  const auto bayer = FindFirstCiRaw();
  if (bayer.empty()) {
    GTEST_SKIP() << "No CI RAW fixture";
  }
  prepared_ = LoadEncodedFile(bayer);
  request_  = InteractiveRequest();
  EXPECT_EQ(prepared_.downsample_passes, 0);
  EXPECT_GT(LongEdge(prepared_.develop_output_extent), kInteractiveMaxLongEdge);

  const auto dump_path = PreviewDumpDirectory() / "cuda_interactive_2560_pass_table.txt";
  std::ofstream out(dump_path, std::ios::trunc);
  WriteSliderDumpHeader(out, "CUDA Interactive 2560 slider GPU traces", bayer, prepared_,
                        kInteractiveMaxLongEdge);

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
  DumpRecord(out, "bayer_2560_eight_grade_cold", cold,
             "initial PopulateHeavyGrade ev=0.2 on all 8 Color Grades");
  EXPECT_EQ(cold.render_width == 0 ? LongEdge(plan.geometry.render_extent)
                                   : std::max(cold.render_width, cold.render_height),
            kInteractiveMaxLongEdge);

  const auto last_exposure = CaptureSliderHot(2210, document, plan, kSliderHotRepeats, NodeId{"g7"},
                                              0.55f, 0.03f);
  DumpHotP50(out, "bayer_2560_eight_grade_last_exposure_hot", last_exposure,
             "g7.exposure 0.55 + 0.03*i");
  ASSERT_FALSE(last_exposure.empty());
  const auto* last_develop =
      FindPass(last_exposure.back(), "develop", diag::PreviewPassKind::UploadRaw);
  ASSERT_NE(last_develop, nullptr);
  EXPECT_NE(last_develop->state, diag::PreviewExecutionState::Executed);

  std::vector<diag::PreviewRequestRecord> first_contrast;
  first_contrast.reserve(static_cast<std::size_t>(kSliderHotRepeats));
  for (int i = 0; i < kSliderHotRepeats; ++i) {
    multi_grade_test::GradeAdjustment<ContrastModel>(document, NodeId{"grade.primary"},
                                                     type_ids::Contrast())
        .SetValue(12.0f + 1.0f * static_cast<float>(i));
    first_contrast.push_back(
        CaptureHot(2400 + static_cast<std::uint64_t>(i), document, plan));
  }
  DumpHotP50(out, "bayer_2560_eight_grade_first_contrast_hot", first_contrast,
             "grade.primary.contrast 12 + 1*i");

  std::vector<diag::PreviewRequestRecord> mid_sat;
  mid_sat.reserve(static_cast<std::size_t>(kSliderHotRepeats));
  for (int i = 0; i < kSliderHotRepeats; ++i) {
    multi_grade_test::GradeAdjustment<SaturationModel>(document, NodeId{"g3"}, type_ids::Saturation())
        .SetValue(1.15f + 0.02f * static_cast<float>(i));
    mid_sat.push_back(CaptureHot(2500 + static_cast<std::uint64_t>(i), document, plan));
  }
  DumpHotP50(out, "bayer_2560_eight_grade_mid_saturation_hot", mid_sat, "g3.saturation 1.15 + 0.02*i");

  const auto xtrans = FindXTransRaw();
  if (!xtrans.empty()) {
    device_.Workspace().ReleaseSessionResources();
    prepared_ = LoadEncodedFile(xtrans);
    auto x_document = multi_grade_test::MakeIdentityGradeDocument();
    PopulateHeavyGrade(x_document, NodeId{"grade.primary"}, 0.3f);
    auto x_plan = CompileInteractive(x_document);
    EXPECT_EQ(LongEdge(x_plan.geometry.render_extent), kInteractiveMaxLongEdge);
    const auto x_cold = CaptureHot(2301, x_document, x_plan);
    DumpRecord(out, "xtrans_2560_three_node_cold", x_cold, "grade.primary PopulateHeavyGrade ev=0.3");
    DumpHotP50(out, "xtrans_2560_exposure_hot",
               CaptureSliderHot(2310, x_document, x_plan, kSliderHotRepeats,
                                NodeId{"grade.primary"}, 0.70f, 0.03f),
               "grade.primary.exposure 0.70 + 0.03*i");
  }
  out.flush();
}

TEST_F(CudaPreviewInteractiveBaselineFixture,
       InteractiveNativeSliderBaselinesDumpCurrentExecutionGpuTimes) {
  const auto bayer = FindFirstCiRaw();
  if (bayer.empty()) {
    GTEST_SKIP() << "No CI RAW fixture";
  }
  prepared_ = LoadEncodedFile(bayer);
  request_  = NativeInteractiveRequest();
  EXPECT_EQ(prepared_.downsample_passes, 0);
  const auto native_long_edge = LongEdge(prepared_.develop_output_extent);
  ASSERT_GT(native_long_edge, kInteractiveMaxLongEdge);

  const auto dump_path = PreviewDumpDirectory() / "cuda_interactive_native_slider_table.txt";
  std::ofstream out(dump_path, std::ios::trunc);
  WriteSliderDumpHeader(out, "CUDA Interactive native-sensor slider GPU traces", bayer, prepared_,
                        0);

  auto expect_native = [&](const ExecutionPlan& plan) {
    EXPECT_EQ(LongEdge(plan.geometry.render_extent), native_long_edge) << "native Interactive";
  };

  {
    auto document = multi_grade_test::MakeIdentityGradeDocument();
    AddExposure(document, NodeId{"grade.primary"}, 0.75f);
    auto plan = CompileInteractive(document);
    expect_native(plan);
    DumpRecord(out, "native_three_node_cold", CaptureHot(3101, document, plan),
               "grade.primary.exposure=0.75");
    DumpHotP50(out, "native_three_node_exposure_hot",
               CaptureSliderHot(3110, document, plan, kSliderHotRepeats, NodeId{"grade.primary"},
                                0.78f, 0.03f),
               "grade.primary.exposure 0.78 + 0.03*i");
  }

  {
    auto document = multi_grade_test::MakeIdentityGradeDocument();
    ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.look"}).empty());
    AddExposure(document, NodeId{"grade.primary"}, 0.5f);
    AddExposure(document, NodeId{"grade.look"}, 0.25f);
    auto* look = multi_grade_test::GradeNode(document, "grade.look");
    ASSERT_NE(look, nullptr);
    look->SetMix(0.8f);
    AddGradeMasks(*look, "mask.look.radial", "mask.look.linear");
    document.MarkTopologyDirty();
    auto plan = CompileInteractive(document);
    expect_native(plan);
    DumpRecord(out, "native_four_node_second_masked_cold", CaptureHot(3201, document, plan),
               "grade.look mix=0.8 radial+linear grade.look.exposure=0.25");
    DumpHotP50(out, "native_four_node_look_exposure_hot",
               CaptureSliderHot(3210, document, plan, kSliderHotRepeats, NodeId{"grade.look"}, 0.28f,
                                0.03f),
               "grade.look.exposure 0.28 + 0.03*i");
  }

  {
    auto document = multi_grade_test::MakeIdentityGradeDocument();
    multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b", "grade.c"});
    auto* g0 = document.PrimaryGrade();
    auto* g1 = multi_grade_test::GradeNode(document, "grade.b");
    auto* g2 = multi_grade_test::GradeNode(document, "grade.c");
    ASSERT_NE(g0, nullptr);
    ASSERT_NE(g1, nullptr);
    ASSERT_NE(g2, nullptr);
    g0->SetMix(0.75f);
    g1->SetMix(0.50f);
    g2->SetMix(0.25f);
    AddExposure(document, NodeId{"grade.primary"}, 0.4f);
    AddExposure(document, NodeId{"grade.b"}, 0.3f);
    AddExposure(document, NodeId{"grade.c"}, 0.2f);
    AddGradeMasks(*g0, "mask.0.r", "mask.0.l");
    AddGradeMasks(*g1, "mask.1.r", "mask.1.l");
    AddGradeMasks(*g2, "mask.2.r", "mask.2.l");
    document.MarkTopologyDirty();
    auto plan = CompileInteractive(document);
    expect_native(plan);
    DumpRecord(out, "native_multi_grade_mask_mix_cold", CaptureHot(3301, document, plan),
               "three grades each Radial+Linear mix=0.75/0.50/0.25");
    DumpHotP50(out, "native_multi_grade_last_exposure_hot",
               CaptureSliderHot(3310, document, plan, kSliderHotRepeats, NodeId{"grade.c"}, 0.23f,
                                0.03f),
               "grade.c.exposure 0.23 + 0.03*i");
  }

  {
    auto document = multi_grade_test::MakeIdentityGradeDocument();
    ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.look"}).empty());
    const auto red  = PreviewDumpDirectory() / "lut_primary.cube";
    const auto blue = PreviewDumpDirectory() / "lut_second.cube";
    multi_grade_test::WriteConstantRgbCube(red, 1.0f, 0.0f, 0.0f);
    multi_grade_test::WriteConstantRgbCube(blue, 0.0f, 0.0f, 1.0f);
    multi_grade_test::GradeAdjustment<LmtModel>(document, NodeId{"grade.primary"}, type_ids::Lmt())
        .SetCubePath(red.string());
    multi_grade_test::GradeAdjustment<LmtModel>(document, NodeId{"grade.look"}, type_ids::Lmt())
        .SetCubePath(blue.string());
    auto plan = CompileInteractive(document);
    expect_native(plan);
    DumpRecord(out, "native_two_lut_grades_cold", CaptureHot(3401, document, plan),
               "grade.primary LMT red cube; grade.look LMT blue cube");
    DumpHotP50(out, "native_two_lut_look_exposure_hot",
               CaptureSliderHot(3410, document, plan, kSliderHotRepeats, NodeId{"grade.look"}, 0.20f,
                                0.03f),
               "grade.look.exposure 0.20 + 0.03*i");
  }

  {
    auto document = multi_grade_test::MakeIdentityGradeDocument();
    multi_grade_test::AddCleanGradesBeforeDrt(
        document, {"g1", "g2", "g3", "g4", "g5", "g6", "g7"});
    const std::array<const char*, 8> ids = {"grade.primary", "g1", "g2", "g3",
                                            "g4",            "g5", "g6", "g7"};
    for (int i = 0; i < 8; ++i) {
      PopulateHeavyGrade(document, NodeId{ids[static_cast<std::size_t>(i)]}, 0.2f);
    }
    document.MarkTopologyDirty();
    auto plan = CompileInteractive(document);
    expect_native(plan);
    DumpRecord(out, "native_eight_grade_cold", CaptureHot(3501, document, plan),
               "8 Color Grades PopulateHeavyGrade ev=0.2");
    const auto last_native = CaptureSliderHot(3510, document, plan, kSliderHotRepeats, NodeId{"g7"},
                                              0.55f, 0.03f);
    DumpHotP50(out, "native_eight_grade_last_exposure_hot", last_native,
               "g7.exposure 0.55 + 0.03*i");
    ASSERT_FALSE(last_native.empty());
    const auto* develop = FindPass(last_native.back(), "develop", diag::PreviewPassKind::UploadRaw);
    ASSERT_NE(develop, nullptr);
    EXPECT_NE(develop->state, diag::PreviewExecutionState::Executed);
  }
  out.flush();
}
#endif

}  // namespace
}  // namespace alcedo
