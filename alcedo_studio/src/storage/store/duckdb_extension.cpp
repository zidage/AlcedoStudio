//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/duckdb_extension.hpp"

#include <duckdb.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace alcedo {
namespace {

auto ExecutableDirectory() -> std::filesystem::path {
#ifdef _WIN32
  std::wstring buffer(MAX_PATH, L'\0');
  DWORD        size = 0;
  while (true) {
    size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0) {
      return {};
    }
    if (size < buffer.size() - 1) {
      buffer.resize(size);
      return std::filesystem::path(buffer).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  return std::filesystem::weakly_canonical(std::filesystem::path(buffer.c_str())).parent_path();
#else
  std::string buffer(PATH_MAX, '\0');
  const auto  size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (size <= 0) {
    return {};
  }
  buffer.resize(static_cast<size_t>(size));
  return std::filesystem::path(buffer).parent_path();
#endif
}

auto EnvironmentVariable(const std::string& name) -> std::string {
#ifdef _WIN32
  char*  raw = nullptr;
  size_t len = 0;
  if (_dupenv_s(&raw, &len, name.c_str()) != 0 || raw == nullptr) {
    return {};
  }
  std::string value(raw, len > 0 ? len - 1 : 0);
  std::free(raw);
  return value;
#else
  const char* raw = std::getenv(name.c_str());
  return raw != nullptr ? std::string(raw) : std::string{};
#endif
}

/// Candidate files in load order: the environment override, then the packaged copies.
auto PackagedExtensionCandidates(std::string_view name) -> std::vector<std::filesystem::path> {
  std::string env_name = "ALCEDO_DUCKDB_";
  for (const char ch : name) {
    env_name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  }
  env_name += "_EXTENSION";
  const auto file_name = std::string(name) + ".duckdb_extension";

  std::vector<std::filesystem::path> candidates;
  if (const auto env_path = EnvironmentVariable(env_name); !env_path.empty()) {
    candidates.emplace_back(env_path);
  }
  const auto exe_dir = ExecutableDirectory();
  if (!exe_dir.empty()) {
#ifdef __APPLE__
    candidates.push_back(exe_dir.parent_path() / "Resources" / "duckdb_extensions" / file_name);
#endif
    candidates.push_back(exe_dir / "duckdb_extensions" / file_name);
    candidates.push_back(exe_dir / "extensions" / file_name);
  }
  return candidates;
}

/// Runs one LOAD statement. Returns the DuckDB error, or an empty string on success.
auto TryLoad(duckdb_connection conn, const duckorm::SqlFragment& statement) -> std::string {
  try {
    duckorm::execute(conn, statement);
    return {};
  } catch (const std::runtime_error& e) {
    return e.what();
  }
}

}  // namespace

auto LoadPackagedDuckDbExtension(duckdb_connection conn, std::string_view name,
                                 std::string* error) -> bool {
  namespace expr = duckorm::expr;
  // Best-effort: without this setting DuckDB may try a download on the `LOAD <name>` step.
  (void)TryLoad(conn, expr::raw("SET autoinstall_known_extensions=false;"));

  std::string errors;
  for (const auto& candidate : PackagedExtensionCandidates(name)) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
      continue;
    }
    // LOAD takes no bind parameter, so the path is an escaped literal.
    auto statement = expr::raw("LOAD ");
    statement.append(expr::lit(candidate.generic_string()));
    const auto candidate_error = TryLoad(conn, statement);
    if (candidate_error.empty()) {
      return true;
    }
    errors += "Packaged extension load failed from " + candidate.generic_string() + ": " +
              candidate_error + "\n";
  }

  const auto name_error = TryLoad(conn, expr::raw("LOAD " + std::string(name) + ";"));
  if (name_error.empty()) {
    return true;
  }
  errors += "Extension load failed by name: " + name_error;
  if (error != nullptr) {
    *error = std::move(errors);
  }
  return false;
}

}  // namespace alcedo
