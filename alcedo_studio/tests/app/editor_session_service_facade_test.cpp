//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "app/editor_render_coordinator.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"

namespace alcedo {
namespace {

class EditorSessionServiceFacadeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto runtime = EditorSessionRuntime::Create();
    service_     = std::move(runtime->service);
    service_->SetPresentationSinkId(1);
    service_->SetPresentationSize(640, 480);
  }

  std::unique_ptr<EditorSessionService> service_;
};

TEST_F(EditorSessionServiceFacadeTest, OpenRoutesToLoadingAndReportsIdentity) {
  const auto result = service_->Open(100, 200);
  EXPECT_EQ(service_->state(), EditorSessionState::Loading);
  EXPECT_EQ(service_->identity().element_id, static_cast<sl_element_id_t>(100));
  EXPECT_EQ(service_->identity().image_id, static_cast<image_id_t>(200));
  EXPECT_TRUE(service_->has_image());
  EXPECT_TRUE(service_->active());
  // The facade routes Open through Submit -> HandleOpenOrSwitch -> lifecycle.
  EXPECT_TRUE(result.kind == EditorSessionResultKind::RenderRouted ||
              result.kind == EditorSessionResultKind::StateChanged);
}

TEST_F(EditorSessionServiceFacadeTest, RejectsOpenWithZeroElementId) {
  const auto result = service_->Open(0, 0);
  EXPECT_EQ(result.kind, EditorSessionResultKind::StateChanged);
  EXPECT_EQ(service_->state(), EditorSessionState::NoImage);
}

TEST_F(EditorSessionServiceFacadeTest, ShutdownRejectsFurtherOpens) {
  service_->Open(1, 2);
  service_->Shutdown();
  EXPECT_EQ(service_->state(), EditorSessionState::ShuttingDown);
  const auto rejected = service_->Open(5, 6);
  EXPECT_EQ(rejected.kind, EditorSessionResultKind::Rejected);
}

TEST_F(EditorSessionServiceFacadeTest, ResultsAreRecordedForObserverOrdering) {
  service_->Open(1, 2);
  const auto results = service_->results();
  EXPECT_FALSE(results.empty());
  // The first result after Open should carry the identity.
  EXPECT_EQ(results.front().identity.element_id, static_cast<sl_element_id_t>(1));
}

}  // namespace
}  // namespace alcedo