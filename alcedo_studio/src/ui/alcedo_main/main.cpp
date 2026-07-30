//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <qqml.h>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QString>
#include <QtGlobal>

#include <exiv2/error.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/album_backend/editor_scope_controller.hpp"
#include "ui/alcedo_main/language_manager.hpp"
#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/editor_startup.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "utils/diagnostics/app_logging.hpp"
#include "utils/clock/time_provider.hpp"

namespace {

void RegisterApplicationModuleTypes() {
  qmlRegisterUncreatableType<alcedo::ui::ProjectModule>(
      "Alcedo.Main", 1, 0, "ProjectModule", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::LibraryModule>(
      "Alcedo.Main", 1, 0, "LibraryModule", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::FolderController>(
      "Alcedo.Main", 1, 0, "FolderController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::ImageController>(
      "Alcedo.Main", 1, 0, "ImageController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::StatsEngine>(
      "Alcedo.Main", 1, 0, "StatsEngine", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::SearchController>(
      "Alcedo.Main", 1, 0, "SearchController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::ImportExportHandler>(
      "Alcedo.Main", 1, 0, "ImportExportHandler", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::NikonHeRecoveryController>(
      "Alcedo.Main", 1, 0, "NikonHeRecoveryController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::EditorController>(
      "Alcedo.Main", 1, 0, "EditorController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::BackgroundTaskController>(
      "Alcedo.Main", 1, 0, "BackgroundTaskController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::InteractionPolicyController>(
      "Alcedo.Main", 1, 0, "InteractionPolicyController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::ModelDownloadController>(
      "Alcedo.Main", 1, 0, "ModelDownloadController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::SemanticGenerationController>(
      "Alcedo.Main", 1, 0, "SemanticGenerationController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::AiProviderProfileController>(
      "Alcedo.Main", 1, 0, "AiProviderProfileController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::ImageAnalysisController>(
      "Alcedo.Main", 1, 0, "ImageAnalysisController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::AdjustmentTransferController>(
      "Alcedo.Main", 1, 0, "AdjustmentTransferController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::EditorSessionController>(
      "Alcedo.Main", 1, 0, "EditorSessionController", "Owned by ApplicationModuleHost");
  qmlRegisterUncreatableType<alcedo::ui::EditorScopeController>(
      "Alcedo.Main", 1, 0, "EditorScopeController", "Owned by EditorSessionController");
  qmlRegisterUncreatableType<alcedo::ui::WorkspaceRouter>(
      "Alcedo.Main", 1, 0, "WorkspaceRouter", "Owned by ApplicationModuleHost");
}

auto FindArgValue(int argc, char** argv, std::string_view option_name)
    -> std::optional<std::string_view> {
  const std::string opt_eq = std::string(option_name) + "=";
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i] ? argv[i] : "");
    if (arg == option_name) {
      if (i + 1 < argc && argv[i + 1]) {
        return std::string_view(argv[i + 1]);
      }
      return std::nullopt;
    }
    if (arg.rfind(opt_eq, 0) == 0) {
      return arg.substr(opt_eq.size());
    }
  }
  return std::nullopt;
}

}  // namespace

int main(int argc, char* argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#else
  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
      Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif

  // Parse --editor-backend before QApplication so ShareOpenGLContexts can be set
  // when OpenCL is selected. Backend application runs after QApplication exists.
  // const auto backend_parse = alcedo::editor_rhi::ParseEditorBackendArgs(argc, argv);
  const auto backend_parse = alcedo::editor_rhi::ParseEditorBackendArgs(argc, argv);
  if (backend_parse.present && !backend_parse.error.empty()) {
    qCritical("Invalid --editor-backend: %s", backend_parse.error.c_str());
    return 1;
  }

  alcedo::editor_rhi::EditorBackend editor_backend;
  if (backend_parse.backend.has_value()) {
    editor_backend = *backend_parse.backend;
  } else if (const auto def = alcedo::editor_rhi::DefaultEditorBackendForPlatform()) {
    editor_backend = *def;
  } else {
    qCritical("No editor backend available for this platform/build");
    return 1;
  }

  if (editor_backend == alcedo::editor_rhi::EditorBackend::OpenCl) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  }


  alcedo::TimeProvider::Refresh();
  alcedo::RegisterAllOperators();
  Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::error);

  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("Alcedo"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("alcedo.app"));
  QCoreApplication::setApplicationName(QStringLiteral("Alcedo"));
  const QString log_path = alcedo::diag::InitializeApplicationLogging();
  qCInfo(alcedo::diag::appLog).noquote()
      << QStringLiteral("app.start log_path=%1").arg(log_path);

  // After QApplication, before any QQuickWindow / QML load: select graphics API
  // and initialize CUDA adapter or OpenCL/GL sharing.
  const auto startup = alcedo::editor_rhi::ApplyEditorBackendBeforeWindow(editor_backend);
  if (!startup.ok) {
    qCritical("Editor backend startup failed (%s): %s",
              alcedo::editor_rhi::ToString(editor_backend), startup.error.c_str());
    alcedo::diag::ShutdownApplicationLogging();
    return 1;
  }
  qCInfo(alcedo::diag::appLog).noquote()
      << QStringLiteral("editor.backend=%1 qt_api=%2 adapter=%3")
             .arg(QString::fromStdString(startup.diagnostics.backend_name),
                  QString::fromStdString(startup.diagnostics.qt_graphics_api),
                  QString::fromStdString(startup.diagnostics.adapter_description.empty()
                                             ? startup.diagnostics.notes
                                             : startup.diagnostics.adapter_description));

  app.setWindowIcon(QIcon(QStringLiteral(":/ICON/alcedo_icon.png")));
  {
    QFont default_font = app.font();
    default_font.setStyleStrategy(QFont::PreferAntialias);
    app.setFont(default_font);
  }
  alcedo::ui::AppTheme::RegisterFonts();
  if (const auto arg = FindArgValue(argc, argv, "--font"); arg.has_value()) {
    alcedo::ui::AppTheme::TryRegisterUiFontOverride(QString::fromUtf8(arg->data(), arg->size()));
  } else if (const auto env = qEnvironmentVariable("ALCEDO_FONT_PATH"); !env.isEmpty()) {
    alcedo::ui::AppTheme::TryRegisterUiFontOverride(env);
  }
  alcedo::ui::LanguageManager language_manager(&app);
  alcedo::ui::AppTheme::SetEffectiveLanguageCode(language_manager.EffectiveLanguageCode());
  alcedo::ui::AppTheme::ApplyApplicationFont(app);
  QObject::connect(&language_manager, &alcedo::ui::LanguageManager::EffectiveLanguageCodeChanged,
                   &app, [&app, &language_manager]() {
                     alcedo::ui::AppTheme::SetEffectiveLanguageCode(
                         language_manager.EffectiveLanguageCode());
                     alcedo::ui::AppTheme::ApplyApplicationFont(app);
                   });
  // Basic (not Material): Material injects large paddings, ripples, and layered
  // chrome that break dense editor adjustment panels. Controls paint their own
  // backgrounds; Basic leaves those custom surfaces alone.
  QQuickStyle::setStyle("Basic");

  alcedo::ui::ApplicationModuleHost app_modules;
  RegisterApplicationModuleTypes();
  alcedo::editor_rhi::RegisterEditorViewportQmlTypes();

  QQmlApplicationEngine engine;
  engine.addImportPath("qrc:/");
  language_manager.AttachEngine(&engine);
  engine.rootContext()->setContextProperty("appModules", &app_modules);
  engine.rootContext()->setContextProperty("appTheme", &alcedo::ui::AppTheme::Instance());
  engine.rootContext()->setContextProperty("languageManager", &language_manager);

  QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                   []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

  engine.loadFromModule("Alcedo.Main", "Main");

  // Bind CUDA adapter LUID to the first created QQuickWindow when present.
  if (!engine.rootObjects().isEmpty()) {
    if (auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst())) {
      alcedo::editor_rhi::BindEditorGraphicsToWindow(window, startup);
    }
  }

  const int exit_code = app.exec();
  qCInfo(alcedo::diag::appLog) << "app.exit code=" << exit_code;
  alcedo::diag::ShutdownApplicationLogging();
  return exit_code;
}
