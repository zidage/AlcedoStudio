//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_lifecycle.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "support/editor_session_test_ports.hpp"

namespace alcedo {
namespace {

class EditorSessionLifecycleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    pipeline_ = std::make_shared<test::FakeEditorPipelinePort>();
    history_  = std::make_shared<test::FakeEditorHistoryPort>();

    EditorSessionLifecycle::Dependencies deps;
    deps.pipeline = pipeline_;
    deps.history  = history_;
    lifecycle_    = std::make_unique<EditorSessionLifecycle>(std::move(deps));
  }

  void OpenImage(sl_element_id_t eid = 100, image_id_t iid = 200) {
    std::string error;
    ASSERT_TRUE(lifecycle_->BeginAcquire(eid, iid, false, nullptr, &error)) << error;
    ASSERT_TRUE(lifecycle_->AcquireGuards(&error)) << error;
    lifecycle_->MarkImageReady();
    lifecycle_->MarkFirstFramePresented();
  }

  std::shared_ptr<test::FakeEditorPipelinePort> pipeline_;
  std::shared_ptr<test::FakeEditorHistoryPort>  history_;
  std::unique_ptr<EditorSessionLifecycle>       lifecycle_;
};

TEST_F(EditorSessionLifecycleTest, BeginAcquireAndAcquireGuardsSucceed) {
  std::string error;
  EXPECT_TRUE(lifecycle_->BeginAcquire(100, 200, false, nullptr, &error)) << error;
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Acquiring);
  EXPECT_EQ(lifecycle_->identity().element_id, static_cast<sl_element_id_t>(100));
  EXPECT_EQ(lifecycle_->identity().image_id, static_cast<image_id_t>(200));
  EXPECT_EQ(lifecycle_->identity().session_generation, 1u);

  EXPECT_TRUE(lifecycle_->AcquireGuards(&error)) << error;
  EXPECT_EQ(pipeline_->acquire_count, 1);
  EXPECT_EQ(history_->acquire_count, 1);
  EXPECT_TRUE(lifecycle_->has_history_guard());
  const auto guard = lifecycle_->history_guard();
  EXPECT_TRUE(guard.valid);
  EXPECT_EQ(guard.element_id, static_cast<sl_element_id_t>(100));
}

TEST_F(EditorSessionLifecycleTest, PipelineAcquireFailureReturnsFalseAndNoHistoryAcquire) {
  pipeline_->fail_acquire = true;
  ASSERT_TRUE(lifecycle_->BeginAcquire(100, 200, false, nullptr, nullptr));
  std::string error;
  EXPECT_FALSE(lifecycle_->AcquireGuards(&error));
  EXPECT_EQ(pipeline_->acquire_count, 1);
  EXPECT_EQ(history_->acquire_count, 0);
  EXPECT_FALSE(lifecycle_->has_history_guard());
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Failed);
}

TEST_F(EditorSessionLifecycleTest, HistoryAcquireFailureReleasesPipelineGuard) {
  history_->fail_acquire = true;
  ASSERT_TRUE(lifecycle_->BeginAcquire(100, 200, false, nullptr, nullptr));
  std::string error;
  EXPECT_FALSE(lifecycle_->AcquireGuards(&error));
  EXPECT_EQ(pipeline_->acquire_count, 1);
  EXPECT_EQ(history_->acquire_count, 1);
  EXPECT_EQ(pipeline_->release_count, 1);
  EXPECT_FALSE(lifecycle_->has_history_guard());
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Failed);
}

TEST_F(EditorSessionLifecycleTest, ReleaseGuardsReleasesBothExactlyOnce) {
  ASSERT_TRUE(lifecycle_->BeginAcquire(100, 200, false, nullptr, nullptr));
  ASSERT_TRUE(lifecycle_->AcquireGuards(nullptr));
  lifecycle_->ReleaseGuards();
  EXPECT_EQ(pipeline_->release_count, 1);
  EXPECT_EQ(history_->release_count, 1);
  EXPECT_FALSE(lifecycle_->has_history_guard());
  lifecycle_->ReleaseGuards();
  EXPECT_EQ(pipeline_->release_count, 1);
  EXPECT_EQ(history_->release_count, 1);
}

TEST_F(EditorSessionLifecycleTest, ReleaseAfterCheckpointReleasesAndReturnsIdentity) {
  ASSERT_TRUE(lifecycle_->BeginAcquire(100, 200, false, nullptr, nullptr));
  ASSERT_TRUE(lifecycle_->AcquireGuards(nullptr));
  const auto outcome = lifecycle_->ReleaseAfterCheckpoint();
  EXPECT_TRUE(outcome.released);
  EXPECT_EQ(outcome.identity.element_id, static_cast<sl_element_id_t>(100));
  EXPECT_EQ(pipeline_->release_count, 1);
  EXPECT_EQ(history_->release_count, 1);
  EXPECT_FALSE(lifecycle_->has_history_guard());
}

TEST_F(EditorSessionLifecycleTest, FullAcquireToInteractiveSequence) {
  OpenImage();
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Interactive);
  EXPECT_TRUE(lifecycle_->has_image());
  EXPECT_TRUE(lifecycle_->active());
  EXPECT_TRUE(lifecycle_->last_error().empty());
}

TEST_F(EditorSessionLifecycleTest, SameImageReopenAdvancesSessionGeneration) {
  OpenImage(1, 2);
  const auto gen1 = lifecycle_->identity();
  EXPECT_EQ(gen1.element_id, static_cast<sl_element_id_t>(1));
  EXPECT_EQ(gen1.image_id, static_cast<image_id_t>(2));
  EXPECT_EQ(gen1.session_generation, 1u);
  EXPECT_EQ(gen1.render_generation, 1u);
  EXPECT_EQ(gen1.view_generation, 1u);

  ASSERT_TRUE(lifecycle_->BeginAcquire(1, 2, false, nullptr, nullptr));
  const auto gen2 = lifecycle_->identity();
  EXPECT_EQ(gen2.session_generation, 2u);
  EXPECT_EQ(gen2.render_generation, 2u);
}

TEST_F(EditorSessionLifecycleTest, FailedSwitchKeepsCurrentImage) {
  OpenImage(1, 2);
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Interactive);

  lifecycle_->KeepCurrentAfterCheckpointFailure("save failed");
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Failed);
  EXPECT_EQ(lifecycle_->last_error(), "save failed");
  EXPECT_EQ(lifecycle_->identity().element_id, static_cast<sl_element_id_t>(1));
}

TEST_F(EditorSessionLifecycleTest, BeginShutdownTransitionsToShuttingDown) {
  OpenImage();
  lifecycle_->BeginShutdown();
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::ShuttingDown);
  EXPECT_FALSE(lifecycle_->active());
}

TEST_F(EditorSessionLifecycleTest, AdvanceRenderAndViewGenerationsAreIndependent) {
  OpenImage(1, 2);
  EXPECT_EQ(lifecycle_->AdvanceRenderGeneration(), 2u);
  EXPECT_EQ(lifecycle_->AdvanceRenderGeneration(), 3u);
  EXPECT_EQ(lifecycle_->AdvanceViewGeneration(), 2u);
  const auto id = lifecycle_->identity();
  EXPECT_EQ(id.render_generation, 3u);
  EXPECT_EQ(id.view_generation, 2u);
}

TEST_F(EditorSessionLifecycleTest, FailTransitionsToFailedWithError) {
  lifecycle_->Fail("something broke");
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Failed);
  EXPECT_EQ(lifecycle_->last_error(), "something broke");
}

TEST_F(EditorSessionLifecycleTest, BeginCheckpointTransitionsToSaving) {
  OpenImage();
  lifecycle_->BeginCheckpoint();
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Saving);
}

TEST_F(EditorSessionLifecycleTest, CompleteCheckpointReturnsToInteractive) {
  OpenImage();
  lifecycle_->BeginCheckpoint();
  lifecycle_->CompleteCheckpoint();
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Interactive);
}

TEST_F(EditorSessionLifecycleTest, MatchesIdentityFiltersCorrectly) {
  OpenImage(100, 200);
  EXPECT_TRUE(lifecycle_->MatchesIdentity(100, 200, 1));
  EXPECT_FALSE(lifecycle_->MatchesIdentity(100, 200, 2));
  EXPECT_FALSE(lifecycle_->MatchesIdentity(99, 200, 1));
}

TEST_F(EditorSessionLifecycleTest, BeginAcquireSwitchSetsSwitchingState) {
  std::string error;
  ASSERT_TRUE(lifecycle_->BeginAcquire(100, 200, true, nullptr, &error)) << error;
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Switching);
}

TEST_F(EditorSessionLifecycleTest, CompleteCloseTransitionsToNoImage) {
  OpenImage();
  lifecycle_->ReleaseGuards();
  lifecycle_->CompleteClose();
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::NoImage);
  EXPECT_FALSE(lifecycle_->has_image());
}

TEST_F(EditorSessionLifecycleTest, BeginRetryFromDiscardTransitionsToLoading) {
  OpenImage();
  lifecycle_->Fail("render failed");
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Failed);
  lifecycle_->BeginRetryFromDiscard();
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Loading);
}

}  // namespace
}  // namespace alcedo
