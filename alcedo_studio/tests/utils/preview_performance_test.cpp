//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_pending_input.hpp"
#include "app/editor_monotonic_clock.hpp"
#include "utils/diagnostics/preview_performance.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace alcedo {
namespace {

class ManualPreviewClock final : public IPreviewMonotonicClock {
 public:
  void SetNs(std::int64_t now_ns) { now_ns_ = now_ns; }
  auto NowNs() const -> std::int64_t override { return now_ns_; }

 private:
  std::int64_t now_ns_ = 0;
};

class ManualEditorClock final : public IEditorMonotonicClock {
 public:
  void SetNs(std::int64_t now_ns) { now_ns_ = now_ns; }
  auto NowNs() const -> std::int64_t override { return now_ns_; }

 private:
  std::int64_t now_ns_ = 0;
};

class PreviewPerformanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    diag::PreviewPerformance::ResetForTesting();
    clock_ = std::make_shared<ManualPreviewClock>();
    diag::PreviewPerformance::SetClock(clock_);
  }
  void TearDown() override { diag::PreviewPerformance::ResetForTesting(); }

  auto EnableDetail() -> void {
    diag::PreviewPerformance::SetMode(diag::PreviewPerformanceMode::Detail);
  }

  std::shared_ptr<ManualPreviewClock> clock_;
};

auto TestIdentity() -> EditorSessionIdentity {
  EditorSessionIdentity identity;
  identity.element_id = 7;
  identity.image_id   = 70;
  return identity;
}

auto MakePatch(std::string field, float value) -> EditorAdjustmentPatch {
  EditorAdjustmentPatch patch;
  patch.field_key = std::move(field);
  patch.write     = EditorScalarWrite{value};
  return patch;
}

TEST_F(PreviewPerformanceTest, DisabledTimingDoesNotAllocateOrQueueEvents) {
  clock_->SetNs(100);
  diag::PreviewPerformance::NoteSubmit(1, diag::PreviewFrameRole::InteractivePrimary,
                                       diag::PreviewQuality::Interactive, "InteractiveAdjustment",
                                       true);
  diag::PreviewPerformance::BindCurrentRequest(1);
  {
    diag::PreviewCpuInterval encode(diag::PreviewCpuStage::Encode);
    diag::PreviewPassInterval pass("develop", diag::PreviewPassKind::UploadRaw);
    diag::PreviewPerformance::NoteDevelopDecode({});
  }
  diag::PreviewPerformance::NoteDisplayed(1);
  diag::PreviewPerformance::FlushWriter();

  EXPECT_EQ(diag::PreviewPerformance::Mode(), diag::PreviewPerformanceMode::Off);
  EXPECT_EQ(diag::PreviewPerformance::EventsQueued(), 0u);
  EXPECT_EQ(diag::PreviewPerformance::EventsLost(), 0u);
  EXPECT_EQ(diag::PreviewPerformance::PendingSampleCount(), 0u);
  EXPECT_EQ(diag::PreviewPerformance::InternCount(), 0u);
  EXPECT_TRUE(diag::PreviewPerformance::WrittenLog().empty());
}

TEST_F(PreviewPerformanceTest, CoalescedInputsRetainFirstAndLatestAcceptedTimes) {
  auto editor_clock = std::make_shared<ManualEditorClock>();
  EditorPendingInputQueue queue;
  queue.SetClock(editor_clock);

  editor_clock->SetNs(1'000);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("exposure", 0.10f)).accepted);
  editor_clock->SetNs(5'000);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("exposure", 0.30f)).accepted);

  const auto batch = queue.TakeReadyBatch();
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->first_accepted_ns, 1'000);
  EXPECT_EQ(batch->latest_accepted_ns, 5'000);
  EXPECT_EQ(batch->fields.size(), 1u);

  EnableDetail();
  std::vector<diag::PreviewRequestRecord> records;
  std::mutex                              records_mutex;
  diag::PreviewPerformance::InstallRecordSink([&](const diag::PreviewRequestRecord& record) {
    std::lock_guard lock(records_mutex);
    records.push_back(record);
  });

  clock_->SetNs(6'000);
  diag::PreviewPerformance::NoteSubmit(11, diag::PreviewFrameRole::InteractivePrimary,
                                       diag::PreviewQuality::Interactive, "InteractiveAdjustment",
                                       true);
  diag::PreviewPerformance::NoteInputTimes(11, batch->sequence_id, batch->first_accepted_ns,
                                           batch->latest_accepted_ns);
  clock_->SetNs(7'000);
  diag::PreviewPerformance::NoteDisplayed(11);
  diag::PreviewPerformance::FlushWriter();

  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].input_sequence_id, batch->sequence_id);
  EXPECT_EQ(records[0].first_accepted_ns, 1'000);
  EXPECT_EQ(records[0].latest_accepted_ns, 5'000);
  EXPECT_TRUE(records[0].has_user_input);
}

TEST_F(PreviewPerformanceTest, PresentedFrameTimingMatchesConsumedRequest) {
  EnableDetail();
  std::vector<diag::PreviewRequestRecord> records;
  diag::PreviewPerformance::InstallRecordSink(
      [&](const diag::PreviewRequestRecord& record) { records.push_back(record); });

  clock_->SetNs(10);
  diag::PreviewPerformance::NoteSubmit(21, diag::PreviewFrameRole::InteractivePrimary,
                                       diag::PreviewQuality::Interactive, "InteractiveAdjustment",
                                       true);
  diag::PreviewPerformance::NoteInputTimes(21, 3, 1, 2);
  clock_->SetNs(20);
  diag::PreviewPerformance::NotePresentWake(21);
  clock_->SetNs(30);
  diag::PreviewPerformance::NoteGuiUpdate();
  clock_->SetNs(40);
  diag::PreviewPerformance::NoteRenderEnter();
  diag::PreviewPerformance::NoteConsumeBegin(21);
  clock_->SetNs(50);
  diag::PreviewPerformance::NoteDisplayed(21);

  clock_->SetNs(80);
  diag::PreviewPerformance::NoteSubmit(22, diag::PreviewFrameRole::QualityBase,
                                       diag::PreviewQuality::Quality, "SettledAdjustment", false);
  clock_->SetNs(90);
  diag::PreviewPerformance::NoteRenderEnter();
  diag::PreviewPerformance::NoteConsumeBegin(22);
  clock_->SetNs(100);
  diag::PreviewPerformance::NoteDisplayed(22);
  diag::PreviewPerformance::FlushWriter();

  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].request_id, 21u);
  EXPECT_EQ(records[0].input_sequence_id, 3u);
  EXPECT_EQ(records[0].qt_frame, 1u);
  EXPECT_EQ(records[0].outcome, diag::PreviewTerminalOutcome::Presented);
  EXPECT_EQ(records[1].request_id, 22u);
  EXPECT_EQ(records[1].qt_frame, 2u);
  EXPECT_NE(records[0].qt_frame, records[1].qt_frame);
  EXPECT_EQ(diag::PreviewPerformance::PendingSampleCount(), 0u);
}

TEST_F(PreviewPerformanceTest, CancelledAndFailedRequestsReleaseTimingEntries) {
  EnableDetail();
  std::vector<diag::PreviewRequestRecord> records;
  diag::PreviewPerformance::InstallRecordSink(
      [&](const diag::PreviewRequestRecord& record) { records.push_back(record); });

  clock_->SetNs(1);
  diag::PreviewPerformance::NoteSubmit(31, diag::PreviewFrameRole::InteractivePrimary,
                                       diag::PreviewQuality::Interactive, "InteractiveAdjustment",
                                       true);
  diag::PreviewPerformance::NoteSubmit(32, diag::PreviewFrameRole::InteractivePrimary,
                                       diag::PreviewQuality::Interactive, "InteractiveAdjustment",
                                       true);
  EXPECT_EQ(diag::PreviewPerformance::PendingSampleCount(), 2u);

  diag::PreviewPerformance::NoteTerminal(31, diag::PreviewTerminalOutcome::Cancelled, "replaced");
  diag::PreviewPerformance::NoteTerminal(32, diag::PreviewTerminalOutcome::Failed, "encode");
  EXPECT_EQ(diag::PreviewPerformance::PendingSampleCount(), 0u);

  diag::PreviewPerformance::FlushWriter();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].outcome, diag::PreviewTerminalOutcome::Cancelled);
  EXPECT_EQ(records[1].outcome, diag::PreviewTerminalOutcome::Failed);
}

TEST_F(PreviewPerformanceTest, FullDiagnosticQueueDoesNotBlockRenderOwner) {
  EnableDetail();
  diag::PreviewPerformance::SetQueueCapacityForTesting(2);
  diag::PreviewPerformance::PauseWriterForTesting(true);

  for (std::uint64_t id = 1; id <= 4; ++id) {
    clock_->SetNs(static_cast<std::int64_t>(id) * 10);
    diag::PreviewPerformance::NoteSubmit(id, diag::PreviewFrameRole::InteractivePrimary,
                                         diag::PreviewQuality::Interactive, "InteractiveAdjustment",
                                         true);
    diag::PreviewPerformance::NoteDisplayed(id);
  }

  EXPECT_EQ(diag::PreviewPerformance::PendingSampleCount(), 0u);
  EXPECT_GE(diag::PreviewPerformance::EventsLost(), 2u);
  EXPECT_LE(diag::PreviewPerformance::EventsQueued(), 2u);

  diag::PreviewPerformance::PauseWriterForTesting(false);
  diag::PreviewPerformance::FlushWriter();
}

TEST_F(PreviewPerformanceTest, BackgroundWriterProducesCompleteStructuredRecords) {
  EnableDetail();
  const auto log_path =
      (std::filesystem::temp_directory_path() /
       ("alcedo_preview_perf_test_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log"))
          .string();
  std::error_code remove_error;
  std::filesystem::remove(log_path, remove_error);
  diag::PreviewPerformance::SetOutputPath(log_path);

  std::vector<diag::PreviewRequestRecord> records;
  diag::PreviewPerformance::InstallRecordSink(
      [&](const diag::PreviewRequestRecord& record) { records.push_back(record); });

  clock_->SetNs(100);
  diag::PreviewPerformance::NoteSubmit(41, diag::PreviewFrameRole::InteractivePrimary,
                                       diag::PreviewQuality::Interactive, "InteractiveAdjustment",
                                       true);
  diag::PreviewPerformance::NoteInputTimes(41, 9, 80, 90);
  diag::PreviewPerformance::BindCurrentRequest(41);
  clock_->SetNs(110);
  {
    diag::PreviewCpuInterval encode(diag::PreviewCpuStage::Encode);
    clock_->SetNs(130);
    diag::PreviewPassInterval pass("grade.primary", diag::PreviewPassKind::PrimaryColorGrade);
    {
      diag::PreviewSubStageInterval llf(diag::PreviewSubStageKind::LlfPyramid);
      clock_->SetNs(150);
    }
    {
      diag::PreviewSubStageInterval mix(diag::PreviewSubStageKind::Mix);
      clock_->SetNs(160);
    }
  }
  diag::PreviewDevelopDecodeParams develop;
  develop.decode_res             = diag::PreviewDecodeRes::Full;
  develop.cfa                    = diag::PreviewCfaKind::Bayer;
  develop.demosaic               = diag::PreviewDemosaicMethod::Legacy;
  develop.highlights_reconstruct = true;
  develop.host_width             = 64;
  develop.host_height            = 48;
  develop.develop_width          = 64;
  develop.develop_height         = 48;
  develop.full_ref_width         = 64;
  develop.full_ref_height        = 48;
  develop.layout                 = diag::PreviewDevelopLayout::FullFrame;
  diag::PreviewPerformance::NoteDevelopDecode(develop);

  diag::PreviewResourceSnapshot snapshot;
  snapshot.texture_used_bytes       = 4096;
  snapshot.texture_entry_count      = 2;
  snapshot.texture_allocation_count = 2;
  snapshot.texture_peak_used_bytes  = 4096;
  diag::PreviewPerformance::NoteResourceSnapshot(snapshot);

  clock_->SetNs(200);
  diag::PreviewPerformance::NoteRenderEnter();
  diag::PreviewPerformance::NoteConsumeBegin(41);
  clock_->SetNs(220);
  diag::PreviewPerformance::NoteDisplayed(41);
  diag::PreviewPerformance::ClearCurrentRequest();
  diag::PreviewPerformance::FlushWriter();

  ASSERT_EQ(records.size(), 1u);
  const auto& record = records[0];
  EXPECT_EQ(record.request_id, 41u);
  EXPECT_EQ(record.input_sequence_id, 9u);
  EXPECT_EQ(record.outcome, diag::PreviewTerminalOutcome::Presented);
  EXPECT_EQ(record.gpu_status, diag::PreviewGpuTimeStatus::Unavailable);
  EXPECT_TRUE(record.has_develop_decode);
  EXPECT_TRUE(record.has_resources);
  EXPECT_EQ(record.resources.texture_entry_count, 2u);
  ASSERT_FALSE(record.passes.empty());
  EXPECT_EQ(record.passes[0].owner, "grade.primary");
  ASSERT_GE(record.passes[0].sub_stages.size(), 2u);

  const auto text = diag::PreviewPerformance::WrittenLog();
  EXPECT_NE(text.find("#preview_perf v1"), std::string::npos);
  EXPECT_NE(text.find("request id=41"), std::string::npos);
  EXPECT_NE(text.find("gpu=unavailable"), std::string::npos);
  EXPECT_NE(text.find("decode_res=FULL"), std::string::npos);
  EXPECT_NE(text.find("kind=llf_pyramid"), std::string::npos);
  EXPECT_NE(text.find("kind=mix"), std::string::npos);
  EXPECT_NE(text.find("texture_allocation_count=2"), std::string::npos);

  diag::PreviewPerformance::ResetForTesting();
  std::ifstream file(log_path);
  ASSERT_TRUE(file.is_open());
  std::string file_text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  EXPECT_NE(file_text.find("request id=41"), std::string::npos);
  file.close();
  std::filesystem::remove(log_path, remove_error);
}

TEST_F(PreviewPerformanceTest, DevelopPassRecordIncludesDecodeParameters) {
  EnableDetail();
  std::vector<diag::PreviewRequestRecord> records;
  diag::PreviewPerformance::InstallRecordSink(
      [&](const diag::PreviewRequestRecord& record) { records.push_back(record); });

  clock_->SetNs(1);
  diag::PreviewPerformance::NoteSubmit(51, diag::PreviewFrameRole::InteractivePrimary,
                                       diag::PreviewQuality::Interactive, "InitialFrame", false);
  diag::PreviewPerformance::BindCurrentRequest(51);
  {
    diag::PreviewPassInterval pass("develop", diag::PreviewPassKind::UploadRaw);
    pass.SetState(diag::PreviewExecutionState::Skipped);
  }
  diag::PreviewDevelopDecodeParams params;
  params.decode_res             = diag::PreviewDecodeRes::Half;
  params.cfa                    = diag::PreviewCfaKind::XTrans;
  params.demosaic               = diag::PreviewDemosaicMethod::NeuralEngine;
  params.highlights_reconstruct = false;
  params.downsample_passes      = 1;
  params.host_width             = 32;
  params.host_height            = 24;
  params.develop_width          = 16;
  params.develop_height         = 12;
  params.full_ref_width         = 32;
  params.full_ref_height        = 24;
  params.layout                 = diag::PreviewDevelopLayout::Tiled;
  diag::PreviewPerformance::NoteDevelopDecode(params);
  diag::PreviewPerformance::NoteDisplayed(51);
  diag::PreviewPerformance::FlushWriter();

  ASSERT_EQ(records.size(), 1u);
  ASSERT_TRUE(records[0].has_develop_decode);
  EXPECT_EQ(records[0].develop.decode_res, diag::PreviewDecodeRes::Half);
  EXPECT_EQ(records[0].develop.cfa, diag::PreviewCfaKind::XTrans);
  EXPECT_EQ(records[0].develop.demosaic, diag::PreviewDemosaicMethod::NeuralEngine);
  EXPECT_EQ(records[0].develop.downsample_passes, 1);
  EXPECT_EQ(records[0].develop.layout, diag::PreviewDevelopLayout::Tiled);
  ASSERT_EQ(records[0].passes.size(), 1u);
  EXPECT_EQ(records[0].passes[0].state, diag::PreviewExecutionState::Skipped);
}

TEST_F(PreviewPerformanceTest, GradePassRecordsLlfAndMixSubStages) {
  EnableDetail();
  std::vector<diag::PreviewRequestRecord> records;
  diag::PreviewPerformance::InstallRecordSink(
      [&](const diag::PreviewRequestRecord& record) { records.push_back(record); });

  clock_->SetNs(1);
  diag::PreviewPerformance::NoteSubmit(61, diag::PreviewFrameRole::InteractivePrimary,
                                       diag::PreviewQuality::Interactive, "InteractiveAdjustment",
                                       true);
  diag::PreviewPerformance::BindCurrentRequest(61);
  clock_->SetNs(2);
  {
    diag::PreviewPassInterval pass("grade.a", diag::PreviewPassKind::PrimaryColorGrade);
    {
      diag::PreviewSubStageInterval pointwise(diag::PreviewSubStageKind::Pointwise);
      clock_->SetNs(4);
    }
    {
      diag::PreviewSubStageInterval llf(diag::PreviewSubStageKind::LlfRemap);
      clock_->SetNs(9);
    }
    {
      diag::PreviewSubStageInterval mix(diag::PreviewSubStageKind::Mix);
      clock_->SetNs(11);
    }
  }
  {
    diag::PreviewPassInterval aliased("grade.b", diag::PreviewPassKind::PrimaryColorGrade, 1);
    aliased.SetState(diag::PreviewExecutionState::Aliased);
  }
  diag::PreviewPerformance::NoteDisplayed(61);
  diag::PreviewPerformance::FlushWriter();

  ASSERT_EQ(records.size(), 1u);
  ASSERT_EQ(records[0].passes.size(), 2u);
  EXPECT_EQ(records[0].passes[0].owner, "grade.a");
  ASSERT_EQ(records[0].passes[0].sub_stages.size(), 3u);
  EXPECT_EQ(records[0].passes[0].sub_stages[0].kind, diag::PreviewSubStageKind::Pointwise);
  EXPECT_EQ(records[0].passes[0].sub_stages[1].kind, diag::PreviewSubStageKind::LlfRemap);
  EXPECT_EQ(records[0].passes[0].sub_stages[2].kind, diag::PreviewSubStageKind::Mix);
  EXPECT_GT(records[0].passes[0].sub_stages[1].cpu_ns, 0);
  EXPECT_EQ(records[0].passes[1].state, diag::PreviewExecutionState::Aliased);
  EXPECT_EQ(records[0].gpu_status, diag::PreviewGpuTimeStatus::Unavailable);
}

TEST_F(PreviewPerformanceTest, ResourceSnapshotReportsAggregatedPoolTotals) {
  EnableDetail();
  std::vector<diag::PreviewRequestRecord> records;
  diag::PreviewPerformance::InstallRecordSink(
      [&](const diag::PreviewRequestRecord& record) { records.push_back(record); });

  clock_->SetNs(1);
  diag::PreviewPerformance::NoteSubmit(71, diag::PreviewFrameRole::InteractivePrimary,
                                       diag::PreviewQuality::Interactive, "InteractiveAdjustment",
                                       true);
  diag::PreviewPerformance::BindCurrentRequest(71);
  diag::PreviewResourceSnapshot first;
  first.texture_used_bytes       = 1024;
  first.texture_entry_count      = 1;
  first.texture_allocation_count = 1;
  first.texture_peak_used_bytes  = 1024;
  diag::PreviewPerformance::NoteResourceSnapshot(first);
  diag::PreviewResourceSnapshot second;
  second.texture_used_bytes       = 4096;
  second.texture_leased_bytes     = 3072;
  second.texture_unleased_bytes   = 1024;
  second.texture_entry_count      = 2;
  second.texture_allocation_count = 2;
  second.texture_peak_used_bytes  = 4096;
  diag::PreviewPerformance::NoteResourceSnapshot(second);
  diag::PreviewPerformance::NoteDisplayed(71);
  diag::PreviewPerformance::FlushWriter();

  ASSERT_EQ(records.size(), 1u);
  ASSERT_TRUE(records[0].has_resources);
  EXPECT_EQ(records[0].resources.texture_used_bytes, 4096u);
  EXPECT_EQ(records[0].resources.texture_entry_count, 2u);
  EXPECT_EQ(records[0].resources.texture_allocation_count, 2u);
  EXPECT_EQ(records[0].resources.texture_peak_used_bytes, 4096u);
}

}  // namespace
}  // namespace alcedo
