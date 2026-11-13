//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The editor session owns its working document (EditorWorkingDocument) and publishes immutable
// preview snapshots from it. Renders and the GUI read only those previews, so a slider write on the
// session owner thread never waits for a frame: it takes no lock that a render holds.

#include "app/editor_working_document.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "app/editor_pipeline_command_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/executor_role.hpp"
#include "json.hpp"
#include "support/editor_lease_test_support.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {
namespace {

using alcedo::test::WithColorGradeTarget;

constexpr sl_element_id_t kElementId = 4101;

/// Exposure of the current panel read the way the editor reads it; NaN when unavailable.
auto                      ExposureEv(const alcedo::PipelineDocument& document) -> double {
  std::string    error;
  const auto     target = alcedo::CompleteCurrentPanelParameterTarget(document, "exposure", &error);
  nlohmann::json json;
  if (!target.has_value() || !alcedo::ReadEditorParameterJson(document, *target, &json, &error) ||
      !json.contains("exposure_ev")) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return json.at("exposure_ev").get<double>();
}

/// Write @p ev into the current exposure panel field of @p document.
void WriteExposure(alcedo::PipelineDocument& document, double ev) {
  std::string error;
  const auto  target = alcedo::CompleteCurrentPanelParameterTarget(document, "exposure", &error);
  ASSERT_TRUE(target.has_value()) << error;
  ASSERT_TRUE(alcedo::ApplyEditorParameterPatch(document, *target,
                                                nlohmann::json{{"exposure_ev", ev}}, &error))
      << error;
}

TEST(EditorWorkingDocumentTest, ConstructionPublishesAPreviewOfTheWorkingDocument) {
  alcedo::EditorWorkingDocument working(kElementId, std::make_shared<alcedo::PipelineDocument>(
                                                        alcedo::CreateDefaultPipelineDocument()));

  const auto                    preview = working.CurrentPreview();
  ASSERT_NE(preview, nullptr);
  EXPECT_FALSE(preview->IsCommitted());
  EXPECT_EQ(preview->ElementId(), kElementId);
  EXPECT_EQ(preview->Lineage(), working.Lineage());
  EXPECT_FALSE(preview->Head().has_value());
  EXPECT_EQ(preview->Document().ToJson(), working.Document().ToJson());
}

TEST(EditorWorkingDocumentTest, PublishedPreviewKeepsItsValuesAfterALaterWrite) {
  alcedo::EditorWorkingDocument working(kElementId, std::make_shared<alcedo::PipelineDocument>(
                                                        alcedo::CreateDefaultPipelineDocument()));
  const auto                    first    = working.CurrentPreview();
  const auto                    first_ev = ExposureEv(first->Document());
  ASSERT_FALSE(std::isnan(first_ev));

  WriteExposure(working.Document(), first_ev + 1.25);
  // Until the owner publishes, readers keep the earlier preview.
  EXPECT_EQ(working.CurrentPreview(), first);

  const auto second = working.PublishPreview();
  EXPECT_EQ(working.CurrentPreview(), second);
  EXPECT_NE(second, first);
  EXPECT_DOUBLE_EQ(ExposureEv(first->Document()), first_ev);
  EXPECT_DOUBLE_EQ(ExposureEv(second->Document()), first_ev + 1.25);
  // Same loaded history: the executor keeps its binding between the two frames.
  EXPECT_EQ(second->Lineage(), first->Lineage());
}

TEST(EditorWorkingDocumentTest, ReplaceTakesANewLineageAndPublishesOnlyWhenAsked) {
  alcedo::EditorWorkingDocument working(kElementId, std::make_shared<alcedo::PipelineDocument>(
                                                        alcedo::CreateDefaultPipelineDocument()));
  const auto                    before         = working.CurrentPreview();
  const auto                    before_lineage = working.Lineage();

  auto                          replacement =
      std::make_shared<alcedo::PipelineDocument>(alcedo::CreateDefaultPipelineDocument());
  WriteExposure(*replacement, 2.0);
  working.Replace(replacement);

  EXPECT_NE(working.Lineage(), before_lineage);
  EXPECT_EQ(&working.Document(), replacement.get());
  EXPECT_EQ(working.CurrentPreview(), before);

  const auto after = working.PublishPreview();
  EXPECT_EQ(after->Lineage(), working.Lineage());
  EXPECT_DOUBLE_EQ(ExposureEv(after->Document()), 2.0);
  EXPECT_THROW(working.Replace(nullptr), std::invalid_argument);
  EXPECT_EQ(&working.Document(), replacement.get());
}

class EditorSliderPathTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    journal_path_ = std::filesystem::temp_directory_path() / ("slider_path_" + stamp + ".wal");
    pipeline_     = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(EditorSessionPipelineMappers{
        {}, [](sl_element_id_t id) { return alcedo::test::MakeInMemoryEditorLease(id); }});
    history_.SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
    history_.SetPipelinePort(pipeline_);
  }

  void TearDown() override {
    history_.Release({kElementId, true});
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);
  }

  std::filesystem::path                      journal_path_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  EditorSessionHistoryPort                   history_;
};

// Exit criterion of P6: a slider tick on the session owner thread takes no lock that a frame
// holds. The editor executor's render lock stays held for the whole run (a frame that waits for a
// present slot), and every tick and the settle still complete and publish their previews.
TEST_F(EditorSliderPathTest, SliderTicksCompleteWhileAFrameHoldsTheEditorExecutorLock) {
  std::string error;
  const auto  handle = history_.Acquire(kElementId, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto start_ev = ExposureEv(pipeline_->CurrentPreview(kElementId)->Document());
  ASSERT_FALSE(std::isnan(start_ev));

  alcedo::PipelineExecutor executor(alcedo::ExecutorRole::Interactive);
  std::promise<void>       frame_holds_lock;
  std::promise<void>       release_frame;
  std::thread              frame([&] {
    std::unique_lock<std::mutex> render_lock(executor.GetRenderLock());
    frame_holds_lock.set_value();
    release_frame.get_future().wait();
  });
  frame_holds_lock.get_future().wait();

  auto       ticks  = std::async(std::launch::async, [&] {
    std::string tick_error;
    for (int tick = 1; tick <= 8; ++tick) {
      const auto patch = WithColorGradeTarget(
          {"exposure", nlohmann::json{{"exposure", start_ev + 0.1 * tick}}.dump(), false});
      if (!history_.CaptureAdjustmentBeforePreview(handle, patch, &tick_error)) {
        return tick_error;
      }
      const auto preview_ev = ExposureEv(pipeline_->CurrentPreview(kElementId)->Document());
      if (std::abs(preview_ev - (start_ev + 0.1 * tick)) > 1e-5) {
        return std::string("preview of tick ") + std::to_string(tick) + " holds " +
               std::to_string(preview_ev);
      }
    }
    const auto settled = WithColorGradeTarget(
        {"exposure", nlohmann::json{{"exposure", start_ev + 0.8}}.dump(), true});
    if (!history_.CommitAdjustment(handle, settled, &tick_error)) {
      return tick_error;
    }
    return std::string{};
  });

  const auto status = ticks.wait_for(std::chrono::seconds(10));
  release_frame.set_value();
  frame.join();
  ASSERT_EQ(status, std::future_status::ready)
      << "a slider tick waited for the lock the frame holds";
  EXPECT_EQ(ticks.get(), "");

  alcedo::EditorHistorySnapshot snapshot;
  ASSERT_TRUE(history_.ReadHistorySnapshot(handle, &snapshot, &error)) << error;
  EXPECT_TRUE(snapshot.active_head.has_value());
  EXPECT_NEAR(ExposureEv(pipeline_->CurrentPreview(kElementId)->Document()), start_ev + 0.8, 1e-5);
}

}  // namespace
}  // namespace alcedo::ui
