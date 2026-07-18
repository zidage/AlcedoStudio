//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/album_backend_test_fixture.hpp"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTimer>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "app/editor_render_intent.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/language_manager.hpp"
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
    const auto scheduled = host.editor_session_production_scheduler()->last_scheduled();
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
    expected_presented_count += 2;
    ASSERT_TRUE(WaitUntil([&] { return viewport->presentedFrameCount() >= expected_presented_count; },
                          std::chrono::minutes(2)))
        << "QualityBase was not acknowledged after InteractivePrimary";
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
  EXPECT_GE(presented_count, switch_count * 2);
  EXPECT_EQ(host.editor_session_production_scheduler()->pending_present_request_id(), 0u);

  host.workspace_router()->OpenLibrary();
  ProcessEvents(200);
  EXPECT_FALSE(host.editor_session()->presentation_viewport_bound());
  EXPECT_TRUE(warnings.empty()) << (warnings.empty() ? std::string{} : warnings.front().toString().toStdString());
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
