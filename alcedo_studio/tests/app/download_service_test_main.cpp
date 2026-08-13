//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <QCoreApplication>

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
