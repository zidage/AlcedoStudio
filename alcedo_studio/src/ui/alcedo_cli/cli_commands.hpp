//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QStringList>

namespace alcedo::cli {

/// Exit codes of `alcedo-cli`.
enum class CliExitCode : int {
  Success               = 0,
  CommandError          = 1,
  SessionSelectionError = 2,
  ConnectionError       = 3,
  UsageError            = 4,
};

/// Runs one `alcedo-cli` invocation. @p arguments excludes the program name. Writes to stdout
/// and stderr and returns the process exit code.
auto RunAlcedoCli(const QStringList& arguments) -> int;

}  // namespace alcedo::cli
