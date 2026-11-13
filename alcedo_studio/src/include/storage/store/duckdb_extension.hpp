//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <duckdb.h>

#include <string>
#include <string_view>

namespace alcedo {

/**
 * @brief Load a DuckDB extension that ships with the app, for example `vss` or `fts`.
 *
 * Turns off DuckDB's extension autoinstall first, so the offline app never tries to download.
 * Then tries, in this order: the file named by the environment variable
 * `ALCEDO_DUCKDB_<NAME>_EXTENSION` (upper-case name), `<exe dir>/duckdb_extensions/`,
 * `<exe dir>/extensions/` (on macOS first `<exe dir>/../Resources/duckdb_extensions/`), and
 * finally `LOAD <name>` from DuckDB's own extension directory. Stops at the first success.
 * A loaded extension is available to every connection of the database.
 *
 * @param conn Connection to load on. The caller holds the database lock.
 * @param name Extension name (`vss`, `fts`); the packaged file is `<name>.duckdb_extension`.
 * @param error Receives one line for each failed attempt when every attempt failed. May be null.
 * @return true when the extension is loaded.
 */
[[nodiscard]] auto LoadPackagedDuckDbExtension(duckdb_connection conn, std::string_view name,
                                               std::string* error = nullptr) -> bool;

}  // namespace alcedo
