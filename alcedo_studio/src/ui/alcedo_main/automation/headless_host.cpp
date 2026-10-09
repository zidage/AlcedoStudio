//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/headless_host.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QEventLoop>
#include <QMetaObject>
#include <QObject>
#include <QSettings>
#include <clocale>
#include <cstdio>
#include <exception>
#include <exiv2/error.hpp>
#include <string_view>
#include <utility>

#include "automation/automation_protocol.hpp"
#include "automation/automation_session_file.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"
#include "ui/alcedo_main/automation/automation_host_commands.hpp"
#include "ui/alcedo_main/automation/automation_server.hpp"
#include "ui/alcedo_main/automation/automation_session_commands.hpp"
#include "ui/alcedo_main/automation/headless_frame_sink.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/diagnostics/app_logging.hpp"

#ifdef HAVE_METAL
#include "metal/metal_context.hpp"
#endif

namespace alcedo::automation {
namespace {

constexpr auto kAcceleratorBackendSettingsKey = "gpu/acceleratorBackend";
constexpr auto kHostMode                      = "headless";

auto           Exit(HeadlessHostExitCode code) -> int { return static_cast<int>(code); }

/// Writes a startup failure to stderr, which the spawning client reads, and to the log.
void           ReportFailure(const QString& message) {
  const QByteArray bytes = QStringLiteral("alcedo_main --headless: %1\n").arg(message).toUtf8();
  std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stderr);
  std::fflush(stderr);
  qCritical().noquote() << "headless.start.failed" << message;
}

auto ParseBackendToken(const QString& token) -> std::optional<editor_rhi::EditorBackend> {
  const QByteArray encoded = token.trimmed().toLower().toUtf8();
  return editor_rhi::ParseEditorBackendToken(
      std::string_view(encoded.constData(), static_cast<std::size_t>(encoded.size())));
}

/// The saved Settings > Acceleration choice, read with the default settings format so that
/// `--settings-dir` applies.
auto ReadConfiguredBackend() -> std::optional<editor_rhi::EditorBackend> {
  const QSettings settings;
  const QString   stored = settings.value(QLatin1String(kAcceleratorBackendSettingsKey)).toString();
  if (stored.trimmed().isEmpty()) {
    return std::nullopt;
  }
  const auto backend = ParseBackendToken(stored);
  if (!backend.has_value() || !editor_rhi::IsBackendAvailableInThisBuild(*backend)) {
    qWarning().noquote() << "Ignoring the saved accelerator backend" << stored;
    return std::nullopt;
  }
  return backend;
}

/// Opens or creates the project and waits until the load finishes. Returns an error text on
/// failure.
auto OpenProject(ui::ProjectModule& project, const HeadlessHostOptions& options) -> QString {
  const bool started =
      options.create_name.isEmpty()
          ? project.LoadProject(options.project_path)
          : project.CreateProjectInFolderNamed(options.create_folder, options.create_name);
  if (!started) {
    return QStringLiteral("the project cannot be opened: %1").arg(project.ServiceMessage());
  }
  if (project.ProjectLoading()) {
    QEventLoop loop;
    QObject::connect(&project, &ui::ProjectModule::ProjectLoadStateChanged, &loop,
                     [&project, &loop]() {
                       if (!project.ProjectLoading()) {
                         loop.quit();
                       }
                     });
    loop.exec();
  }
  if (!project.ServiceReady() || !project.ProjectEntered()) {
    return QStringLiteral("the project cannot be opened: %1").arg(project.ServiceMessage());
  }
  return {};
}

auto ParseViewport(const QString& value, int* width, int* height) -> bool {
  const QStringList parts = value.toLower().split(QLatin1Char('x'));
  if (parts.size() != 2) {
    return false;
  }
  bool      width_ok  = false;
  bool      height_ok = false;
  const int w         = parts[0].toInt(&width_ok);
  const int h         = parts[1].toInt(&height_ok);
  if (!width_ok || !height_ok || w <= 0 || h <= 0) {
    return false;
  }
  *width  = w;
  *height = h;
  return true;
}

/// Runs one session: the module host, the headless sink, the project, the server, and the
/// session file. The host is destroyed before the function returns.
auto RunHostSession(QCoreApplication& app, const HeadlessHostOptions& options,
                    editor_rhi::EditorBackend backend, const QString& session_dir) -> int {
  // The sink outlives the host, whose shutdown waits for the in-flight frame.
  HeadlessFrameSink         frame_sink;
  ui::ApplicationModuleHost host;
  host.project()->SetRuntimeAcceleratorPreference(HeadlessAcceleratorPreference(backend));

  if (!host.editor_session()->BindHeadlessPresentationSink(&frame_sink, options.viewport_width,
                                                           options.viewport_height)) {
    ReportFailure(QStringLiteral("the editor session has no backend for the presentation sink"));
    host.Shutdown();
    return Exit(HeadlessHostExitCode::SessionError);
  }

  if (const QString project_error = OpenProject(*host.project(), options);
      !project_error.isEmpty()) {
    ReportFailure(project_error);
    host.Shutdown();
    return Exit(HeadlessHostExitCode::ProjectLoadError);
  }

  const QString session_file_path = AutomationSessionFilePath(session_dir, options.session_name);
  if (const auto existing = ReadAutomationSessionFile(session_file_path);
      existing.has_value() && existing->pid != QCoreApplication::applicationPid() &&
      IsAutomationProcessRunning(existing->pid)) {
    ReportFailure(QStringLiteral("session '%1' is in use by process %2")
                      .arg(options.session_name)
                      .arg(existing->pid));
    host.Shutdown();
    return Exit(HeadlessHostExitCode::SessionError);
  }

  AutomationCommandRegistry registry;
  AutomationServer          server(registry);
  bool                      session_file_written = false;
  const auto                remove_session_file  = [&]() {
    if (session_file_written) {
      (void)RemoveAutomationSessionFile(session_dir, options.session_name);
      session_file_written = false;
    }
  };

  AutomationHostCommandContext context;
  context.host           = &host;
  context.host_mode      = QLatin1String(kHostMode);
  context.after_shutdown = [&remove_session_file, &app]() {
    remove_session_file();
    // Quit after the event loop writes the response.
    QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
  };
  QString registration_error;
  if (!RegisterAutomationSessionCommands(
          registry,
          AutomationSessionDescription{QLatin1String(kHostMode),
                                       QCoreApplication::applicationVersion()},
          &registration_error) ||
      !RegisterAutomationHostCommands(registry, std::move(context), &registration_error)) {
    ReportFailure(QStringLiteral("command registration failed: %1").arg(registration_error));
    host.Shutdown();
    return Exit(HeadlessHostExitCode::SessionError);
  }

  const QString socket_name = QStringLiteral("alcedo-automation-%1-%2")
                                  .arg(QCoreApplication::applicationPid())
                                  .arg(options.session_name);
  QString server_error;
  if (!server.Listen(socket_name, &server_error)) {
    ReportFailure(server_error);
    host.Shutdown();
    return Exit(HeadlessHostExitCode::SessionError);
  }

  AutomationSessionFile session_file;
  session_file.protocol_version = kAutomationProtocolVersion;
  session_file.session_name     = options.session_name;
  session_file.socket_name      = socket_name;
  session_file.pid              = QCoreApplication::applicationPid();
  session_file.host_mode        = QLatin1String(kHostMode);
  session_file.project_path =
      QString::fromStdWString(host.project()->handler().package_path().generic_wstring());
  session_file.started_at = QDateTime::currentDateTimeUtc();
  QString file_error;
  if (!WriteAutomationSessionFile(session_dir, session_file, &file_error)) {
    ReportFailure(file_error);
    server.Close();
    host.Shutdown();
    return Exit(HeadlessHostExitCode::SessionError);
  }
  session_file_written = true;
  qCInfo(diag::appLog).noquote() << QStringLiteral(
                                        "automation.session.ready name=%1 socket=%2 file=%3")
                                        .arg(options.session_name, socket_name, session_file_path);

  const int exit_code = app.exec();

  server.Close();
  remove_session_file();
  host.Shutdown();
  return exit_code;
}

}  // namespace

auto HeadlessAcceleratorPreference(editor_rhi::EditorBackend backend)
    -> AcceleratorBackendPreference {
  switch (backend) {
    case editor_rhi::EditorBackend::Cuda:
      return AcceleratorBackendPreference::CUDA;
    case editor_rhi::EditorBackend::OpenCl:
      return AcceleratorBackendPreference::OpenCL;
    case editor_rhi::EditorBackend::Metal:
      return AcceleratorBackendPreference::Metal;
  }
  return AcceleratorBackendPreference::CPU;
}

auto StartHeadlessEditorBackend(editor_rhi::EditorBackend backend) -> QString {
  if (!editor_rhi::IsBackendSupportedOnThisPlatform(backend)) {
    return QStringLiteral("editor backend %1 is not supported on this platform")
        .arg(QLatin1String(editor_rhi::ToString(backend)));
  }
  if (!editor_rhi::IsBackendAvailableInThisBuild(backend)) {
    return QStringLiteral("editor backend %1 is not available in this build")
        .arg(QLatin1String(editor_rhi::ToString(backend)));
  }
  try {
    (void)ResolveAcceleratorBackend(HeadlessAcceleratorPreference(backend));
  } catch (const std::exception& exception) {
    return QStringLiteral("editor backend %1 cannot start: %2")
        .arg(QLatin1String(editor_rhi::ToString(backend)), QString::fromUtf8(exception.what()));
  }
#ifdef HAVE_METAL
  if (backend == editor_rhi::EditorBackend::Metal && MetalContext::Instance().Device() == nullptr) {
    return QStringLiteral("editor backend metal cannot start: no Metal device");
  }
#endif
  editor_rhi::SetActiveEditorBackend(backend);
  return {};
}

auto IsHeadlessHostRequested(int argc, char** argv) -> bool {
  for (int index = 1; index < argc; ++index) {
    if (argv[index] != nullptr && std::string_view(argv[index]) == "--headless") {
      return true;
    }
  }
  return false;
}

auto ParseHeadlessHostOptions(const QStringList& arguments, HeadlessHostOptions* options,
                              QString* error) -> bool {
  // --create takes two values, so the options are read by hand.
  for (qsizetype index = 0; index < arguments.size(); ++index) {
    QString    argument = arguments.at(index);
    QString    inline_value;
    const bool has_inline_value =
        argument.startsWith(QLatin1String("--")) && argument.contains(QLatin1Char('='));
    if (has_inline_value) {
      inline_value = argument.section(QLatin1Char('='), 1);
      argument     = argument.section(QLatin1Char('='), 0, 0);
    }
    auto take_value = [&](QString* value) -> bool {
      if (has_inline_value) {
        *value = inline_value;
        return true;
      }
      if (index + 1 >= arguments.size()) {
        *error = QStringLiteral("option %1 needs a value").arg(argument);
        return false;
      }
      *value = arguments.at(++index);
      return true;
    };

    if (argument == QLatin1String("--headless")) {
      continue;
    }
    if (argument == QLatin1String("--project")) {
      if (!take_value(&options->project_path)) return false;
    } else if (argument == QLatin1String("--create")) {
      if (has_inline_value || index + 2 >= arguments.size()) {
        *error = QStringLiteral("--create needs <folder> <name>");
        return false;
      }
      options->create_folder = arguments.at(++index);
      options->create_name   = arguments.at(++index);
    } else if (argument == QLatin1String("--session")) {
      if (!take_value(&options->session_name)) return false;
    } else if (argument == QLatin1String("--session-dir")) {
      if (!take_value(&options->session_dir)) return false;
    } else if (argument == QLatin1String("--settings-dir")) {
      if (!take_value(&options->settings_dir)) return false;
    } else if (argument == QLatin1String("--log-file")) {
      if (!take_value(&options->log_file)) return false;
    } else if (argument == QLatin1String("--editor-backend")) {
      QString value;
      if (!take_value(&value)) return false;
      options->editor_backend = ParseBackendToken(value);
      if (!options->editor_backend.has_value()) {
        *error = QStringLiteral("--editor-backend must be cuda, opencl, or metal");
        return false;
      }
    } else if (argument == QLatin1String("--viewport")) {
      QString value;
      if (!take_value(&value)) return false;
      if (!ParseViewport(value, &options->viewport_width, &options->viewport_height)) {
        *error = QStringLiteral("--viewport must be <width>x<height>, for example 1920x1080");
        return false;
      }
    } else {
      *error = QStringLiteral("unknown option '%1'").arg(arguments.at(index));
      return false;
    }
  }

  if (options->project_path.isEmpty() == options->create_name.isEmpty()) {
    *error = QStringLiteral("give exactly one of --project <path> and --create <folder> <name>");
    return false;
  }
  if (!IsValidAutomationSessionName(options->session_name)) {
    *error = QStringLiteral("invalid session name '%1'").arg(options->session_name);
    return false;
  }
  return true;
}

void ApplyHeadlessSettingsDirectory(const QString& settings_dir) {
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir);
}

auto RunHeadlessHost(int argc, char** argv) -> int {
  QStringList arguments;
  for (int index = 1; index < argc; ++index) {
    arguments.push_back(QString::fromLocal8Bit(argv[index]));
  }
  HeadlessHostOptions options;
  QString             error;
  if (!ParseHeadlessHostOptions(arguments, &options, &error)) {
    ReportFailure(error);
    return Exit(HeadlessHostExitCode::UsageError);
  }
  if (!options.settings_dir.isEmpty()) {
    ApplyHeadlessSettingsDirectory(options.settings_dir);
  }

  TimeProvider::Refresh();
  Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::error);
  QCoreApplication app(argc, argv);
  // The same numeric locale rule as the GUI host: number text always uses a decimal point.
  std::setlocale(LC_NUMERIC, "C");
  const QString log_path = options.log_file.isEmpty()
                               ? diag::InitializeApplicationLogging()
                               : diag::InitializeApplicationLoggingToFile(options.log_file);
  qCInfo(diag::appLog).noquote()
      << QStringLiteral("app.start mode=headless log_path=%1").arg(log_path);

  const auto finish = [](int code) {
    qCInfo(diag::appLog) << "app.exit code=" << code;
    diag::ShutdownApplicationLogging();
    return code;
  };

  std::optional<editor_rhi::EditorBackend> backend = options.editor_backend;
  if (!backend.has_value()) {
    backend = ReadConfiguredBackend();
  }
  if (!backend.has_value()) {
    backend = editor_rhi::DefaultEditorBackendForPlatform();
  }
  if (!backend.has_value()) {
    ReportFailure(QStringLiteral("no editor backend is available for this platform and build"));
    return finish(Exit(HeadlessHostExitCode::BackendError));
  }
  if (const QString backend_error = StartHeadlessEditorBackend(*backend);
      !backend_error.isEmpty()) {
    ReportFailure(backend_error);
    return finish(Exit(HeadlessHostExitCode::BackendError));
  }
  qCInfo(diag::appLog).noquote()
      << QStringLiteral("editor.backend=%1").arg(QLatin1String(editor_rhi::ToString(*backend)));

  const QString session_dir =
      options.session_dir.isEmpty() ? DefaultAutomationSessionDir() : options.session_dir;
  return finish(RunHostSession(app, options, *backend, session_dir));
}

}  // namespace alcedo::automation
