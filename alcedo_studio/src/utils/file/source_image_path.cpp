// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "utils/file/source_image_path.hpp"

#include <stdexcept>
#include <utility>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace alcedo {
auto NormalizeSourceImagePath(const image_path_t& path) -> std::wstring {
  if (!path.is_absolute()) {
    throw std::invalid_argument("Source image path must be absolute");
  }
  auto value = path.lexically_normal().generic_wstring();
#ifdef _WIN32
  if (!value.empty()) {
    const auto length =
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
                      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
    if (length == 0) throw std::runtime_error("Cannot normalize source path case");
    std::wstring folded(length, L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
                       static_cast<int>(value.size()), folded.data(), length, nullptr, nullptr,
                       0)) {
      throw std::runtime_error("Cannot normalize source path case");
    }
    value = std::move(folded);
  }
#endif
  return value;
}
}  // namespace alcedo
