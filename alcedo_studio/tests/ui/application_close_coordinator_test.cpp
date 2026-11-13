//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// ApplicationCloseCoordinator tests. The editor session is a real EditorSessionController over a
// recording backend whose close completes when the test says so. The project close tests use a
// real project of ApplicationModuleHost.

#include "ui/alcedo_main/album_backend/application_close_coordinator.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <string>

#include "app/editor_session_service.hpp"
#include "app/editor_session_types.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/workspace_router.hpp"

namespace alcedo::ui::test {
namespace {

using alcedo::EditorSessionIdentity;
using alcedo::EditorSessionResult;
using alcedo::EditorSessionResultKind;
using alcedo::EditorSessionState;

/// Editor session backend whose Close queues until CompleteClose or FailClose runs, like the
/// owner-thread close of EditorSessionService.
class DeferredCloseBackend final : public alcedo::IEditorSessionBackend {
 public:
  [[nodiscard]] auto state() const -> EditorSessionState override { return state_; }
  [[nodiscard]] auto identity() const -> EditorSessionIdentity override { return identity_; }
  [[nodiscard]] auto active() const -> bool override {
    return state_ != EditorSessionState::NoImage && state_ != EditorSessionState::ShuttingDown;
  }
  [[nodiscard]] auto has_image() const -> bool override {
    return identity_.element_id > 0 && identity_.image_id > 0 &&
           alcedo::EditorSessionHasImage(state_);
  }
  [[nodiscard]] auto last_error() const -> std::string override { return last_error_; }
  void               SetPresentationSinkId(alcedo::PresentationSinkId) override {}
  void               SetPresentationSize(int, int) override {}

  auto Open(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override {
    identity_.element_id = element_id;
    identity_.image_id   = image_id;
    state_               = EditorSessionState::Interactive;
    NotifyChange();
    return Result(EditorSessionResultKind::StateChanged);
  }
  auto Switch(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override {
    return Open(element_id, image_id);
  }
  auto Close(bool persist_changes) -> EditorSessionResult override {
    ++close_count_;
    last_close_persist_ = persist_changes;
    // The owner thread runs the close later; the image stays open until then.
    return Result(EditorSessionResultKind::Accepted);
  }
  auto Shutdown() -> EditorSessionResult override {
    state_    = EditorSessionState::ShuttingDown;
    identity_ = {};
    NotifyChange();
    return Result(EditorSessionResultKind::StateChanged);
  }
  auto Discard() -> EditorSessionResult override {
    return Result(EditorSessionResultKind::Accepted);
  }
  auto Undo() -> EditorSessionResult override { return Discard(); }
  auto Redo() -> EditorSessionResult override { return Discard(); }

  void CompleteClose() {
    state_    = EditorSessionState::NoImage;
    identity_ = {};
    NotifyChange();
  }
  /// The save of the close fails and the session keeps the image, as the checkpoint failure of
  /// EditorSessionService does.
  void FailClose(const std::string& message) {
    last_error_ = message;
    state_      = EditorSessionState::RetainedImageFailure;
    NotifyChange();
  }

  int  close_count_        = 0;
  bool last_close_persist_ = false;

 private:
  auto Result(EditorSessionResultKind kind) const -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = kind;
    result.state    = state_;
    result.identity = identity_;
    return result;
  }

  EditorSessionState    state_ = EditorSessionState::NoImage;
  EditorSessionIdentity identity_{};
  std::string           last_error_;
};

class ApplicationCloseCoordinatorTest : public ApplicationModuleHostTestFixture {};

TEST_F(ApplicationCloseCoordinatorTest, CloseWaitsForSaveAndFinishesOnNoImage) {
  DeferredCloseBackend    backend;
  EditorSessionController editor(&backend);
  WorkspaceRouter         router(&editor);
  editor.Open(4, 7);
  router.OpenEditor(4, 7);
  ApplicationCloseCoordinator close(&editor, &router, nullptr);
  ASSERT_TRUE(close.CloseNeedsConfirmation());

  QSignalSpy finished(&close, &ApplicationCloseCoordinator::ApplicationCloseFinished);
  QSignalSpy aborted(&close, &ApplicationCloseCoordinator::ApplicationCloseAborted);
  close.BeginSave();
  ProcessEvents(100);

  EXPECT_EQ(backend.close_count_, 1);
  EXPECT_TRUE(backend.last_close_persist_);
  EXPECT_TRUE(editor.close_in_flight());
  EXPECT_TRUE(close.waiting());
  EXPECT_TRUE(close.CloseNeedsConfirmation());
  EXPECT_EQ(router.workspace(), QStringLiteral("library"));
  EXPECT_EQ(finished.count(), 0);

  backend.CompleteClose();
  ProcessEvents(100);

  EXPECT_EQ(finished.count(), 1);
  EXPECT_EQ(aborted.count(), 0);
  EXPECT_FALSE(close.waiting());
}

TEST_F(ApplicationCloseCoordinatorTest, SaveFailureAbortsApplicationCloseWithSessionError) {
  DeferredCloseBackend    backend;
  EditorSessionController editor(&backend);
  editor.Open(4, 7);
  ApplicationCloseCoordinator close(&editor, nullptr, nullptr);

  QSignalSpy finished(&close, &ApplicationCloseCoordinator::ApplicationCloseFinished);
  QSignalSpy aborted(&close, &ApplicationCloseCoordinator::ApplicationCloseAborted);
  close.BeginSave();
  ProcessEvents(100);
  backend.FailClose("The disk is full.");
  ProcessEvents(100);

  EXPECT_EQ(finished.count(), 0);
  ASSERT_EQ(aborted.count(), 1);
  EXPECT_EQ(aborted.front().front().toString(), QStringLiteral("The disk is full."));
  EXPECT_FALSE(close.waiting());
}

TEST_F(ApplicationCloseCoordinatorTest, SaveFailureAbortsClose) {
  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));
  const auto              package_path = host.project()->handler().package_path();

  DeferredCloseBackend    backend;
  EditorSessionController editor(&backend);
  editor.Open(4, 7);
  ApplicationCloseCoordinator close(&editor, nullptr, host.project());

  QSignalSpy                  finished(&close, &ApplicationCloseCoordinator::ProjectCloseFinished);
  ASSERT_TRUE(close.BeginProjectClose(/*persist=*/true).isEmpty());
  ProcessEvents(100);
  EXPECT_EQ(backend.close_count_, 1);
  EXPECT_TRUE(backend.last_close_persist_);
  backend.FailClose("The disk is full.");
  ProcessEvents(100);

  ASSERT_EQ(finished.count(), 1);
  EXPECT_FALSE(finished.front().at(0).toBool());
  EXPECT_EQ(finished.front().at(1).toString(), QStringLiteral("The disk is full."));
  // The project stays open and the editor keeps the retained image.
  EXPECT_TRUE(host.project()->ServiceReady());
  EXPECT_TRUE(host.project()->ProjectEntered());
  EXPECT_EQ(host.project()->handler().package_path(), package_path);
  EXPECT_EQ(editor.session_state(), EditorSessionState::RetainedImageFailure);
}

TEST_F(ApplicationCloseCoordinatorTest, ProjectCloseFinalizesEditorThenClosesProject) {
  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));

  DeferredCloseBackend    backend;
  EditorSessionController editor(&backend);
  editor.Open(4, 7);
  ApplicationCloseCoordinator close(&editor, nullptr, host.project());

  QSignalSpy                  finished(&close, &ApplicationCloseCoordinator::ProjectCloseFinished);
  ASSERT_TRUE(close.BeginProjectClose(/*persist=*/false).isEmpty());
  ProcessEvents(100);
  EXPECT_FALSE(backend.last_close_persist_);
  EXPECT_TRUE(host.project()->ServiceReady());
  backend.CompleteClose();
  ProcessEvents(100);

  ASSERT_EQ(finished.count(), 1);
  EXPECT_TRUE(finished.front().at(0).toBool());
  EXPECT_FALSE(host.project()->ServiceReady());
  EXPECT_FALSE(host.project()->ProjectEntered());
  EXPECT_FALSE(host.project()->handler().project());
}

TEST_F(ApplicationCloseCoordinatorTest, ProjectCloseIsRejectedWithoutLoadedProject) {
  ApplicationModuleHost       host;
  ApplicationCloseCoordinator close(host.editor_session(), host.workspace_router(), host.project());
  EXPECT_FALSE(close.BeginProjectClose(true).isEmpty());
  EXPECT_FALSE(close.waiting());
}

}  // namespace
}  // namespace alcedo::ui::test
