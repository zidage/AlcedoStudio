//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/result_persistence.hpp"
#include "multi_grade_runtime_test_support.hpp"
#include "utils/diagnostics/preview_performance.hpp"

namespace alcedo {
namespace {

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

constexpr float kPixelAbsTolerance = 1.0e-5f;

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto Download(CudaRenderDevice& device, const GraphValueId& id) -> std::vector<Rgba> {
  auto* lease = device.Workspace().Images().Find(id);
  EXPECT_NE(lease, nullptr);
  if (lease == nullptr) {
    return {};
  }
  const auto&       texture = lease->Texture();
  std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
  device.Workspace().Device().DownloadTexture2D(
      texture,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
}

auto MaxAbsRgbaError(const std::vector<Rgba>& left, const std::vector<Rgba>& right) -> float {
  if (left.size() != right.size() || left.empty()) {
    return std::numeric_limits<float>::infinity();
  }
  float max_abs = 0.0f;
  for (std::size_t i = 0; i < left.size(); ++i) {
    max_abs = std::max(max_abs, std::abs(left[i].r - right[i].r));
    max_abs = std::max(max_abs, std::abs(left[i].g - right[i].g));
    max_abs = std::max(max_abs, std::abs(left[i].b - right[i].b));
  }
  return max_abs;
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

auto CountKind(const diag::PreviewRequestRecord& record, diag::PreviewPassKind kind)
    -> std::size_t {
  std::size_t count = 0;
  for (const auto& pass : record.passes) {
    if (pass.kind == kind) {
      ++count;
    }
  }
  return count;
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

void DumpRecord(std::string_view name, const diag::PreviewRequestRecord& record) {
  const auto path = PreviewDumpDirectory() / "cuda_dag_baseline.txt";
  std::ofstream out(path, std::ios::app);
  out << name << " request=" << record.request_id
      << " gpu=" << diag::PreviewGpuTimeStatusName(record.gpu_status)
      << " gpu_ms=" << (static_cast<double>(record.gpu_ns) / 1.0e6)
      << " encode_ms=" << (static_cast<double>(record.cpu.encode_ns) / 1.0e6)
      << " lost=" << diag::PreviewPerformance::EventsLost();
  if (record.has_resources) {
    out << " texture_mb="
        << (static_cast<double>(record.resources.texture_used_bytes) / (1024.0 * 1024.0))
        << " peak_mb="
        << (static_cast<double>(record.resources.texture_peak_used_bytes) / (1024.0 * 1024.0));
  }
  out << '\n';
  for (const auto& pass : record.passes) {
    out << "  " << pass.owner << " " << diag::PreviewPassKindName(pass.kind)
        << " state=" << diag::PreviewExecutionStateName(pass.state)
        << " cpu_ms=" << (static_cast<double>(pass.cpu_ns) / 1.0e6)
        << " gpu=" << diag::PreviewGpuTimeStatusName(pass.gpu_status)
        << " gpu_ms=" << (static_cast<double>(pass.gpu_ns) / 1.0e6);
    if (!pass.mask_id.empty()) {
      out << " mask=" << pass.mask_id;
    }
    out << '\n';
    for (const auto& sub : pass.sub_stages) {
      out << "    " << diag::PreviewSubStageKindName(sub.kind)
          << " cpu_ms=" << (static_cast<double>(sub.cpu_ns) / 1.0e6)
          << " gpu=" << diag::PreviewGpuTimeStatusName(sub.gpu_status)
          << " gpu_ms=" << (static_cast<double>(sub.gpu_ns) / 1.0e6) << '\n';
    }
  }
}

class CudaPreviewGpuTimingFixture : public ::testing::Test {
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

  auto Compile(PipelineDocument& document) -> ExecutionPlan {
    return GraphCompiler::Compile(document, prepared_.CompileSource(), RenderRequest{});
  }

  auto Capture(std::uint64_t request_id, PipelineDocument& document, const ExecutionPlan& plan,
               bool wait_after_execute = true) -> diag::PreviewRequestRecord {
    std::vector<diag::PreviewRequestRecord> records;
    diag::PreviewPerformance::InstallRecordSink(
        [&](const diag::PreviewRequestRecord& record) { records.push_back(record); });
    diag::PreviewPerformance::NoteSubmit(request_id, diag::PreviewFrameRole::InteractivePrimary,
                                         diag::PreviewQuality::Interactive, "InteractiveAdjustment",
                                         false);
    diag::PreviewPerformance::BindCurrentRequest(request_id);
    device_.ResetPassStats();
    {
      diag::PreviewCpuInterval encode(diag::PreviewCpuStage::Encode);
      (void)device_.Execute(plan, prepared_, document, true,
                            TransientAllocationPolicy::SessionPacked,
                            ResultPersistenceScope::AllCurrentResults);
    }
    if (wait_after_execute) {
      device_.WaitIdle();
    }
    diag::PreviewPerformance::NoteResourceSnapshot(
        device_.Workspace().CaptureResourceSnapshot());
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

  PreparedRawInput prepared_;
  CudaRenderDevice device_;
};

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

}  // namespace

TEST_F(CudaPreviewGpuTimingFixture, GpuPassSamplesKeepRequestAndNodeIdentity) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  AddExposure(document, NodeId{"grade.primary"}, 1.0f);
  auto* primary = document.PrimaryGrade();
  ASSERT_NE(primary, nullptr);
  AddGradeMasks(*primary, "mask.radial", "mask.linear");
  document.MarkTopologyDirty();
  const auto plan   = Compile(document);
  const auto record = Capture(1001, document, plan);
  EXPECT_EQ(record.request_id, 1001u);
  EXPECT_EQ(record.frame_role, diag::PreviewFrameRole::InteractivePrimary);
  const auto* mask = FindPass(record, "grade.primary", diag::PreviewPassKind::MaskEvaluate);
  ASSERT_NE(mask, nullptr);
  EXPECT_FALSE(mask->mask_id.empty());
  EXPECT_EQ(mask->state, diag::PreviewExecutionState::Executed);
  EXPECT_EQ(mask->gpu_status, diag::PreviewGpuTimeStatus::Available);
  const auto* grade = FindPass(record, "grade.primary", diag::PreviewPassKind::PrimaryColorGrade);
  ASSERT_NE(grade, nullptr);
  EXPECT_EQ(grade->state, diag::PreviewExecutionState::Executed);
  EXPECT_EQ(grade->gpu_status, diag::PreviewGpuTimeStatus::Available);
  bool has_pointwise = false;
  for (const auto& sub : grade->sub_stages) {
    if (sub.kind == diag::PreviewSubStageKind::Pointwise) {
      has_pointwise = true;
      EXPECT_EQ(sub.gpu_status, diag::PreviewGpuTimeStatus::Available);
    }
  }
  EXPECT_TRUE(has_pointwise);
}

TEST_F(CudaPreviewGpuTimingFixture, TimingSlotsAreNotReusedBeforeSubmissionCompletes) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  AddExposure(document, NodeId{"grade.primary"}, 1.0f);
  const auto plan = Compile(document);
  diag::PreviewPerformance::NoteSubmit(1002, diag::PreviewFrameRole::InteractivePrimary,
                                       diag::PreviewQuality::Interactive, "InteractiveAdjustment",
                                       false);
  diag::PreviewPerformance::BindCurrentRequest(1002);
  (void)device_.Execute(plan, prepared_, document);
  EXPECT_GT(device_.Workspace().Device().GpuTimestampInFlightCount(), 0u);
  const auto in_flight = device_.Workspace().Device().GpuTimestampInFlightCount();
  const auto slots     = device_.Workspace().Device().GpuTimestampSlotCount();
  EXPECT_LE(in_flight, slots);
  device_.WaitIdle();
  EXPECT_EQ(device_.Workspace().Device().GpuTimestampInFlightCount(), 0u);
  diag::PreviewPerformance::NoteDisplayed(1002);
  diag::PreviewPerformance::ClearCurrentRequest();
}

TEST_F(CudaPreviewGpuTimingFixture, GpuTimingDoesNotAddPerPassHostWaits) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  AddExposure(document, NodeId{"grade.primary"}, 1.0f);
  auto* primary = document.PrimaryGrade();
  ASSERT_NE(primary, nullptr);
  AddGradeMasks(*primary, "mask.radial", "mask.linear");
  document.MarkTopologyDirty();
  const auto plan = Compile(document);

  diag::PreviewPerformance::SetMode(diag::PreviewPerformanceMode::Off);
  CudaRenderDevice off_device;
  off_device.Workspace().Device().ResetCounters();
  (void)off_device.Execute(plan, prepared_, document, true,
                           TransientAllocationPolicy::SessionPacked,
                           ResultPersistenceScope::AllCurrentResults);
  const auto waits_off = off_device.Workspace().Device().HostWaitCount();
  off_device.WaitIdle();

  diag::PreviewPerformance::SetMode(diag::PreviewPerformanceMode::Detail);
  CudaRenderDevice detail_device;
  detail_device.Workspace().Device().ResetCounters();
  diag::PreviewPerformance::NoteSubmit(1003, diag::PreviewFrameRole::InteractivePrimary,
                                       diag::PreviewQuality::Interactive, "InteractiveAdjustment",
                                       false);
  diag::PreviewPerformance::BindCurrentRequest(1003);
  (void)detail_device.Execute(plan, prepared_, document, true,
                              TransientAllocationPolicy::SessionPacked,
                              ResultPersistenceScope::AllCurrentResults);
  const auto waits_detail = detail_device.Workspace().Device().HostWaitCount();
  const auto executed_passes =
      detail_device.PassStats().sensor_develop_execute +
      detail_device.PassStats().geometry_execute + detail_device.PassStats().camera_color_execute +
      detail_device.PassStats().mask_execute + detail_device.PassStats().mask_union_execute +
      detail_device.PassStats().primary_grade_execute + detail_device.PassStats().drt_execute;
  const auto mask_grade_drt = detail_device.PassStats().mask_execute +
                              detail_device.PassStats().mask_union_execute +
                              detail_device.PassStats().primary_grade_execute +
                              detail_device.PassStats().drt_execute;
  EXPECT_GE(executed_passes, 4u);
  EXPECT_GE(mask_grade_drt, 4u);
  EXPECT_EQ(waits_detail, waits_off);
  EXPECT_LT(waits_detail, executed_passes);
  EXPECT_LT(waits_detail, mask_grade_drt);
  detail_device.WaitIdle();
  diag::PreviewPerformance::NoteDisplayed(1003);
  diag::PreviewPerformance::ClearCurrentRequest();
}

TEST_F(CudaPreviewGpuTimingFixture, CachedAndDisabledPassesReportExecutionState) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.look"}).empty());
  AddExposure(document, NodeId{"grade.primary"}, 1.0f);
  auto* look = multi_grade_test::GradeNode(document, "grade.look");
  ASSERT_NE(look, nullptr);
  look->SetMix(0.0f);
  auto plan        = Compile(document);
  const auto first = Capture(1004, document, plan);
  const auto* first_primary =
      FindPass(first, "grade.primary", diag::PreviewPassKind::PrimaryColorGrade);
  const auto* first_look = FindPass(first, "grade.look", diag::PreviewPassKind::PrimaryColorGrade);
  ASSERT_NE(first_primary, nullptr);
  ASSERT_NE(first_look, nullptr);
  EXPECT_EQ(first_primary->state, diag::PreviewExecutionState::Executed);
  EXPECT_EQ(first_primary->gpu_status, diag::PreviewGpuTimeStatus::Available);
  EXPECT_EQ(first_look->state, diag::PreviewExecutionState::Aliased);
  EXPECT_EQ(first_look->gpu_status, diag::PreviewGpuTimeStatus::Unavailable);
  EXPECT_EQ(first_look->gpu_ns, 0);

  const auto second = Capture(1005, document, plan);
  const auto* second_primary =
      FindPass(second, "grade.primary", diag::PreviewPassKind::PrimaryColorGrade);
  ASSERT_NE(second_primary, nullptr);
  EXPECT_EQ(second_primary->state, diag::PreviewExecutionState::Skipped);
  EXPECT_EQ(second_primary->gpu_status, diag::PreviewGpuTimeStatus::Unavailable);
  EXPECT_EQ(second_primary->gpu_ns, 0);
}

TEST_F(CudaPreviewGpuTimingFixture, DetailTimingPreservesRenderedPixelsWithinTolerance) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  AddExposure(document, NodeId{"grade.primary"}, 1.25f);
  const auto plan = Compile(document);

  diag::PreviewPerformance::SetMode(diag::PreviewPerformanceMode::Off);
  CudaRenderDevice off_device;
  (void)off_device.Execute(plan, prepared_, document);
  off_device.WaitIdle();
  const auto off_pixels = Download(off_device, plan.display_output);

  diag::PreviewPerformance::SetMode(diag::PreviewPerformanceMode::Detail);
  CudaRenderDevice detail_device;
  diag::PreviewPerformance::NoteSubmit(1006, diag::PreviewFrameRole::InteractivePrimary,
                                       diag::PreviewQuality::Interactive, "InteractiveAdjustment",
                                       false);
  diag::PreviewPerformance::BindCurrentRequest(1006);
  (void)detail_device.Execute(plan, prepared_, document);
  detail_device.WaitIdle();
  const auto detail_pixels = Download(detail_device, plan.display_output);
  diag::PreviewPerformance::NoteDisplayed(1006);
  diag::PreviewPerformance::ClearCurrentRequest();

  ASSERT_EQ(off_pixels.size(), detail_pixels.size());
  EXPECT_LE(MaxAbsRgbaError(off_pixels, detail_pixels), kPixelAbsTolerance);
}

TEST_F(CudaPreviewGpuTimingFixture, InteractiveThreeNodeGraphReportsPassGpuTimes) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  AddExposure(document, NodeId{"grade.primary"}, 0.75f);
  const auto plan   = Compile(document);
  const auto record = Capture(1101, document, plan);
  DumpRecord("three_node", record);
  EXPECT_EQ(CountKind(record, diag::PreviewPassKind::MaskEvaluate), 0u);
  const auto* grade = FindPass(record, "grade.primary", diag::PreviewPassKind::PrimaryColorGrade);
  const auto* drt   = FindPass(record, "drt", diag::PreviewPassKind::Drt);
  ASSERT_NE(grade, nullptr);
  ASSERT_NE(drt, nullptr);
  EXPECT_EQ(grade->state, diag::PreviewExecutionState::Executed);
  EXPECT_EQ(grade->gpu_status, diag::PreviewGpuTimeStatus::Available);
  EXPECT_EQ(drt->gpu_status, diag::PreviewGpuTimeStatus::Available);
  bool has_pointwise = false;
  for (const auto& sub : grade->sub_stages) {
    has_pointwise = has_pointwise || sub.kind == diag::PreviewSubStageKind::Pointwise;
  }
  EXPECT_TRUE(has_pointwise);
}

TEST_F(CudaPreviewGpuTimingFixture, InteractiveFourNodeSecondGradeMasksReportGpuTimes) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.look"}).empty());
  AddExposure(document, NodeId{"grade.primary"}, 0.5f);
  AddExposure(document, NodeId{"grade.look"}, 0.25f);
  auto* look = multi_grade_test::GradeNode(document, "grade.look");
  ASSERT_NE(look, nullptr);
  look->SetMix(0.8f);
  AddGradeMasks(*look, "mask.look.radial", "mask.look.linear");
  document.MarkTopologyDirty();
  const auto plan   = Compile(document);
  const auto record = Capture(1102, document, plan);
  DumpRecord("four_node_second_masked", record);
  EXPECT_EQ(FindPass(record, "grade.primary", diag::PreviewPassKind::MaskEvaluate), nullptr);
  const auto* look_mask = FindPass(record, "grade.look", diag::PreviewPassKind::MaskEvaluate);
  const auto* look_union = FindPass(record, "grade.look", diag::PreviewPassKind::MaskUnion);
  const auto* look_grade =
      FindPass(record, "grade.look", diag::PreviewPassKind::PrimaryColorGrade);
  ASSERT_NE(look_mask, nullptr);
  ASSERT_NE(look_union, nullptr);
  ASSERT_NE(look_grade, nullptr);
  EXPECT_EQ(look_mask->gpu_status, diag::PreviewGpuTimeStatus::Available);
  EXPECT_EQ(look_union->gpu_status, diag::PreviewGpuTimeStatus::Available);
  EXPECT_EQ(look_grade->gpu_status, diag::PreviewGpuTimeStatus::Available);
  bool has_mix = false;
  for (const auto& sub : look_grade->sub_stages) {
    if (sub.kind == diag::PreviewSubStageKind::Mix) {
      has_mix = true;
      EXPECT_EQ(sub.gpu_status, diag::PreviewGpuTimeStatus::Available);
    }
  }
  EXPECT_TRUE(has_mix);
}

TEST_F(CudaPreviewGpuTimingFixture, InteractiveMultiGradeMaskMixReportsPerNodeGpuTimes) {
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
  const auto plan   = Compile(document);
  const auto record = Capture(1103, document, plan);
  DumpRecord("multi_grade_mask_mix", record);
  for (const char* id : {"grade.primary", "grade.b", "grade.c"}) {
    const auto* mask  = FindPass(record, id, diag::PreviewPassKind::MaskEvaluate);
    const auto* grade = FindPass(record, id, diag::PreviewPassKind::PrimaryColorGrade);
    ASSERT_NE(mask, nullptr) << id;
    ASSERT_NE(grade, nullptr) << id;
    EXPECT_EQ(mask->owner, id);
    EXPECT_FALSE(mask->mask_id.empty());
    EXPECT_EQ(mask->gpu_status, diag::PreviewGpuTimeStatus::Available);
    EXPECT_EQ(grade->gpu_status, diag::PreviewGpuTimeStatus::Available);
    bool has_mix = false;
    for (const auto& sub : grade->sub_stages) {
      has_mix = has_mix || sub.kind == diag::PreviewSubStageKind::Mix;
    }
    EXPECT_TRUE(has_mix) << id;
  }
}

TEST_F(CudaPreviewGpuTimingFixture, TwoLutGradesReportIndependentPassGpuTimes) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.look"}).empty());
    const auto red = PreviewDumpDirectory() / "lut_primary.cube";
    const auto blue = PreviewDumpDirectory() / "lut_second.cube";
  multi_grade_test::WriteConstantRgbCube(red, 1.0f, 0.0f, 0.0f);
  multi_grade_test::WriteConstantRgbCube(blue, 0.0f, 0.0f, 1.0f);
  multi_grade_test::GradeAdjustment<LmtModel>(document, NodeId{"grade.primary"}, type_ids::Lmt())
      .SetCubePath(red.string());
  multi_grade_test::GradeAdjustment<LmtModel>(document, NodeId{"grade.look"}, type_ids::Lmt())
      .SetCubePath(blue.string());
  const auto plan   = Compile(document);
  const auto record = Capture(1104, document, plan);
  DumpRecord("two_lut_grades", record);
  const auto* primary =
      FindPass(record, "grade.primary", diag::PreviewPassKind::PrimaryColorGrade);
  const auto* look = FindPass(record, "grade.look", diag::PreviewPassKind::PrimaryColorGrade);
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(look, nullptr);
  EXPECT_EQ(primary->state, diag::PreviewExecutionState::Executed);
  EXPECT_EQ(look->state, diag::PreviewExecutionState::Executed);
  EXPECT_EQ(primary->gpu_status, diag::PreviewGpuTimeStatus::Available);
  EXPECT_EQ(look->gpu_status, diag::PreviewGpuTimeStatus::Available);
  const auto a = Download(device_, plan.grade_nodes[0].scene_output);
  const auto b = Download(device_, plan.grade_nodes[1].scene_output);
  ASSERT_FALSE(a.empty());
  ASSERT_FALSE(b.empty());
  EXPECT_NEAR(a.front().r, 1.0f, 1.0e-4f);
  EXPECT_NEAR(b.front().b, 1.0f, 1.0e-4f);
}

auto PercentileNs(std::vector<std::int64_t> samples, const double fraction) -> std::int64_t {
  if (samples.empty()) {
    return 0;
  }
  std::sort(samples.begin(), samples.end());
  const auto index = static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1));
  return samples[index];
}

void DumpP50(std::string_view name, const std::vector<diag::PreviewRequestRecord>& records,
             std::uint32_t width, std::uint32_t height) {
  const auto path = PreviewDumpDirectory() / "cuda_dag_baseline_p50.txt";
  std::ofstream out(path, std::ios::app);
  std::vector<std::int64_t> encode;
  std::vector<std::int64_t> gpu;
  encode.reserve(records.size());
  gpu.reserve(records.size());
  for (const auto& record : records) {
    encode.push_back(record.cpu.encode_ns);
    gpu.push_back(record.gpu_ns);
  }
  out << name << " n=" << records.size() << " plane=" << width << "x" << height
      << " encode_p50_ms=" << (static_cast<double>(PercentileNs(encode, 0.50)) / 1.0e6)
      << " gpu_p50_ms=" << (static_cast<double>(PercentileNs(gpu, 0.50)) / 1.0e6) << '\n';
  if (!records.empty()) {
    for (const auto& pass : records.back().passes) {
      std::vector<std::int64_t> pass_gpu;
      pass_gpu.reserve(records.size());
      for (const auto& record : records) {
        const auto* found = FindPass(record, pass.owner, pass.kind);
        pass_gpu.push_back(found == nullptr ? 0 : found->gpu_ns);
      }
      out << "  " << pass.owner << " " << diag::PreviewPassKindName(pass.kind)
          << " gpu_p50_ms=" << (static_cast<double>(PercentileNs(pass_gpu, 0.50)) / 1.0e6) << '\n';
    }
  }
}

TEST_F(CudaPreviewGpuTimingFixture, InteractiveDagBaselinesDumpCurrentExecutionGpuTimes) {
  GTEST_SKIP() << "Replaced by Interactive2560SliderBaselinesDumpCurrentExecutionGpuTimes";
}

#if 0
void InteractiveDagBaselinesDumpCurrentExecutionGpuTimesRetiredDead() {
#if defined(_DEBUG)
  constexpr std::uint32_t kWidth   = 256;
  constexpr std::uint32_t kHeight  = 192;
  constexpr int           kRepeats = 1;
#else
  constexpr std::uint32_t kWidth   = 1920;
  constexpr std::uint32_t kHeight  = 1280;
  constexpr int           kRepeats = 11;
#endif
  prepared_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(kWidth, kHeight),
                                            gpu_dag_test::FullSensor(kWidth, kHeight));

  auto render_once = [&](std::uint64_t request_id, PipelineDocument& document,
                         std::string_view name) {
    const auto plan = Compile(document);
    (void)device_.Execute(plan, prepared_, document);
    device_.WaitIdle();
    device_.Workspace().ReleaseSessionResources();
    std::vector<diag::PreviewRequestRecord> measured;
    measured.reserve(static_cast<std::size_t>(kRepeats));
    for (int i = 0; i < kRepeats; ++i) {
      const auto record = Capture(request_id + static_cast<std::uint64_t>(i), document, plan);
      DumpRecord(name, record);
      EXPECT_EQ(record.gpu_status, diag::PreviewGpuTimeStatus::Available);
      measured.push_back(record);
      device_.Workspace().ReleaseSessionResources();
    }
    DumpP50(name, measured, kWidth, kHeight);
  };

  {
    auto document = multi_grade_test::MakeIdentityGradeDocument();
    AddExposure(document, NodeId{"grade.primary"}, 0.75f);
    render_once(1201, document, "baseline_three_node");
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
    render_once(1202, document, "baseline_four_node_second_masked");
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
    render_once(1203, document, "baseline_multi_grade_mask_mix");
  }
  {
    auto document = multi_grade_test::MakeIdentityGradeDocument();
    ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.look"}).empty());
    const auto red = PreviewDumpDirectory() / "lut_primary_large.cube";
    const auto blue = PreviewDumpDirectory() / "lut_second_large.cube";
    multi_grade_test::WriteConstantRgbCube(red, 1.0f, 0.0f, 0.0f);
    multi_grade_test::WriteConstantRgbCube(blue, 0.0f, 0.0f, 1.0f);
    multi_grade_test::GradeAdjustment<LmtModel>(document, NodeId{"grade.primary"}, type_ids::Lmt())
        .SetCubePath(red.string());
    multi_grade_test::GradeAdjustment<LmtModel>(document, NodeId{"grade.look"}, type_ids::Lmt())
        .SetCubePath(blue.string());
    render_once(1204, document, "baseline_two_lut_grades");
  }

#ifndef _DEBUG
  {
    std::filesystem::path raw_root;
    {
      auto cwd = std::filesystem::current_path();
      for (int i = 0; i < 8; ++i) {
        const auto candidate =
            cwd / "alcedo_studio" / "tests" / "resources" / "sample_images" / "ci_rawfiles";
        if (std::filesystem::exists(candidate)) {
          raw_root = candidate;
          break;
        }
        const auto parent = cwd.parent_path();
        if (parent == cwd) {
          break;
        }
        cwd = parent;
      }
    }
    std::filesystem::path raw_file;
    if (!raw_root.empty() && std::filesystem::exists(raw_root)) {
      for (const auto& entry : std::filesystem::directory_iterator(raw_root)) {
        const auto ext = entry.path().extension().string();
        if (ext == ".ARW" || ext == ".arw" || ext == ".DNG" || ext == ".dng") {
          raw_file = entry.path();
          break;
        }
      }
    }
    if (!raw_file.empty()) {
      std::ifstream input(raw_file, std::ios::binary);
      ASSERT_TRUE(static_cast<bool>(input)) << raw_file.string();
      const std::vector<char> chars((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
      std::vector<std::byte>  bytes(chars.size());
      std::memcpy(bytes.data(), chars.data(), chars.size());
      prepared_ = RawInputLoader::LoadEncoded(bytes, DecodeRes::FULL);
      EXPECT_EQ(prepared_.downsample_passes, 0);
      device_.Workspace().ReleaseSessionResources();
      auto document = multi_grade_test::MakeIdentityGradeDocument();
      AddExposure(document, NodeId{"grade.primary"}, 0.75f);
      const auto plan = Compile(document);
      (void)device_.Execute(plan, prepared_, document);
      device_.WaitIdle();
      device_.Workspace().ReleaseSessionResources();
      const auto record = Capture(1301, document, plan);
      DumpRecord("baseline_three_node_bayer_full", record);
      DumpP50("baseline_three_node_bayer_full", {record},
              prepared_.develop_output_extent.width, prepared_.develop_output_extent.height);
      const auto* develop = FindPass(record, "develop", diag::PreviewPassKind::UploadRaw);
      ASSERT_NE(develop, nullptr);
      EXPECT_EQ(develop->state, diag::PreviewExecutionState::Executed);
      EXPECT_EQ(develop->gpu_status, diag::PreviewGpuTimeStatus::Available);
    }
  }
#endif
}
#endif

}  // namespace alcedo
