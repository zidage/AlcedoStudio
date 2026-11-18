//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Storage invariant check (#805): the rules that the rows of one project database keep after
// any sequence of production writes. A test that builds its data through production paths runs
// the check after its writes; a test that writes broken rows on purpose does not.

#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "sleeve/storage.hpp"

namespace alcedo::test {

/// One row that breaks one storage invariant.
struct StorageInvariantViolation {
  std::string rule;  ///< Name of the broken rule (see FindStorageInvariantViolations).
  std::string row;   ///< Key columns of the row that breaks it.
};

/**
 * @brief Read the database of @p storage and return every row that breaks a storage invariant.
 *
 * @details The rules, by area:
 * - Library: each file element has exactly one image binding, to an existing Image row; each
 *   Image row is bound to a file; each file is listed in the library root folder (id 0); each
 *   folder content row names an existing folder and an existing element.
 * - Edit history: each file has an ImageEditState row (the history root of an image, created
 *   at import); each ImageEditState row belongs to a file and has the PipelineRoot row of its
 *   root id; each Version ref, commit, and attribution row belongs to a stored history; head and
 *   parent commits exist in the same root (storage writes the empty text, not NULL, for a head
 *   at the root and for no parent); CommitGraphStore::LoadGraph reads each history
 *   without an error (it checks the parent rules and the materialized head and chain).
 * - File side tables: pipeline JSON, semantic embedding and label, and AI annotation rows
 *   name an existing file element (file removal deletes them in the same operation).
 * - Photo stacks: members name live Files with image bindings and an existing stack; a stack
 *   has at least two members, unique nonnegative positions, and a cover in its own membership.
 *
 * The read takes the database lock. Throws std::runtime_error when a statement fails.
 */
auto FindStorageInvariantViolations(Storage& storage) -> std::vector<StorageInvariantViolation>;

/// One line per violation, for a test failure message.
auto FormatStorageInvariantViolations(const std::vector<StorageInvariantViolation>& violations)
    -> std::string;

/// Add a test failure that lists every storage invariant violation of @p storage.
inline void ExpectStorageInvariants(Storage& storage) {
  const auto violations = FindStorageInvariantViolations(storage);
  EXPECT_TRUE(violations.empty()) << FormatStorageInvariantViolations(violations);
}

/**
 * @brief Open the project database at @p db_path and add a test failure that lists every
 *        storage invariant violation of its stored rows.
 *
 * @details A fixture calls this in TearDown, after the test closed its project: the check then
 * reads the rows that the production write paths stored, as the next project open reads them.
 * Nothing is checked when the file does not exist (the test did not create a project) or when
 * the test already failed or was skipped.
 */
void ExpectStoredStorageInvariants(const std::filesystem::path& db_path);

}  // namespace alcedo::test
