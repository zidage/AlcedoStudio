//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <vector>

#include "edit/operators/operator_registeration.hpp"
#include "test_probe.hpp"
#include "type/supported_file_type.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/application_module_qml_types.hpp"
#include "ui/alcedo_main/language_manager.hpp"
#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/editor_startup.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "utils/clock/time_provider.hpp"

namespace {

struct HostOptions {
  QString project_path;
  QString import_dir;
  QString probe_socket;
  bool    reuse_project = false;
};

auto BuildArgumentList(int argc, char** argv) -> QStringList {
  QStringList arguments;
  arguments.reserve(argc);
  for (int index = 0; index < argc; ++index) {
    arguments.append(QString::fromLocal8Bit(argv[index] != nullptr ? argv[index] : ""));
  }
  return arguments;
}

auto ParseOptions(int argc, char** argv) -> std::optional<HostOptions> {
  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("Alcedo Studio GUI host with a JSON Lines inspection probe."));
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addOption(QCommandLineOption(
      {QStringLiteral("project-path")},
      QStringLiteral("Packed .alcd file to open, or a directory in which to create one."),
      QStringLiteral("path")));
  parser.addOption(
      QCommandLineOption({QStringLiteral("import-dir")},
                         QStringLiteral("Directory recursively scanned for supported RAW files."),
                         QStringLiteral("path"), QStringLiteral(TEST_IMG_PATH "/raw/camera")));
  parser.addOption(QCommandLineOption({QStringLiteral("probe-socket")},
                                      QStringLiteral("QLocalSocket server name."),
                                      QStringLiteral("name")));
  parser.addOption(QCommandLineOption(
      {QStringLiteral("reuse-project")},
      QStringLiteral("Open an existing project without starting a new import.")));
  parser.addOption(QCommandLineOption(
      {QStringLiteral("editor-backend")},
      QStringLiteral("Editor backend token: cuda, opencl, or metal."), QStringLiteral("backend")));

  const QStringList arguments = BuildArgumentList(argc, argv);
  if (!parser.parse(arguments)) {
    qCritical().noquote() << parser.errorText();
    return std::nullopt;
  }
  if (parser.isSet(QStringLiteral("help"))) {
    std::cout << parser.helpText().toStdString() << std::endl;
    return std::nullopt;
  }
  if (parser.isSet(QStringLiteral("version"))) {
    std::cout << QCoreApplication::applicationVersion().toStdString() << std::endl;
    return std::nullopt;
  }

  HostOptions options;
  options.project_path  = parser.value(QStringLiteral("project-path")).trimmed();
  options.import_dir    = parser.value(QStringLiteral("import-dir")).trimmed();
  options.probe_socket  = parser.value(QStringLiteral("probe-socket")).trimmed();
  options.reuse_project = parser.isSet(QStringLiteral("reuse-project"));
  if (options.project_path.isEmpty()) {
    qCritical("--project-path is required");
    return std::nullopt;
  }
  if (options.import_dir.isEmpty()) {
    qCritical("--import-dir cannot be empty");
    return std::nullopt;
  }
  return options;
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

auto CollectImportPaths(const std::filesystem::path& root) -> std::vector<image_path_t> {
  std::vector<image_path_t> paths;
  std::error_code           error;
  if (!std::filesystem::is_directory(root, error) || error) {
    return paths;
  }

  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  while (iterator != end) {
    if (error) {
      error.clear();
      iterator.increment(error);
      continue;
    }
    const auto& entry = *iterator;
    if (entry.is_regular_file(error) && !error && alcedo::is_supported_file(entry.path())) {
      paths.push_back(entry.path());
    }
    error.clear();
    iterator.increment(error);
  }

  std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
    return left.generic_wstring() < right.generic_wstring();
  });
  return paths;
}

auto ResolveProjectPath(const QString& input) -> std::optional<std::filesystem::path> {
  const auto path = alcedo::ui::album_util::InputToPath(input);
  if (!path.has_value() || path->empty()) {
    return std::nullopt;
  }
  return path;
}

auto StartProject(alcedo::ui::ApplicationModuleHost& modules,
                  const std::filesystem::path&       project_path) -> bool {
  std::error_code error;
  if (std::filesystem::is_regular_file(project_path, error) && !error) {
    return modules.project()->LoadProject(alcedo::ui::album_util::PathToQString(project_path));
  }
  if (std::filesystem::exists(project_path, error) && !error &&
      !std::filesystem::is_directory(project_path, error)) {
    return false;
  }
  if (!std::filesystem::exists(project_path, error)) {
    std::filesystem::create_directories(project_path, error);
  }
  if (error || !std::filesystem::is_directory(project_path, error) || error) {
    return false;
  }
  return modules.project()->CreateProjectInFolderNamed(
      alcedo::ui::album_util::PathToQString(project_path), QStringLiteral("automation_project"));
}

auto ConfigureGraphicsApi(alcedo::editor_rhi::EditorBackend backend) -> bool {
  if (backend == alcedo::editor_rhi::EditorBackend::OpenCl) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
  } else if (backend == alcedo::editor_rhi::EditorBackend::Cuda) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
  } else if (backend == alcedo::editor_rhi::EditorBackend::Metal) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
  } else {
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication::setOrganizationName(QStringLiteral("Alcedo"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("alcedo.app"));
  QCoreApplication::setApplicationName(QStringLiteral("Alcedo Studio Test Host"));

  const auto options = ParseOptions(argc, argv);
  if (!options.has_value()) {
    return 1;
  }

  const auto backend_parse = alcedo::editor_rhi::ParseEditorBackendArgs(argc, argv);
  if (backend_parse.present && !backend_parse.error.empty()) {
    qCritical("Invalid --editor-backend: %s", backend_parse.error.c_str());
    return 1;
  }
  const auto editor_backend = backend_parse.backend.has_value()
                                  ? *backend_parse.backend
                                  : alcedo::editor_rhi::DefaultEditorBackendForPlatform();
  if (!editor_backend.has_value()) {
    qCritical("No editor backend is available for this build.");
    return 1;
  }
  if (!ConfigureGraphicsApi(*editor_backend)) {
    qCritical("Could not configure the requested editor graphics API.");
    return 1;
  }

  alcedo::TimeProvider::Refresh();
  alcedo::RegisterAllOperators();

  QApplication app(argc, argv);
  QQuickStyle::setStyle("Basic");

  const auto startup = alcedo::editor_rhi::ApplyEditorBackendBeforeWindow(*editor_backend);
  if (!startup.ok) {
    qCritical("Editor backend startup failed (%s): %s",
              alcedo::editor_rhi::ToString(*editor_backend), startup.error.c_str());
    return 1;
  }

  alcedo::ui::AppTheme::RegisterFonts();
  alcedo::ui::LanguageManager language_manager(&app);
  alcedo::ui::AppTheme::SetEffectiveLanguageCode(language_manager.EffectiveLanguageCode());
  alcedo::ui::AppTheme::ApplyApplicationFont(app);

  alcedo::ui::ApplicationModuleHost app_modules;
  app_modules.project()->SetRuntimeAcceleratorPreference(ToAcceleratorPreference(*editor_backend));
  alcedo::ui::RegisterApplicationModuleTypes();
  alcedo::editor_rhi::RegisterEditorViewportQmlTypes();

  QQmlApplicationEngine engine;
  engine.addImportPath(QStringLiteral("qrc:/"));
  language_manager.AttachEngine(&engine);
  app_modules.AttachQmlEngine(&engine);
  engine.rootContext()->setContextProperty("appModules", &app_modules);
  engine.rootContext()->setContextProperty("appTheme", &alcedo::ui::AppTheme::Instance());
  engine.rootContext()->setContextProperty("languageManager", &language_manager);
  engine.rootContext()->setContextProperty("automationMode", true);

  bool qml_failed = false;
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
      [&qml_failed]() {
        qml_failed = true;
        QCoreApplication::exit(1);
      },
      Qt::QueuedConnection);
  engine.loadFromModule("Alcedo.Main", "Main");
  if (qml_failed || engine.rootObjects().isEmpty()) {
    qCritical("The Alcedo.Main QML module did not create a window.");
    return 1;
  }

  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  if (window == nullptr) {
    qCritical("The Alcedo.Main root object is not a QQuickWindow.");
    return 1;
  }
  alcedo::editor_rhi::BindEditorGraphicsToWindow(window, startup);
  window->show();
  window->requestActivate();

  QString       probe_error;
  const QString probe_socket =
      options->probe_socket.isEmpty()
          ? QStringLiteral("alcedo_studio_test_host_%1").arg(QCoreApplication::applicationPid())
          : options->probe_socket;
  alcedo::ui::TestProbe probe(&engine, window, &app);
  if (!probe.Start(probe_socket, &probe_error)) {
    qCritical().noquote() << "Failed to start the probe:" << probe_error;
    return 1;
  }
  std::cout << "PROBE_SOCKET=" << probe_socket.toStdString() << std::endl;

  const auto project_path = ResolveProjectPath(options->project_path);
  const auto import_path  = ResolveProjectPath(options->import_dir);
  if (!project_path.has_value() || !import_path.has_value()) {
    qCritical("Project and import paths must be valid local paths.");
    return 1;
  }

  const auto import_paths = CollectImportPaths(*import_path);
  if (!options->reuse_project && import_paths.empty()) {
    qCritical().noquote() << "No supported RAW files were found under" << options->import_dir;
    return 1;
  }

  bool       project_started    = false;
  bool       import_started     = false;
  bool       finished           = false;

  const auto mark_ready_or_fail = [&]() {
    if (finished) {
      return;
    }
    if (app_modules.import_export()->ImportFailed() > 0) {
      finished = true;
      qCritical("RAW import failed: %d files", app_modules.import_export()->ImportFailed());
      QCoreApplication::exit(1);
      return;
    }
    if (import_started && app_modules.import_export()->ImportCompleted() !=
                              app_modules.import_export()->ImportTotal()) {
      finished = true;
      qCritical("RAW import incomplete: %d of %d files",
                app_modules.import_export()->ImportCompleted(),
                app_modules.import_export()->ImportTotal());
      QCoreApplication::exit(1);
      return;
    }
    finished = true;
    probe.MarkReady();
  };

  QObject::connect(app_modules.import_export(),
                   &alcedo::ui::ImportExportHandler::ImportStateChanged, &app, [&]() {
                     if (import_started && !app_modules.import_export()->ImportRunning()) {
                       mark_ready_or_fail();
                     }
                   });

  std::function<void()> begin_after_project_ready;
  begin_after_project_ready = [&]() {
    if (!project_started || finished) {
      return;
    }
    if (app_modules.project()->ProjectLoading()) {
      QTimer::singleShot(100, &app, begin_after_project_ready);
      return;
    }
    if (!app_modules.project()->ServiceReady()) {
      qCritical().noquote() << "Project initialization failed:"
                            << app_modules.project()->ServiceMessage();
      finished = true;
      QCoreApplication::exit(1);
      return;
    }
    if (options->reuse_project) {
      mark_ready_or_fail();
      return;
    }
    import_started = true;
    app_modules.import_export()->StartImportPaths(import_paths);
  };

  QObject::connect(app_modules.project(), &alcedo::ui::ProjectModule::ProjectChanged, &app,
                   [&]() { QTimer::singleShot(0, &app, begin_after_project_ready); });

  project_started = true;
  if (!StartProject(app_modules, *project_path)) {
    qCritical().noquote() << "Could not start project:" << options->project_path;
    return 1;
  }
  QTimer::singleShot(0, &app, begin_after_project_ready);

  QTimer::singleShot(120000, &app, [&]() {
    if (!finished) {
      qCritical("Timed out while preparing the automation project.");
      finished = true;
      QCoreApplication::exit(1);
    }
  });

  return app.exec();
}
