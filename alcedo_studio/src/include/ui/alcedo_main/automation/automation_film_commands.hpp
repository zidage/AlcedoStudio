//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>
#include <functional>

#include "edit/runtime/film/film_stock_bundle.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/// Reads the runtime film catalog: the stock bundle of the film print resources. Throws
/// std::runtime_error that names the file when the resources are missing or damaged. The bundle
/// must stay valid for the life of the process.
using FilmStockBundleReader = std::function<const FilmStockBundle&()>;

/// Registers `film.list` with the stock bundle of SharedFilmPrintTables, the tables that the film
/// print render reads from `config/film_print` next to the executable. Returns false and writes
/// @p error when the registration fails.
[[nodiscard]] auto RegisterAutomationFilmCommands(AutomationCommandRegistry& registry,
                                                  QString* error = nullptr) -> bool;

/// Registers `film.list`. It lists the films and print materials of the film print browser
/// (kFilmPresentations) in browser order, with the catalog names of the editor, and the balance
/// light of each negative from the bundle of @p read_bundle. When @p read_bundle throws, the
/// result has the browser fields and the warning `film_resources_unavailable`. Returns false and
/// writes @p error when the registration fails.
[[nodiscard]] auto RegisterAutomationFilmCommands(AutomationCommandRegistry& registry,
                                                  FilmStockBundleReader      read_bundle,
                                                  QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
