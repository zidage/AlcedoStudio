//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QCoreApplication>
#include <QStringList>

#include "cli_commands.hpp"

int main(int argc, char** argv) {
  // The same names as alcedo_main, so that AppLocalDataLocation resolves to the same directory.
  QCoreApplication::setOrganizationName(QStringLiteral("Alcedo"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("alcedo.app"));
  QCoreApplication::setApplicationName(QStringLiteral("Alcedo"));
  QCoreApplication app(argc, argv);

  QStringList      arguments = QCoreApplication::arguments();
  arguments.removeFirst();
  return alcedo::cli::RunAlcedoCli(arguments);
}
