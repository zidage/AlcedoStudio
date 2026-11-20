//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <ctime>
#include <string>

namespace alcedo {

/// `YYYY-MM-DD HH:MM:SS` text of @p value in UTC. The TIMESTAMP columns of the project database
/// (`Element.added_time`, `MountRoot.last_scan_at`, ...) are plain DuckDB TIMESTAMP values that
/// hold UTC by application convention. Calendar arithmetic only, so it does not use the shared
/// std::gmtime buffer.
[[nodiscard]] auto FormatUtcTimestampText(std::time_t value) -> std::string;

/// The instant of `YYYY-MM-DD HH:MM:SS[.ffffff]` UTC text, independent of the process time zone.
/// Text that does not parse reads as 0.
[[nodiscard]] auto ParseUtcTimestampText(const std::string& text) -> std::time_t;

}  // namespace alcedo
