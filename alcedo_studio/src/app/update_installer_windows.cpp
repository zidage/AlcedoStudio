//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/update_installer_windows.hpp"

#include <QDir>

namespace alcedo {

auto BuildSilentNsisArguments(const QString& install_path) -> QString {
  const QString destination =
      QDir::toNativeSeparators(QDir::cleanPath(QDir(install_path).absolutePath()));
  return QStringLiteral("/S /D=") + destination;
}

}  // namespace alcedo
