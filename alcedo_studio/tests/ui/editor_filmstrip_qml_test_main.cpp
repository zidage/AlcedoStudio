//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QApplication>
#include <gtest/gtest.h>

#include "ui/qt_test_plugin_paths.hpp"

int main(int argc, char** argv) {
  alcedo::ui::test::ConfigureQtPluginPaths(argc > 0 ? argv[0] : nullptr, /*force_offscreen=*/true);
  QApplication app(argc, argv);

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
