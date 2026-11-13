//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file ui_test_main.cpp
/// @brief Custom main() for ApplicationModuleHost UI tests.
///
/// Starts the Exiv2 XMP toolkit and creates a QCoreApplication before running GoogleTest — the
/// same process setup as the GUI and headless hosts, required for parallel metadata reads, Qt
/// signal/slot delivery, and QSignalSpy in headless (CI) environments.

#include <QCoreApplication>
#include <cstdio>
#include <exiv2/error.hpp>
#include <gtest/gtest.h>

#include "image/exiv2_xmp_toolkit_scope.hpp"

int main(int argc, char** argv) {
  Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::error);
  // Import and thumbnail workers read XMP in parallel; the toolkit must start before they do,
  // as in the GUI and headless hosts.
  const alcedo::Exiv2XmpToolkitScope xmp_toolkit;
  if (!xmp_toolkit.initialized()) {
    std::fputs("The Exiv2 XMP toolkit cannot be initialized\n", stderr);
    return 1;
  }

  // Qt requires a QCoreApplication for signal delivery & event-loop
  // processing.  QCoreApplication is sufficient for non-GUI tests —
  // swap to QApplication if tests ever need a display.
  QCoreApplication app(argc, argv);

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
