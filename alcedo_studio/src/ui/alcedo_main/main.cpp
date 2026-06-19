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
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QString>
#include <QtGlobal>

#include <exiv2/error.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "ui/alcedo_main/album_backend/album_backend.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/language_manager.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "utils/diagnostics/app_logging.hpp"
#ifdef HAVE_OPENCL
#include "opencl/opencl_runtime.hpp"
#endif
#include "utils/clock/time_provider.hpp"

#if defined(Q_OS_WIN) && defined(HAVE_OPENCL)
#include <QtGui/qopenglcontext_platform.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#endif

namespace {

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

#if defined(Q_OS_WIN) && defined(HAVE_OPENCL)
class OpenClGlSharingBootstrap {
 public:
  auto Initialize() -> bool {
    if (initialized_) {
      return true;
    }

    context_ = std::make_unique<QOpenGLContext>();
    if (auto* global_share_context = QOpenGLContext::globalShareContext()) {
      context_->setShareContext(global_share_context);
      context_->setFormat(global_share_context->format());
    }
    if (!context_->create()) {
      qWarning("OpenCL/OpenGL bootstrap: failed to create hidden OpenGL context.");
      context_.reset();
      return false;
    }

    surface_ = std::make_unique<QOffscreenSurface>();
    surface_->setFormat(context_->format());
    surface_->create();
    if (!surface_->isValid() || !context_->makeCurrent(surface_.get())) {
      qWarning("OpenCL/OpenGL bootstrap: failed to make hidden OpenGL context current.");
      surface_.reset();
      context_.reset();
      return false;
    }

    auto* native_context = context_->nativeInterface<QNativeInterface::QWGLContext>();
    HGLRC hglrc = native_context ? native_context->nativeContext() : nullptr;
    HDC   hdc   = wglGetCurrentDC();
    if (hglrc == nullptr || hdc == nullptr) {
      qWarning("OpenCL/OpenGL bootstrap: failed to resolve WGL context handles.");
      context_->doneCurrent();
      surface_.reset();
      context_.reset();
      return false;
    }

    alcedo::OpenClInitializationOptions options;
    options.gl_context        = hglrc;
    options.gl_device_context = hdc;
    initialized_ = alcedo::TryInitializeOpenClRuntime(options);
    context_->doneCurrent();

    if (!initialized_) {
      qWarning("OpenCL/OpenGL bootstrap: failed to initialize OpenCL with OpenGL sharing.");
      surface_.reset();
      context_.reset();
    }
    return initialized_;
  }

 private:
  std::unique_ptr<QOpenGLContext>  context_;
  std::unique_ptr<QOffscreenSurface> surface_;
  bool                             initialized_ = false;
};
#endif

}  // namespace

int main(int argc, char* argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#else
  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
      Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
#if defined(Q_OS_WIN) && defined(HAVE_OPENCL)
  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#endif

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
  QQuickStyle::setStyle("Material");

#if defined(Q_OS_WIN) && defined(HAVE_OPENCL)
  OpenClGlSharingBootstrap opencl_gl_bootstrap;
  (void)opencl_gl_bootstrap.Initialize();
#endif

  alcedo::ui::AlbumBackend backend;

  QQmlApplicationEngine engine;
  engine.addImportPath("qrc:/");
  language_manager.AttachEngine(&engine);
  engine.rootContext()->setContextProperty("albumBackend", &backend);
  engine.rootContext()->setContextProperty("appTheme", &alcedo::ui::AppTheme::Instance());
  engine.rootContext()->setContextProperty("languageManager", &language_manager);

  QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                   []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

  engine.loadFromModule("Alcedo.Main", "Main");

  const int exit_code = app.exec();
  qCInfo(alcedo::diag::appLog) << "app.exit code=" << exit_code;
  alcedo::diag::ShutdownApplicationLogging();
  return exit_code;
}
