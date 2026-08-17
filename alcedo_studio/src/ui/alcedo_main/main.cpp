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
#include <QSGRendererInterface>
#include <QWindow>
#include <QSettings>
#include <QString>
#include <QtGlobal>

#include <exiv2/error.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "alcedo_version.hpp"
#include "ui/alcedo_main/application_module_qml_types.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/album_backend/editor_scope_controller.hpp"
#include "ui/alcedo_main/language_manager.hpp"
#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/editor_startup.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "utils/diagnostics/app_logging.hpp"
#include "utils/clock/time_provider.hpp"

#ifdef Q_OS_WIN
#include "windows_frameless_window.hpp"
#endif
#ifdef Q_OS_MACOS
#include "macos_frameless_window.hpp"
#endif

namespace {

constexpr auto kAcceleratorBackendSettingsKey = "gpu/acceleratorBackend";
// Hard-coded so pre-QApplication reads hit the same registry/ini path ProjectModule
// writes after QApplication exists. Do not rely on QSettings{} alone before QApp.
constexpr auto kSettingsOrganization = "Alcedo";
constexpr auto kSettingsApplication  = "Alcedo";

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

// Same semantics as --editor-backend: accept only cuda|opencl|metal tokens that
// this build can host. "auto"/"cpu" are pipeline preferences, not RHI backends.
auto ParseStoredEditorBackendToken(const QString& raw)
    -> std::optional<alcedo::editor_rhi::EditorBackend> {
  const QString stored_value = raw.trimmed().toLower();
  if (stored_value.isEmpty()) {
    return std::nullopt;
  }
  const QByteArray encoded_value = stored_value.toUtf8();
  const auto parsed = alcedo::editor_rhi::ParseEditorBackendToken(
      std::string_view(encoded_value.constData(), static_cast<size_t>(encoded_value.size())));
  if (!parsed.has_value()) {
    qWarning("Ignoring unsupported saved accelerator backend: %s", encoded_value.constData());
    return std::nullopt;
  }
  if (!alcedo::editor_rhi::IsBackendAvailableInThisBuild(*parsed)) {
    qWarning("Saved accelerator backend is unavailable in this build: %s",
             alcedo::editor_rhi::ToString(*parsed));
    return std::nullopt;
  }
  return parsed;
}

// Explicit org/app so this works before QApplication (and matches ProjectModule writes).
auto ReadConfiguredEditorBackend() -> std::optional<alcedo::editor_rhi::EditorBackend> {
  const QSettings settings(QSettings::NativeFormat, QSettings::UserScope,
                           QLatin1String(kSettingsOrganization),
                           QLatin1String(kSettingsApplication));
  const QString stored_value =
      settings.value(QLatin1String(kAcceleratorBackendSettingsKey)).toString();
  qInfo("editor.backend.settings key=%s value=\"%s\" file=%s", kAcceleratorBackendSettingsKey,
        qPrintable(stored_value), qPrintable(settings.fileName()));
  return ParseStoredEditorBackendToken(stored_value);
}

auto ToAcceleratorPreference(alcedo::editor_rhi::EditorBackend backend)
    -> alcedo::AcceleratorBackendPreference {
  switch (backend) {
    case alcedo::editor_rhi::EditorBackend::Cuda:
      return alcedo::AcceleratorBackendPreference::CUDA;
    case alcedo::editor_rhi::EditorBackend::OpenCl:
      return alcedo::AcceleratorBackendPreference::OpenCL;
    case alcedo::editor_rhi::EditorBackend::Metal:
      return alcedo::AcceleratorBackendPreference::Metal;
  }
  return alcedo::AcceleratorBackendPreference::CPU;
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

  // Keep the same org/app identity ProjectModule uses after QApplication exists.
  QCoreApplication::setOrganizationName(QStringLiteral("Alcedo"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("alcedo.app"));
  QCoreApplication::setApplicationName(QStringLiteral("Alcedo"));
  QCoreApplication::setApplicationVersion(QStringLiteral(ALCEDO_APP_VERSION));

  // Priority matches the old manual override model:
  //   1) --editor-backend (debug/force, same as before)
  //   2) QSettings gpu/acceleratorBackend (Settings → Acceleration)
  //   3) platform default
  const auto backend_parse = alcedo::editor_rhi::ParseEditorBackendArgs(argc, argv);
  if (backend_parse.present && !backend_parse.error.empty()) {
    qCritical("Invalid --editor-backend: %s", backend_parse.error.c_str());
    return 1;
  }

  alcedo::editor_rhi::EditorBackend editor_backend;
  const char*                      backend_source = "default";
  if (backend_parse.backend.has_value()) {
    editor_backend = *backend_parse.backend;
    backend_source = "cli";
  } else if (const auto configured = ReadConfiguredEditorBackend(); configured.has_value()) {
    editor_backend = *configured;
    backend_source = "settings";
  } else if (const auto def = alcedo::editor_rhi::DefaultEditorBackendForPlatform()) {
    editor_backend = *def;
    backend_source = "default";
  } else {
    qCritical("No editor backend available for this platform/build");
    return 1;
  }

  // Graphics API must be locked before QApplication creates any RHI/window
  // scaffolding. ApplyEditorBackendBeforeWindow repeats the same selection after
  // QApp for adapter init; doing both keeps OpenCL from silently landing on D3D11.
  if (editor_backend == alcedo::editor_rhi::EditorBackend::OpenCl) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
  } else if (editor_backend == alcedo::editor_rhi::EditorBackend::Cuda) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
  } else if (editor_backend == alcedo::editor_rhi::EditorBackend::Metal) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
  }

  alcedo::TimeProvider::Refresh();
  alcedo::RegisterAllOperators();
  Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::error);

  QApplication app(argc, argv);
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
      << QStringLiteral("editor.backend=%1 source=%2 qt_api=%3 adapter=%4")
             .arg(QString::fromStdString(startup.diagnostics.backend_name),
                  QString::fromUtf8(backend_source),
                  QString::fromStdString(startup.diagnostics.qt_graphics_api),
                  QString::fromStdString(startup.diagnostics.adapter_description.empty()
                                             ? startup.diagnostics.notes
                                             : startup.diagnostics.adapter_description));

  // Platform window / taskbar fallback icon.
  // Windows: multi-res ICO (Explorer taskbar + Alt-Tab). EXE also embeds the
  // same ICO via alcedo_main.rc. Other non-Apple platforms use the PNG master.
  // On macOS, do not replace the bundle icon at runtime: Dock and Finder load
  // the ICNS resource through CFBundleIconFile, preserving the system-rendered
  // icon appearance while the application is running.
#if !defined(Q_OS_MACOS)
  {
#if defined(Q_OS_WIN)
    QIcon app_icon(QStringLiteral(":/ICON/alcedo_icon.ico"));
    if (app_icon.isNull()) {
      app_icon = QIcon(QStringLiteral(":/ICON/alcedo_icon.png"));
    }
#else
    QIcon app_icon(QStringLiteral(":/ICON/alcedo_icon.png"));
#endif
    app.setWindowIcon(app_icon);
  }
#endif
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
  app_modules.project()->SetRuntimeAcceleratorPreference(
      ToAcceleratorPreference(editor_backend));
  alcedo::ui::RegisterApplicationModuleTypes();
  alcedo::editor_rhi::RegisterEditorViewportQmlTypes();

  QQmlApplicationEngine engine;
  engine.addImportPath("qrc:/");
  language_manager.AttachEngine(&engine);
  app_modules.AttachQmlEngine(&engine);
  engine.rootContext()->setContextProperty("appModules", &app_modules);
  engine.rootContext()->setContextProperty("appTheme", &alcedo::ui::AppTheme::Instance());
  engine.rootContext()->setContextProperty("languageManager", &language_manager);
  engine.rootContext()->setContextProperty("automationMode", false);
#ifdef Q_OS_WIN
  engine.rootContext()->setContextProperty("nativeFrameManaged", true);
#else
  engine.rootContext()->setContextProperty("nativeFrameManaged", false);
#endif
  // Production starts as the real maximized app. Tests and the automation host
  // leave this unset so they keep the declared 1200x760 windowed geometry.
  engine.rootContext()->setContextProperty("startMaximized", true);

  QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                   []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

  engine.loadFromModule("Alcedo.Main", "Main");

#ifdef Q_OS_WIN
  alcedo::ui::WindowsFramelessWindow native_window_frame;
#endif
#ifdef Q_OS_MACOS
  alcedo::ui::MacosFramelessWindow native_window_frame;
#endif

  // Install platform frame behavior before the hidden production window is
  // shown, then bind the editor renderer to that final native window.
  if (!engine.rootObjects().isEmpty()) {
    if (auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst())) {
#ifdef Q_OS_WIN
      if (!native_window_frame.Install(window)) {
        qWarning("Could not install the native Windows frame integration");
      }
#endif
#ifdef Q_OS_MACOS
      if (!native_window_frame.Install(window)) {
        qWarning("Could not install the native macOS traffic-light integration");
      }
#endif
      alcedo::editor_rhi::BindEditorGraphicsToWindow(window, startup);
      window->setProperty("nativeFrameReady", true);
      window->showMaximized();
    }
  }

  const int exit_code = app.exec();
  qCInfo(alcedo::diag::appLog) << "app.exit code=" << exit_code;
  alcedo::diag::ShutdownApplicationLogging();
  return exit_code;
}
