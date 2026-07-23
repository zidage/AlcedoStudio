//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "app/editor_render_intent.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/language_manager.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/editor_rhi/editor_startup.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"

namespace alcedo::ui::test {
namespace {

editor_rhi::EditorBackend       g_backend = editor_rhi::EditorBackend::Cuda;
editor_rhi::EditorStartupResult g_startup{};

auto MainQmlUrl() -> QUrl {
  const auto path = std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml" /
                    "Main.qml";
#ifdef _WIN32
  return QUrl::fromLocalFile(QString::fromStdWString(path.wstring()));
#else
  return QUrl::fromLocalFile(QString::fromStdString(path.string()));
#endif
}

auto CollectCiRawFiles(std::size_t max_count = 2) -> std::vector<std::filesystem::path> {
  const std::filesystem::path root{std::string(TEST_IMG_PATH) + "/ci_rawfiles"};
  std::vector<std::filesystem::path> paths;
  if (!std::filesystem::exists(root)) {
    return paths;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (entry.is_regular_file() && is_supported_file(entry.path())) {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  if (paths.size() > max_count) {
    paths.resize(max_count);
  }
  return paths;
}

template <class Predicate>
auto WaitUntil(Predicate&& predicate, std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    ProcessEvents(20);
  }
  return predicate();
}

void WaitForImportFinished(ApplicationModuleHost& host) {
  ASSERT_TRUE(WaitUntil([&] { return !host.import_export()->ImportRunning(); },
                        std::chrono::minutes(2)));
  ProcessEvents(500);
}

class EditorRealRawGpuE2eTest : public ApplicationModuleHostTestFixture {};

TEST_F(EditorRealRawGpuE2eTest,
       RealRawGpuFramesRemainAcknowledgedAcrossSustainedImageSwitches) {
  if (!g_startup.ok) {
    GTEST_SKIP() << g_startup.error;
  }

  const auto raw_files = CollectCiRawFiles();
  if (raw_files.size() < 2) {
    GTEST_SKIP() << "Two CI RAW fixtures are required";
  }

  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));
  host.import_export()->StartImport(PathsToQStringList(raw_files));
  WaitForImportFinished(host);
  ASSERT_EQ(host.import_export()->ImportFailed(), 0);
  ASSERT_GE(host.library()->Thumbnails().size(), 2);

  LanguageManager language_manager{QCoreApplication::instance()};
  AppTheme::RegisterFonts();
  AppTheme::Instance().setReduceMotion(true);
  AppTheme::SetEffectiveLanguageCode(language_manager.EffectiveLanguageCode());
  QQuickStyle::setStyle(QStringLiteral("Material"));

  QQmlApplicationEngine engine;
  std::vector<QQmlError> warnings;
  engine.addImportPath(QStringLiteral("qrc:/"));
  language_manager.AttachEngine(&engine);
  engine.rootContext()->setContextProperty(QStringLiteral("appModules"), &host);
  engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
  engine.rootContext()->setContextProperty(QStringLiteral("languageManager"), &language_manager);
  QObject::connect(&engine, &QQmlEngine::warnings,
                   [&warnings](const QList<QQmlError>& emitted) {
                     warnings.insert(warnings.end(), emitted.begin(), emitted.end());
                   });
  engine.load(MainQmlUrl());
  ASSERT_FALSE(engine.rootObjects().empty());
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
  ASSERT_NE(window, nullptr);
  editor_rhi::BindEditorGraphicsToWindow(window, g_startup);
  window->show();
  window->requestActivate();
  ProcessEvents(500);

  struct ImageKey {
    uint element_id = 0;
    uint image_id   = 0;
  };
  std::vector<ImageKey> images;
  for (int i = 0; i < 2; ++i) {
    const QVariantMap item = host.library()->Thumbnails().at(i).toMap();
    images.push_back({item.value("elementId").toUInt(), item.value("imageId").toUInt()});
  }
  ASSERT_GT(images[0].element_id, 0u);
  ASSERT_GT(images[0].image_id, 0u);
  ASSERT_GT(images[1].element_id, 0u);
  ASSERT_GT(images[1].image_id, 0u);

  bool ok = false;
  const int configured_switches = qEnvironmentVariableIntValue("ALCEDO_REAL_RAW_E2E_SWITCHES", &ok);
  const int switch_count = ok ? std::max(1, configured_switches) : 8;
  qulonglong expected_presented_count = 0;
  std::uint64_t previous_request_id = 0;

  for (int i = 0; i < switch_count; ++i) {
    const auto key = images[static_cast<std::size_t>(i) % images.size()];
    host.workspace_router()->OpenEditor(key.element_id, key.image_id);

    ASSERT_TRUE(WaitUntil([&] { return host.editor_session()->presentation_viewport_bound(); },
                          std::chrono::seconds(5)));
    auto* viewport = qobject_cast<editor_rhi::EditorViewportItem*>(
        host.editor_session()->presentation_viewport());
    ASSERT_NE(viewport, nullptr);
    ASSERT_TRUE(WaitUntil([&] { return viewport->isVisible(); }, std::chrono::seconds(2)))
        << "Bound viewport did not become visible while loading";
    ASSERT_TRUE(
        WaitUntil([&] { return viewport->presentationAvailable(); }, std::chrono::seconds(2)))
        << "Visible viewport never published render-thread availability; status="
        << viewport->statusText().toStdString();

    if (i == 0) {
      const auto first_request_id = host.editor_session_service()->first_frame_request_id();
      ASSERT_NE(first_request_id, 0u);
      ASSERT_TRUE(WaitUntil(
          [&] {
            const auto results = host.editor_render_coordinator()->results();
            return std::any_of(results.begin(), results.end(),
                               [&](const EditorRenderResult& result) {
                                 return result.request_id == first_request_id &&
                                        result.kind == EditorRenderResultKind::FrameSubmitted;
                               });
          },
          std::chrono::minutes(2)))
          << "Primary frame never reached the submitted/visible state";

      auto* interaction = window->findChild<editor_rhi::EditorInteractionController*>(
          QStringLiteral("editorInteractionController"));
      ASSERT_NE(interaction, nullptr);
      const auto scheduled_before_zoom =
          host.editor_session_scheduler()->last_scheduled().size();

      interaction->handleDoubleTap(viewport->width() * 0.5, viewport->height() * 0.5);

      ASSERT_TRUE(WaitUntil(
          [&] {
            const auto requests = host.editor_session_scheduler()->last_scheduled();
            return requests.size() > scheduled_before_zoom &&
                   requests.back().intent.reason == EditorRenderReason::DetailRefresh &&
                   requests.back().intent.frame_role == FrameRole::DetailPatch;
          },
          std::chrono::seconds(5)))
          << "Settled double-click zoom did not reach the production scheduler as DetailPatch; "
          << "session=" << EditorSessionStateName(host.editor_session_service()->state());

      const auto detail_request = host.editor_session_scheduler()->last_scheduled().back();
      ASSERT_TRUE(detail_request.intent.view_region.has_value());
      EXPECT_LT(detail_request.intent.view_region->scale_x_, 1.0f);
      EXPECT_LT(detail_request.intent.view_region->scale_y_, 1.0f);
      ASSERT_TRUE(WaitUntil(
          [&] {
            const auto results = host.editor_render_coordinator()->results();
            return std::any_of(results.begin(), results.end(),
                               [&](const EditorRenderResult& result) {
                                 return result.request_id == detail_request.request_id &&
                                        result.kind == EditorRenderResultKind::FrameSubmitted;
                               });
          },
          std::chrono::minutes(2)))
          << "DetailPatch reached the scheduler but never submitted a frame";

      ASSERT_TRUE(WaitUntil([&] { return !host.editor_session()->render_busy(); },
                            std::chrono::seconds(10)))
          << "First DetailPatch left the coordinator busy";
      ProcessEvents(250);

      const auto scheduled_before_pan =
          host.editor_session_scheduler()->last_scheduled().size();
      const auto center_x = viewport->width() * 0.5;
      const auto center_y = viewport->height() * 0.5;
      interaction->handlePress(center_x, center_y, static_cast<int>(Qt::LeftButton));
      interaction->handleMove(center_x + 80.0, center_y + 30.0, static_cast<int>(Qt::LeftButton));
      interaction->handleRelease(center_x + 80.0, center_y + 30.0,
                                 static_cast<int>(Qt::LeftButton));

      ASSERT_TRUE(WaitUntil(
          [&] {
            return host.editor_session_scheduler()->last_scheduled().size() >
                   scheduled_before_pan;
          },
          std::chrono::seconds(5)))
          << "Settled pan did not schedule a replacement DetailPatch";
      const auto panned_detail = host.editor_session_scheduler()->last_scheduled().back();
      ASSERT_EQ(panned_detail.intent.frame_role, FrameRole::DetailPatch);
      EXPECT_NE(panned_detail.request_id, detail_request.request_id);
      ASSERT_TRUE(WaitUntil(
          [&] {
            const auto results = host.editor_render_coordinator()->results();
            return std::any_of(results.begin(), results.end(),
                               [&](const EditorRenderResult& result) {
                                 return result.request_id == panned_detail.request_id &&
                                        result.kind == EditorRenderResultKind::FrameSubmitted;
                               });
          },
          std::chrono::minutes(2)))
          << "Replacement DetailPatch after pan never submitted a frame; liveTargets="
          << viewport->liveTargetCount() << " status=" << viewport->statusText().toStdString();
      ASSERT_TRUE(WaitUntil([&] { return !host.editor_session()->render_busy(); },
                            std::chrono::seconds(10)))
          << "Replacement DetailPatch after pan left the coordinator busy";
    }

    const auto first_frame_ready = [&] {
      return host.editor_session_service()->state() == EditorSessionState::Interactive &&
             viewport->lastPresentedImageGeneration() ==
                 host.editor_session()->session_generation() &&
             viewport->lastPresentedRequestId() != 0 &&
             viewport->lastPresentedRequestId() != previous_request_id;
    };
    ASSERT_TRUE(WaitUntil(
        [&] {
          return first_frame_ready() ||
                 host.editor_session_service()->state() == EditorSessionState::Failed;
        },
        std::chrono::minutes(2)));
    const auto scheduled = host.editor_session_scheduler()->last_scheduled();
    const auto* last_intent = scheduled.empty() ? nullptr : &scheduled.back().intent;
    ASSERT_TRUE(first_frame_ready())
        << host.editor_session_service()->last_error() << " backend="
        << viewport->backendName().toStdString() << " status="
        << viewport->statusText().toStdString() << " available="
        << viewport->presentationAvailable() << " live=" << viewport->liveTargetCount()
        << " targetGen=" << viewport->targetGeneration() << " imageGen="
        << viewport->imageGeneration() << " item=" << viewport->width() << 'x'
        << viewport->height() << " requested=" << (last_intent ? last_intent->requested_width : 0)
        << 'x' << (last_intent ? last_intent->requested_height : 0)
        << " windowExposed=" << window->isExposed() << " itemVisible=" << viewport->isVisible()
        << " parentVisible="
        << (viewport->parentItem() ? viewport->parentItem()->isVisible() : false) << " opacity="
        << viewport->opacity() << " warning="
        << (warnings.empty() ? std::string{} : warnings.front().toString().toStdString());

    const auto first_request_id = viewport->lastPresentedRequestId();
    previous_request_id = first_request_id;
    // Direct-present composition counts every primary drawn into a Qt Quick
    // window frame (InteractivePrimary then QualityBase). Application-level
    // FramePresented is a one-shot first-frame composition event only.
    expected_presented_count += 2;
    ASSERT_TRUE(WaitUntil([&] { return viewport->presentedFrameCount() >= expected_presented_count; },
                          std::chrono::minutes(2)))
        << "QualityBase was not composed after InteractivePrimary";

    if (i == 0) {
      // Keep the adjustment drag open: no settled patch or pointer release
      // is sent. The QQuickRhiItem must still synchronize and compose a FAST
      // frame while QualityBase and the zoom DetailPatch have already exercised
      // the other presentation layers.
      const auto composed_before_drag = viewport->presentedFrameCount();
      const auto wakeups_before_drag = viewport->adjustmentFrameRequestCount();
      auto* session = host.editor_session();
      ASSERT_NE(session, nullptr);
      ASSERT_TRUE(session->submitPatch(QStringLiteral("exposure"),
                                       QStringLiteral(R"({"value":0.10})"), false));
      ASSERT_TRUE(session->submitPatch(QStringLiteral("exposure"),
                                       QStringLiteral(R"({"value":0.20})"), false));
      ASSERT_TRUE(session->submitPatch(QStringLiteral("exposure"),
                                       QStringLiteral(R"({"value":0.30})"), false));
      EXPECT_EQ(viewport->adjustmentFrameRequestCount(), wakeups_before_drag + 3);
      ASSERT_TRUE(WaitUntil([&] { return viewport->presentedFrameCount() > composed_before_drag; },
                            std::chrono::minutes(2)))
          << "FAST adjustment frame was not composed before pointer release; status="
          << viewport->statusText().toStdString()
          << " liveTargets=" << viewport->liveTargetCount();
    }

    EXPECT_EQ(viewport->lastPresentedImageGeneration(),
              host.editor_session()->session_generation());
    EXPECT_EQ(viewport->imageIdentity(), key.image_id);
    EXPECT_GT(viewport->liveTargetCount(), 0);
    EXPECT_TRUE(viewport->presentationAvailable());
  }

  const auto coordinator_results = host.editor_render_coordinator()->results();
  const auto presented_count = std::count_if(
      coordinator_results.begin(), coordinator_results.end(), [](const EditorRenderResult& result) {
        return result.kind == EditorRenderResultKind::FramePresented;
      });
  // Phase 5C: exactly one first-frame composition confirmation per image open.
  EXPECT_GE(presented_count, switch_count);
  EXPECT_EQ(host.editor_session_scheduler()->pending_present_request_id(), 0u);

  host.workspace_router()->OpenLibrary();
  ProcessEvents(200);
  EXPECT_FALSE(host.editor_session()->presentation_viewport_bound());
  EXPECT_TRUE(warnings.empty()) << (warnings.empty() ? std::string{}
                                                     : warnings.front().toString().toStdString());
}

}  // namespace
}  // namespace alcedo::ui::test

int main(int argc, char** argv) {
#ifdef _WIN32
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", QByteArray("windows"));
  }
#endif
  const QByteArray requested = qgetenv("ALCEDO_TEST_EDITOR_BACKEND").toLower();
  if (requested == "opencl") {
    alcedo::ui::test::g_backend = alcedo::editor_rhi::EditorBackend::OpenCl;
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  }

  QApplication app(argc, argv);
  alcedo::ui::test::g_startup =
      alcedo::editor_rhi::ApplyEditorBackendBeforeWindow(alcedo::ui::test::g_backend);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
