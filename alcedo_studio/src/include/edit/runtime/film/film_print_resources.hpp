//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>

#include "edit/runtime/film/film_print_tables.hpp"

namespace alcedo {

/// The directory of the installed resources: `config/film_print` next to the executable.
[[nodiscard]] auto FilmPrintResourceDirectory() -> std::filesystem::path;

/**
 * @brief The host tables of this process, for all render devices and backends.
 *
 * The first successful call opens the bundle from @ref FilmPrintResourceDirectory, which holds
 * about 25 MB until the process ends. Safe to call from any thread.
 *
 * @throws std::runtime_error that names the file when a resource is missing or damaged. Nothing is
 *         kept then, so a later call reads the directory again.
 */
[[nodiscard]] auto SharedFilmPrintTables() -> FilmPrintTables&;

}  // namespace alcedo
