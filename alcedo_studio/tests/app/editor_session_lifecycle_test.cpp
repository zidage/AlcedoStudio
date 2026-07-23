//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_lifecycle.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "app/editor_session_ports.hpp"

namespace alcedo {
namespace {

class FakePipelinePort final : public IEditorPipelinePort {
 public:
  bool fail_acquire  = false;
  int  acquire_count = 0;
  int  release_count = 0;

  auto Acquire(sl_element_id_t element_id, std::string* error)
      -> EditorPipelineGuardHandle override {
    ++acquire_count;
    if (fail_acquire) {
      if (error) {
        *error = "pipeline acquire failed";
      }
      return {};
    }
    return EditorPipelineGuardHandle{element_id, true};
  }
  void Release(const EditorPipelineGuardHandle&) override { ++release_count; }
};

class FakeHistoryPort final : public IEditorHistoryPort {
 public:
  bool fail_acquire  = false;
  int  acquire_count = 0;
  int  release_count = 0;

  auto Acquire(sl_element_id_t element_id, std::string* error)
      -> EditorHistoryGuardHandle override {
    ++acquire_count;
    if (fail_acquire) {
      if (error) {
        *error = "history acquire failed";
      }
      return {};
    }
    return EditorHistoryGuardHandle{element_id, true};
  }
  void Release(const EditorHistoryGuardHandle&) override { ++release_count; }
  auto Undo(const EditorHistoryGuardHandle&, std::string*) -> bool override { return true; }
  auto Redo(const EditorHistoryGuardHandle&, std::string*) -> bool override { return true; }
  auto ReadAdjustmentSnapshot(const EditorHistoryGuardHandle&, EditorRenderAdjustmentSnapshot*,
                              std::string*) -> bool override {
    return true;
  }
};

class EditorSessionLifecycleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    pipeline_ = std::make_shared<FakePipelinePort>();
    history_  = std::make_shared<FakeHistoryPort>();

    EditorSessionLifecycle::Dependencies deps;
    deps.pipeline = pipeline_;
    deps.history  = history_;
    lifecycle_    = std::make_unique<EditorSessionLifecycle>(std::move(deps));
  }

  std::shared_ptr<FakePipelinePort>       pipeline_;
  std::shared_ptr<FakeHistoryPort>        history_;
  std::unique_ptr<EditorSessionLifecycle> lifecycle_;
};

TEST_F(EditorSessionLifecycleTest, AcquireGuardsSucceedsAndStoresHandles) {
  std::string error;
  EXPECT_TRUE(lifecycle_->AcquireGuards(100, &error));
  EXPECT_EQ(pipeline_->acquire_count, 1);
  EXPECT_EQ(history_->acquire_count, 1);
  EXPECT_TRUE(lifecycle_->has_history_guard());
  const auto guard = lifecycle_->history_guard();
  EXPECT_TRUE(guard.valid);
  EXPECT_EQ(guard.element_id, static_cast<sl_element_id_t>(100));
}

TEST_F(EditorSessionLifecycleTest, PipelineAcquireFailureReturnsFalseAndNoHistoryAcquire) {
  pipeline_->fail_acquire = true;
  std::string error;
  EXPECT_FALSE(lifecycle_->AcquireGuards(100, &error));
  EXPECT_EQ(pipeline_->acquire_count, 1);
  EXPECT_EQ(history_->acquire_count, 0);
  EXPECT_FALSE(lifecycle_->has_history_guard());
}

TEST_F(EditorSessionLifecycleTest, HistoryAcquireFailureReleasesPipelineGuard) {
  history_->fail_acquire = true;
  std::string error;
  EXPECT_FALSE(lifecycle_->AcquireGuards(100, &error));
  EXPECT_EQ(pipeline_->acquire_count, 1);
  EXPECT_EQ(history_->acquire_count, 1);
  EXPECT_EQ(pipeline_->release_count, 1);
  EXPECT_FALSE(lifecycle_->has_history_guard());
}

TEST_F(EditorSessionLifecycleTest, ReleaseImageReleasesBothGuardsExactlyOnce) {
  ASSERT_TRUE(lifecycle_->AcquireGuards(100, nullptr));
  lifecycle_->ReleaseImage();
  EXPECT_EQ(pipeline_->release_count, 1);
  EXPECT_EQ(history_->release_count, 1);
  EXPECT_FALSE(lifecycle_->has_history_guard());
  // Second release is idempotent.
  lifecycle_->ReleaseImage();
  EXPECT_EQ(pipeline_->release_count, 1);
  EXPECT_EQ(history_->release_count, 1);
}

TEST_F(EditorSessionLifecycleTest, SameImageReopenAdvancesSessionGeneration) {
  lifecycle_->AdvanceSessionGeneration(1, 2);
  const auto gen1 = lifecycle_->identity();
  EXPECT_EQ(gen1.element_id, static_cast<sl_element_id_t>(1));
  EXPECT_EQ(gen1.image_id, static_cast<image_id_t>(2));
  EXPECT_EQ(gen1.session_generation, 1u);
  EXPECT_EQ(gen1.render_generation, 1u);
  EXPECT_EQ(gen1.view_generation, 1u);

  lifecycle_->AdvanceSessionGeneration(1, 2);
  const auto gen2 = lifecycle_->identity();
  EXPECT_EQ(gen2.session_generation, 2u);
  EXPECT_EQ(gen2.render_generation, 2u);
}

TEST_F(EditorSessionLifecycleTest, FailedSwitchRetainsCurrentImage) {
  lifecycle_->AdvanceSessionGeneration(1, 2);
  lifecycle_->TransitionTo(EditorSessionState::Interactive, EditorSessionResultKind::ImageReady,
                           "ready");
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Interactive);

  lifecycle_->KeepCurrentImageAfterFailure("save failed");
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Failed);
  EXPECT_EQ(lifecycle_->last_error(), "save failed");
  // Identity is preserved.
  EXPECT_EQ(lifecycle_->identity().element_id, static_cast<sl_element_id_t>(1));
}

TEST_F(EditorSessionLifecycleTest, ShutdownTransitionsToShuttingDown) {
  lifecycle_->TransitionTo(EditorSessionState::ShuttingDown, EditorSessionResultKind::StateChanged,
                           "shutting down");
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::ShuttingDown);
  EXPECT_FALSE(lifecycle_->active());
}

TEST_F(EditorSessionLifecycleTest, AdvanceRenderAndViewGenerationsAreIndependent) {
  lifecycle_->AdvanceSessionGeneration(1, 2);
  EXPECT_EQ(lifecycle_->AdvanceRenderGeneration(), 2u);
  EXPECT_EQ(lifecycle_->AdvanceRenderGeneration(), 3u);
  EXPECT_EQ(lifecycle_->AdvanceViewGeneration(), 2u);
  const auto id = lifecycle_->identity();
  EXPECT_EQ(id.render_generation, 3u);
  EXPECT_EQ(id.view_generation, 2u);
}

}  // namespace
}  // namespace alcedo