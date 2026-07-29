//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QApplication>
#include <gtest/gtest.h>

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
#ifdef ALCEDO_QT_PLUGIN_PATH
  qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", QByteArray(ALCEDO_QT_PLUGIN_PATH));
#endif
#ifdef ALCEDO_QT_QML_IMPORT_PATH
  qputenv("QML2_IMPORT_PATH", QByteArray(ALCEDO_QT_QML_IMPORT_PATH));
#endif
  QApplication app(argc, argv);

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
