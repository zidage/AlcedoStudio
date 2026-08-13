//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <QDir>

#include "app/update_installer_windows.hpp"

namespace alcedo::test {

TEST(UpdateInstallerWindowsTests, SilentInstallKeepsTheExistingDriveAndDirectory) {
  const QString install_path = QStringLiteral("D:/Photo Tools/Alcedo Studio/");

  EXPECT_EQ(BuildSilentNsisArguments(install_path),
            QStringLiteral(R"(/S /D=D:\Photo Tools\Alcedo Studio)"));
}

TEST(UpdateInstallerWindowsTests, DestinationOverrideIsUnquotedAndLast) {
  const QString arguments =
      BuildSilentNsisArguments(QStringLiteral("E:/Portable Apps/Alcedo Studio"));

  EXPECT_TRUE(arguments.startsWith(QStringLiteral("/S ")));
  EXPECT_TRUE(arguments.endsWith(QStringLiteral(R"(/D=E:\Portable Apps\Alcedo Studio)")));
  EXPECT_FALSE(arguments.contains(QLatin1Char('"')));
}

}  // namespace alcedo::test
