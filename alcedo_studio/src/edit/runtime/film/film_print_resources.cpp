//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/film/film_print_resources.hpp"

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>

#include <cstdint>
#endif

namespace alcedo {
namespace {

auto ExecutableDirectory() -> std::filesystem::path {
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD copied =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0) {
      throw std::runtime_error("Film print: the path of the executable is not available");
    }
    if (copied < buffer.size()) {
      buffer.resize(copied);
      return std::filesystem::path(buffer).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0) {
    throw std::runtime_error("Film print: the path of the executable is not available");
  }
  return std::filesystem::path(buffer.c_str()).parent_path();
#else
  throw std::runtime_error("Film print: this platform has no installed resource directory");
#endif
}

}  // namespace

auto FilmPrintResourceDirectory() -> std::filesystem::path {
  return ExecutableDirectory() / "config" / "film_print";
}

auto SharedFilmPrintTables() -> FilmPrintTables& {
  static std::mutex                       mutex;
  static std::unique_ptr<FilmPrintTables> tables;
  std::lock_guard<std::mutex>             lock(mutex);
  if (tables == nullptr) {
    tables = std::make_unique<FilmPrintTables>(FilmStockBundle::Open(FilmPrintResourceDirectory()));
  }
  return *tables;
}

}  // namespace alcedo
