//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

namespace alcedo {

// NSIS requires /D=<destination> to be the final command-line option. The
// destination must not be surrounded by quotes, even when it contains spaces.
[[nodiscard]] auto BuildSilentNsisArguments(const QString& install_path) -> QString;

}  // namespace alcedo
