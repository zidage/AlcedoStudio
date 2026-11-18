// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <string>

#include "type/type.hpp"

namespace alcedo {
/// Absolute lexical comparison path. Windows folds case; other platforms preserve case.
/// Performs no filesystem reads or symlink resolution. Throws for relative paths.
auto NormalizeSourceImagePath(const image_path_t& path) -> std::wstring;
}  // namespace alcedo
