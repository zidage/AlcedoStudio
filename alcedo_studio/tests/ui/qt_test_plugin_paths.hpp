//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QString>

#include <cstdio>

namespace alcedo::ui::test {

inline auto PathExists(const QByteArray& path) -> bool {
  return !path.isEmpty() && QFileInfo::exists(QString::fromUtf8(path));
}

/// Resolve Qt plugin and QML import roots before QApplication starts.
/// Isolated *_runtime/ dirs only contain applocal DLLs; without QT_PLUGIN_PATH,
/// Material/SVG/QML modules fail and Main.qml leaves rootObjects empty.
///
/// @param argv0  Process argv[0]; used to find <exe>/plugins/platforms.
/// @param force_offscreen  When true, set QT_QPA_PLATFORM=offscreen (widget UI
///        tests). Real GPU editor E2E keeps the caller-chosen platform (windows).
inline void ConfigureQtPluginPaths(const char* argv0, bool force_offscreen = true) {
  if (force_offscreen) {
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
  }

  QByteArray platforms;
#ifdef ALCEDO_QT_PLUGIN_PATH
  platforms = QByteArray(ALCEDO_QT_PLUGIN_PATH);
#endif
  if (!PathExists(platforms) && argv0 != nullptr) {
    const QString exe_platforms = QFileInfo(QString::fromLocal8Bit(argv0)).absolutePath() +
                                  QStringLiteral("/plugins/platforms");
    platforms = QDir::toNativeSeparators(exe_platforms).toUtf8();
  }
  if (!PathExists(platforms)) {
    platforms = QLibraryInfo::path(QLibraryInfo::PluginsPath).toUtf8() + "/platforms";
  }
  if (!PathExists(platforms)) {
    std::fprintf(stderr,
                  "UI test: Qt platforms dir not found "
                  "(ALCEDO_QT_PLUGIN_PATH / <exe>/plugins/platforms / "
                  "QLibraryInfo).\n");
  } else {
    qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", platforms);
    // Parent of platforms/ is the full plugins root (imageformats, iconengines,
    // styles, …). Required for SVG icons used by Material Buttons in Main.qml.
    const QByteArray plugins_root =
        QFileInfo(QString::fromUtf8(platforms)).dir().absolutePath().toUtf8();
    if (PathExists(plugins_root)) {
      qputenv("QT_PLUGIN_PATH", plugins_root);
    }
  }

#ifdef ALCEDO_QT_QML_IMPORT_PATH
  qputenv("QML2_IMPORT_PATH", QByteArray(ALCEDO_QT_QML_IMPORT_PATH));
#else
  qputenv("QML2_IMPORT_PATH", QLibraryInfo::path(QLibraryInfo::QmlImportsPath).toUtf8());
#endif
}

}  // namespace alcedo::ui::test
