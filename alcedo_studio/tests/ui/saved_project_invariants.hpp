//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file saved_project_invariants.hpp
/// @brief Storage invariant check (#805) of the project packages that a UI test saved.

#pragma once

#include <gtest/gtest.h>

#include <QString>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "app/project_package_backend.hpp"
#include "support/storage_invariants.hpp"

namespace alcedo::ui::test {

/**
 * @brief Unpack each project package (.alcd) under @p dir and check the storage invariants of
 *        its database.
 *
 * @details The application saves a package after an import, a delete, and a library change, and
 * the next project open reads the rows of that package. A fixture calls this in TearDown, after
 * the test closed its ApplicationModuleHost. Nothing is checked when the test already failed or
 * was skipped.
 */
inline void ExpectSavedProjectInvariants(const std::filesystem::path& dir) {
  if (::testing::Test::HasFailure() || ::testing::Test::IsSkipped()) {
    return;
  }
  std::error_code                    ec;
  std::vector<std::filesystem::path> packages;
  for (auto it = std::filesystem::recursive_directory_iterator(
           dir, std::filesystem::directory_options::skip_permission_denied, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().extension() == ".alcd") {
      packages.push_back(it->path());
    }
  }
  const auto check_root = dir / "saved_project_invariants";
  for (std::size_t i = 0; i < packages.size(); ++i) {
    const auto workspace = check_root / std::to_string(i);
    std::filesystem::remove_all(workspace, ec);
    std::filesystem::create_directories(workspace, ec);
    std::filesystem::path db_path;
    std::filesystem::path meta_path;
    QString               error;
    if (!project_pack::UnpackProjectToWorkspace(packages[i], workspace,
                                                QStringLiteral("saved_project_invariants"),
                                                &db_path, &meta_path, &error)) {
      ADD_FAILURE() << "Saved project " << packages[i].string()
                    << " does not unpack: " << error.toStdString();
      continue;
    }
    alcedo::test::ExpectStoredStorageInvariants(db_path);
  }
  std::filesystem::remove_all(check_root, ec);
}

}  // namespace alcedo::ui::test
