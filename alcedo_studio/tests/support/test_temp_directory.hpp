//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Per-test temporary directory (#809). ctest runs each test in its own process, and `ctest -j`
// runs several of them at the same time. A database or project file at a fixed name in the
// system temporary directory is then written by several tests at once. Each test puts its files
// in its own directory instead:
//
//   <system temp>/alcedo_tests/<test suite>.<test name>_<process id>
//
// The directory is created empty by the constructor and removed with its contents by the
// destructor.

#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace alcedo::test {

/// Id of the current process.
inline auto CurrentProcessId() -> std::int64_t {
#ifdef _WIN32
  return static_cast<std::int64_t>(_getpid());
#else
  return static_cast<std::int64_t>(getpid());
#endif
}

/// @p text with each character other than an ASCII letter, a digit, `.`, `-`, and `_`
/// replaced by `_` (a parameterized test name contains `/`).
inline auto FileNameSafeText(std::string_view text) -> std::string {
  std::string safe;
  safe.reserve(text.size());
  for (const char c : text) {
    const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '-' || c == '_';
    safe.push_back(keep ? c : '_');
  }
  return safe;
}

/**
 * @brief Name of the temporary directory of the running test: `<suite>.<test>_<pid>`.
 *
 * @details Outside a test body (SetUpTestSuite), the name is `<suite>_<pid>`. The suite and
 * test parts together are at most 120 characters, so the paths stay below the Windows path
 * limit.
 */
inline auto CurrentTestTempDirectoryName() -> std::string {
  constexpr std::size_t kMaxNameLength = 120;
  const auto*           unit           = ::testing::UnitTest::GetInstance();
  std::string           name;
  if (const auto* info = unit->current_test_info()) {
    name = std::format("{}.{}", info->test_suite_name(), info->name());
  } else if (const auto* suite = unit->current_test_suite()) {
    name = suite->name();
  } else {
    name = "no_test";
  }
  name = FileNameSafeText(name);
  if (name.size() > kMaxNameLength) {
    name.resize(kMaxNameLength);
  }
  return std::format("{}_{}", name, CurrentProcessId());
}

/// Root of all per-test temporary directories: `<system temp>/alcedo_tests`.
inline auto TestTempRoot() -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / "alcedo_tests";
}

/// An empty directory of the running test, removed with its contents on destruction.
class TestTempDirectory {
 public:
  /// The directory of the running test (see CurrentTestTempDirectoryName).
  TestTempDirectory() : TestTempDirectory(std::string_view{}) {}

  /// A directory of the running test with @p label appended to its name, for a test that
  /// needs more than one directory with separate lifetimes.
  explicit TestTempDirectory(std::string_view label)
      : path_(TestTempRoot() / (label.empty() ? CurrentTestTempDirectoryName()
                                              : std::format("{}_{}", CurrentTestTempDirectoryName(),
                                                            FileNameSafeText(label)))) {
    // A directory of an earlier process with the same id holds files of a run that ended
    // without its destructor.
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_);
  }

  ~TestTempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TestTempDirectory(const TestTempDirectory&)                                  = delete;
  auto               operator=(const TestTempDirectory&) -> TestTempDirectory& = delete;

  [[nodiscard]] auto Path() const -> const std::filesystem::path& { return path_; }

  /// Path of @p name inside the directory.
  [[nodiscard]] auto operator/(const std::filesystem::path& name) const -> std::filesystem::path {
    return path_ / name;
  }

 private:
  std::filesystem::path path_;
};

}  // namespace alcedo::test
