//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <libraw/libraw.h>

#include <string>

namespace alcedo::metadata_detail {

auto IsNikonCamera(const std::string& make, const std::string& model) -> bool;

/// Nikon lens model of an opened file: the maker note lens name, then the name of the lens ID in
/// `config/nikon_lens/id_map.json`, then a text that lists the Nikon lens ID fields.
/// @return Empty when the file has no Nikon lens data.
auto ResolveNikonLensModel(const libraw_lensinfo_t& lens) -> std::string;

}  // namespace alcedo::metadata_detail
