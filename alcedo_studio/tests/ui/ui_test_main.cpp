//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file ui_test_main.cpp
/// @brief Custom main() for ApplicationModuleHost UI tests.
///
/// Creates a QCoreApplication before running GoogleTest — required for Qt
/// signal/slot delivery and QSignalSpy in headless (CI) environments.

#include <QCoreApplication>
#include <gtest/gtest.h>

int main(int argc, char** argv) {
  // Qt requires a QCoreApplication for signal delivery & event-loop
  // processing.  QCoreApplication is sufficient for non-GUI tests —
  // swap to QApplication if tests ever need a display.
  QCoreApplication app(argc, argv);

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
