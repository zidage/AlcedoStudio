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
  EXPECT_EQ(lifecycle_->active_image_load_request().value, 1u);

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

TEST_F(EditorSessionLifecycleTest, SameImageReopenAdvancesImageLoadRequest) {
  OpenImage(1, 2);
  const auto load1 = lifecycle_->active_image_load_request();
  EXPECT_EQ(lifecycle_->identity().element_id, static_cast<sl_element_id_t>(1));
  EXPECT_EQ(lifecycle_->identity().image_id, static_cast<image_id_t>(2));
  EXPECT_EQ(load1.value, 1u);

  ASSERT_TRUE(lifecycle_->BeginAcquire(1, 2, false, nullptr, nullptr));
  const auto load2 = lifecycle_->active_image_load_request();
  EXPECT_EQ(load2.value, 2u);
}

TEST_F(EditorSessionLifecycleTest, FailedSwitchKeepsCurrentImage) {
  OpenImage(1, 2);
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Interactive);

  lifecycle_->KeepCurrentAfterCheckpointFailure("save failed");
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::RetainedImageFailure);
  EXPECT_TRUE(lifecycle_->has_image());
  EXPECT_EQ(lifecycle_->last_error(), "save failed");
  EXPECT_EQ(lifecycle_->identity().element_id, static_cast<sl_element_id_t>(1));
}

TEST_F(EditorSessionLifecycleTest, RetainedImageFailureResumesInteractiveWithoutChangingIdentity) {
  OpenImage(1, 2);
  const auto prior_identity = lifecycle_->identity();
  const auto prior_load     = lifecycle_->active_image_load_request();

  lifecycle_->KeepCurrentAfterCheckpointFailure("save failed");
  lifecycle_->ResumeInteractiveAfterFailure();

  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Interactive);
  EXPECT_TRUE(lifecycle_->has_image());
  EXPECT_EQ(lifecycle_->identity().element_id, prior_identity.element_id);
  EXPECT_EQ(lifecycle_->identity().image_id, prior_identity.image_id);
  EXPECT_EQ(lifecycle_->active_image_load_request(), prior_load);
}

TEST_F(EditorSessionLifecycleTest, BeginShutdownTransitionsToShuttingDown) {
  OpenImage();
  lifecycle_->BeginShutdown();
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::ShuttingDown);
  EXPECT_FALSE(lifecycle_->active());
  EXPECT_FALSE(lifecycle_->active_image_load_request().valid());
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

TEST_F(EditorSessionLifecycleTest, MatchesIdentityFiltersByElementAndImage) {
  OpenImage(100, 200);
  EXPECT_TRUE(lifecycle_->MatchesIdentity(100, 200));
  EXPECT_FALSE(lifecycle_->MatchesIdentity(99, 200));
}

TEST_F(EditorSessionLifecycleTest, MatchesImageLoadRequestFiltersByLoadId) {
  OpenImage(100, 200);
  const auto load = lifecycle_->active_image_load_request();
  EXPECT_TRUE(lifecycle_->MatchesImageLoadRequest(load));
  EXPECT_FALSE(lifecycle_->MatchesImageLoadRequest(ImageLoadRequestId{load.value + 1}));
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
  EXPECT_FALSE(lifecycle_->active_image_load_request().valid());
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
