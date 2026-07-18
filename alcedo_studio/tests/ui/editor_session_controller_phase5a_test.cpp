//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// Phase 5A: EditorSessionController with a fake IEditorSessionBackend, and a
/// dependency scan proving editor UI modules do not call PipelineScheduler.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <fstream>
#include <string>
#include <vector>

#include "app/editor_session_service.hpp"
#include "app/editor_session_types.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

namespace alcedo::ui {
namespace {

class FakeSessionBackend final : public IEditorSessionBackend {
 public:
  EditorSessionState    state_ = EditorSessionState::NoImage;
  EditorSessionIdentity identity_{};
  PresentationSinkId    sink_id_ = 0;
  int                   open_count = 0;
  int                   switch_count = 0;
  int                   shutdown_count = 0;
  std::string           last_error_;

  auto state() const -> EditorSessionState override { return state_; }
  auto identity() const -> EditorSessionIdentity override { return identity_; }
  auto active() const -> bool override {
    return state_ != EditorSessionState::NoImage && state_ != EditorSessionState::ShuttingDown;
  }
  auto has_image() const -> bool override {
    return identity_.element_id > 0 && identity_.image_id > 0 && EditorSessionHasImage(state_);
  }
  auto last_error() const -> std::string override { return last_error_; }

  void SetPresentationSinkId(PresentationSinkId sink_id) override { sink_id_ = sink_id; }

  auto Open(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override {
    ++open_count;
    if (element_id == 0 || image_id == 0) {
      identity_ = {};
      state_    = EditorSessionState::NoImage;
    } else {
      ++identity_.session_generation;
      identity_.element_id        = element_id;
      identity_.image_id          = image_id;
      identity_.render_generation = identity_.session_generation;
      identity_.view_generation   = 1;
      state_                      = EditorSessionState::Loading;
    }
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::StateChanged;
    result.state    = state_;
    result.identity = identity_;
    return result;
  }

  auto Switch(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override {
    ++switch_count;
    return Open(element_id, image_id);
  }

  auto Shutdown() -> EditorSessionResult override {
    ++shutdown_count;
    state_    = EditorSessionState::ShuttingDown;
    identity_ = {};
    EditorSessionResult result;
    result.kind  = EditorSessionResultKind::StateChanged;
    result.state = state_;
    return result;
  }

  auto Discard() -> EditorSessionResult override {
    EditorSessionResult result;
    result.kind  = EditorSessionResultKind::Accepted;
    result.state = state_;
    return result;
  }

  auto Undo() -> EditorSessionResult override { return Discard(); }
  auto Redo() -> EditorSessionResult override { return Discard(); }
};

TEST(EditorSessionControllerPhase5ATest, RoutesOpenThroughInjectedFakeBackend) {
  FakeSessionBackend backend;
  EditorSessionController controller(nullptr, &backend);
  int state_signals = 0;
  QObject::connect(&controller, &EditorSessionController::StateChanged, [&] { ++state_signals; });

  controller.Open(42, 84);
  EXPECT_EQ(backend.open_count, 1);
  EXPECT_TRUE(controller.active());
  EXPECT_TRUE(controller.has_image());
  EXPECT_EQ(controller.element_id(), 42u);
  EXPECT_EQ(controller.image_id(), 84u);
  EXPECT_EQ(controller.session_generation(), 1u);
  EXPECT_EQ(controller.session_state(), EditorSessionState::Loading);
  EXPECT_GE(state_signals, 1);

  controller.Open(100, 200);
  EXPECT_EQ(backend.switch_count, 1);
  EXPECT_EQ(controller.element_id(), 100u);
  EXPECT_EQ(controller.last_element_id(), 100u);

  controller.Close();
  EXPECT_FALSE(controller.has_image());
  EXPECT_EQ(controller.session_state(), EditorSessionState::NoImage);
}

TEST(EditorSessionControllerPhase5ATest, WorksWithoutBackendForShellOnlyTests) {
  EditorSessionController controller(static_cast<EditorController*>(nullptr));
  controller.Open(1, 2);
  EXPECT_TRUE(controller.has_image());
  EXPECT_EQ(controller.session_generation(), 1u);
  controller.Close();
  EXPECT_FALSE(controller.has_image());
}

auto ReadFileText(const std::string& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

TEST(EditorSessionControllerPhase5ATest, EditorUiModulesDoNotIncludePipelineScheduler) {
  // Phase 5A acceptance: no editor UI module or input controller calls the
  // pipeline scheduler directly. Allowlist is the legacy QWidget path only.
  const char* repo_root = ALCEDO_REPO_ROOT;
  const std::vector<std::string> forbidden_includes = {
      "renderer/pipeline_scheduler.hpp",
      "PipelineScheduler",
      "ScheduleTask(",
  };
  const std::vector<std::string> scanned_files = {
      std::string(repo_root) +
          "/alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_controller.cpp",
      std::string(repo_root) +
          "/alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_controller.hpp",
      std::string(repo_root) +
          "/alcedo_studio/src/include/ui/editor_rhi/editor_interaction_controller.hpp",
      std::string(repo_root) +
          "/alcedo_studio/src/ui/editor_rhi/editor_interaction_controller.cpp",
      std::string(repo_root) +
          "/alcedo_studio/src/include/ui/edit_viewer/view_transform_controller.hpp",
      std::string(repo_root) +
          "/alcedo_studio/src/ui/edit_viewer/view_transform_controller.cpp",
      std::string(repo_root) +
          "/alcedo_studio/src/include/ui/edit_viewer/crop_interaction_controller.hpp",
      std::string(repo_root) +
          "/alcedo_studio/src/ui/edit_viewer/crop_interaction_controller.cpp",
      std::string(repo_root) + "/alcedo_studio/src/app/editor_session_service.cpp",
  };

  for (const auto& path : scanned_files) {
    const std::string text = ReadFileText(path);
    ASSERT_FALSE(text.empty()) << "missing source: " << path;
    for (const auto& needle : forbidden_includes) {
      EXPECT_EQ(text.find(needle), std::string::npos)
          << path << " must not reference " << needle
          << "; only EditorRenderCoordinator may own scheduler calls";
    }
  }
}

}  // namespace
}  // namespace alcedo::ui
