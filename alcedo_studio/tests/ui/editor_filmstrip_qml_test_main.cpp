//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <gtest/gtest.h>

#include <cstdio>

namespace {

auto PathExists(const QByteArray& path) -> bool {
  return !path.isEmpty() && QFileInfo::exists(QString::fromUtf8(path));
}

void ConfigureHeadlessQtPluginPaths(const char* argv0) {
  qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));

  QByteArray platforms;
#ifdef ALCEDO_QT_PLUGIN_PATH
  platforms = QByteArray(ALCEDO_QT_PLUGIN_PATH);
#endif
  if (!PathExists(platforms) && argv0 != nullptr) {
    const QString exe_platforms =
        QFileInfo(QString::fromLocal8Bit(argv0)).absolutePath() +
        QStringLiteral("/plugins/platforms");
    platforms = QDir::toNativeSeparators(exe_platforms).toUtf8();
  }
  if (!PathExists(platforms)) {
    platforms =
        QLibraryInfo::path(QLibraryInfo::PluginsPath).toUtf8() + "/platforms";
  }
  if (!PathExists(platforms)) {
    std::fprintf(stderr, "UI test: Qt platforms dir not found.\n");
  } else {
    qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", platforms);
    const QByteArray plugins_root =
        QFileInfo(QString::fromUtf8(platforms)).dir().absolutePath().toUtf8();
    if (PathExists(plugins_root)) {
      qputenv("QT_PLUGIN_PATH", plugins_root);
    }
  }

#ifdef ALCEDO_QT_QML_IMPORT_PATH
  qputenv("QML2_IMPORT_PATH", QByteArray(ALCEDO_QT_QML_IMPORT_PATH));
#else
  qputenv("QML2_IMPORT_PATH",
          QLibraryInfo::path(QLibraryInfo::QmlImportsPath).toUtf8());
#endif
}

}  // namespace

int main(int argc, char** argv) {
  ConfigureHeadlessQtPluginPaths(argc > 0 ? argv[0] : nullptr);
  QApplication app(argc, argv);

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
