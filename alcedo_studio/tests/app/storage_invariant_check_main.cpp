//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// StorageInvariantCheck <database> (#1191): the storage invariant check (#805,
// tests/support/storage_invariants) on a saved project database, for the file management fuzz
// (scripts/file_fuzz/), which is a Python program and cannot run a gtest fixture.
//
// It prints each broken row to stdout as `<rule>: <row>`, one line per row, and the count to
// stderr. Exit codes: 0 no broken row, 1 one or more broken rows, 2 a usage error or a database
// that cannot be read. It is a test program: it is built with the test targets and is not
// installed.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>

#include "sleeve/storage.hpp"
#include "support/storage_invariants.hpp"

namespace {

constexpr int kNoViolation    = 0;
constexpr int kViolationFound = 1;
constexpr int kUsageError     = 2;

void          WriteLine(std::FILE* stream, const std::string& text) {
  std::fputs(text.c_str(), stream);
  std::fputc('\n', stream);
}

/// UTF-8 text of @p path; path::string throws for a character outside the ANSI code page.
auto PathText(const std::filesystem::path& path) -> std::string {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}

auto RunCheck(const std::filesystem::path& db_path) -> int {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(db_path, ec)) {
    WriteLine(stderr, std::format("StorageInvariantCheck: no database file at {}",
                                  PathText(db_path)));
    return kUsageError;
  }
  try {
    alcedo::Storage storage(db_path);
    const auto      violations = alcedo::test::FindStorageInvariantViolations(storage);
    for (const auto& violation : violations) {
      WriteLine(stdout, std::format("{}: {}", violation.rule, violation.row));
    }
    std::fflush(stdout);
    WriteLine(stderr, std::format("{} storage invariant violation(s) in {}", violations.size(),
                                  PathText(db_path)));
    return violations.empty() ? kNoViolation : kViolationFound;
  } catch (const std::exception& error) {
    WriteLine(stderr, std::format("StorageInvariantCheck: the database {} cannot be read: {}",
                                  PathText(db_path), error.what()));
    return kUsageError;
  }
}

}  // namespace

// wmain on Windows, so a database path outside the ANSI code page reaches the check unchanged.
#ifdef _WIN32
auto wmain(int argc, wchar_t** argv) -> int {
#else
auto main(int argc, char** argv) -> int {
#endif
  if (argc != 2) {
    WriteLine(stderr, "Usage: StorageInvariantCheck <database>");
    return kUsageError;
  }
  return RunCheck(std::filesystem::path(argv[1]));
}
