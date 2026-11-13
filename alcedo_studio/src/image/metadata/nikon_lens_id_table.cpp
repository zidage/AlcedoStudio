//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "nikon_lens_id_table.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "json.hpp"
#include "metadata_values.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace alcedo::metadata_detail {
namespace {
// ---------------------------------------------------------------------------
//  Nikon lens ID lookup (moved from raw_processor.cpp)
// ---------------------------------------------------------------------------
struct NikonLensIdLookup {
  std::unordered_map<std::string, std::string> hex_id_map;
  std::unordered_map<uint64_t, std::string>    numeric_id_map;
  bool                                         valid = false;
};

auto NormalizeHexIdKey(const std::string& key) -> std::string {
  std::istringstream iss(key);
  std::string        token;
  std::string        out;
  bool               first = true;
  while (iss >> token) {
    if (token.size() == 1) {
      token = "0" + token;
    }
    if (token.size() > 2) {
      return {};
    }
    for (char& c : token) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (!first) {
      out.push_back(' ');
    }
    out += token;
    first = false;
  }
  return out;
}

auto UInt64ToHexIdKey(uint64_t value, bool little_endian) -> std::string {
  char buffer[3 * 8] = {};
  int  offset        = 0;
  for (int i = 0; i < 8; ++i) {
    const int  index = little_endian ? i : (7 - i);
    const auto byte  = static_cast<unsigned>((value >> (index * 8)) & 0xFFULL);
    std::snprintf(buffer + offset, sizeof(buffer) - static_cast<size_t>(offset),
                  (i == 0) ? "%02X" : " %02X", byte);
    offset += (i == 0) ? 2 : 3;
  }
  return std::string(buffer);
}

auto GetExecutableDir() -> std::filesystem::path {
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD copied =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0) {
      return {};
    }
    if (copied < buffer.size()) {
      buffer.resize(copied);
      return std::filesystem::path(buffer).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) {
    return {};
  }
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  return std::filesystem::path(buffer.c_str()).parent_path();
#else
  return {};
#endif
}

/**
 * @brief List the Nikon lens id map files to try, most authoritative first.
 *
 * Installed builds ship `config/nikon_lens/id_map.json` next to the executable. When that
 * packaged directory exists it is the only candidate, so a missing packaged file is not
 * masked by the build machine's source checkout. Build trees fall back to the source paths.
 */
auto NikonLensIdMapCandidates() -> std::vector<std::filesystem::path> {
  const auto exe_dir = GetExecutableDir();
  if (!exe_dir.empty()) {
    const auto      packaged_dir = exe_dir / "config" / "nikon_lens";
    std::error_code ec;
    if (std::filesystem::is_directory(packaged_dir, ec) && !ec) {
      return {packaged_dir / "id_map.json"};
    }
  }

  std::vector<std::filesystem::path> candidates;
#ifdef CONFIG_PATH
  candidates.emplace_back(std::filesystem::path(CONFIG_PATH) / "nikon_lens" / "id_map.json");
#endif
  candidates.emplace_back(std::filesystem::path("src/config/nikon_lens/id_map.json"));
  candidates.emplace_back(std::filesystem::path("alcedo/src/config/nikon_lens/id_map.json"));
  return candidates;
}

auto LoadNikonLensIdLookup() -> NikonLensIdLookup {
  NikonLensIdLookup db;
  const auto        candidates = NikonLensIdMapCandidates();

  for (const auto& path : candidates) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
      continue;
    }

    try {
      std::ifstream ifs(path, std::ios::binary);
      if (!ifs.is_open()) {
        continue;
      }
      nlohmann::json payload;
      ifs >> payload;

      if (payload.contains("hex_id_map") && payload["hex_id_map"].is_object()) {
        for (auto it = payload["hex_id_map"].begin(); it != payload["hex_id_map"].end(); ++it) {
          if (!it.value().is_string()) {
            continue;
          }
          const std::string key = NormalizeHexIdKey(it.key());
          if (key.empty()) {
            continue;
          }
          db.hex_id_map[key] = it.value().get<std::string>();
        }
      }

      if (payload.contains("numeric_id_map") && payload["numeric_id_map"].is_object()) {
        for (auto it = payload["numeric_id_map"].begin(); it != payload["numeric_id_map"].end();
             ++it) {
          if (!it.value().is_string()) {
            continue;
          }
          try {
            const uint64_t numeric_key     = std::stoull(it.key());
            db.numeric_id_map[numeric_key] = it.value().get<std::string>();
          } catch (...) {
            continue;
          }
        }
      }

      db.valid = !db.hex_id_map.empty() || !db.numeric_id_map.empty();
      if (db.valid) {
        return db;
      }
    } catch (...) {
      continue;
    }
  }

  return db;
}

auto GetNikonLensIdLookup() -> const NikonLensIdLookup& {
  static const NikonLensIdLookup db = LoadNikonLensIdLookup();
  return db;
}

auto LookupNikonLensModelById(const libraw_lensinfo_t& lens) -> std::string {
  const auto& db = GetNikonLensIdLookup();
  if (!db.valid) {
    return {};
  }

  const auto id = static_cast<uint64_t>(lens.makernotes.LensID);
  if (id == 0) {
    return {};
  }

  // LibRaw stores the 16-bit Z-mount lens ID in makernotes.LensID for a native Z lens, and the
  // 8-byte F-mount composite ID otherwise. The two ID spaces overlap: F-mount LensIDNumber 1 is
  // the AF Nikkor 50mm f/1.8, and Z-mount ID 1 is the Nikkor Z 24-70mm f/4 S.
  if (lens.makernotes.LensMount == LIBRAW_MOUNT_Nikon_Z) {
    const auto it = db.numeric_id_map.find(id);
    if (it != db.numeric_id_map.end() && !it->second.empty()) {
      return it->second;
    }
    return {};
  }

  const std::string key_be = UInt64ToHexIdKey(id, false);
  auto              it     = db.hex_id_map.find(key_be);
  if (it != db.hex_id_map.end() && !it->second.empty()) {
    return it->second;
  }

  const std::string key_le = UInt64ToHexIdKey(id, true);
  it                       = db.hex_id_map.find(key_le);
  if (it != db.hex_id_map.end() && !it->second.empty()) {
    return it->second;
  }

  return {};
}
}  // namespace

auto IsNikonCamera(const std::string& make, const std::string& model) -> bool {
  return ContainsCaseInsensitive(make, "nikon") || ContainsCaseInsensitive(model, "nikon");
}

auto ResolveNikonLensModel(const libraw_lensinfo_t& lens) -> std::string {
  std::string candidate = TrimTrailingZeroPadded(lens.makernotes.Lens);
  if (!candidate.empty()) {
    return candidate;
  }

  std::string mapped = LookupNikonLensModelById(lens);
  if (!mapped.empty()) {
    return mapped;
  }

  const auto& nikon = lens.nikon;
  const bool  has_nikon_signature =
      (nikon.LensIDNumber != 0 || nikon.LensType != 0 || nikon.MCUVersion != 0 ||
       nikon.LensFStops != 0 || IsFinitePositive(nikon.EffectiveMaxAp));
  if (!has_nikon_signature) {
    return {};
  }

  char model_buf[192] = {};
  std::snprintf(model_buf, sizeof(model_buf),
                "Nikon LensID %u (type=0x%02X mcu=%u fStops=%u effMaxAp=%.2f)",
                static_cast<unsigned>(nikon.LensIDNumber), static_cast<unsigned>(nikon.LensType),
                static_cast<unsigned>(nikon.MCUVersion), static_cast<unsigned>(nikon.LensFStops),
                static_cast<double>(nikon.EffectiveMaxAp));

  std::string model = model_buf;
  if (IsFinitePositive(lens.MinFocal) && IsFinitePositive(lens.MaxFocal)) {
    char focal_buf[64] = {};
    if (std::fabs(lens.MinFocal - lens.MaxFocal) < 1e-4f) {
      std::snprintf(focal_buf, sizeof(focal_buf), " %.1fmm", static_cast<double>(lens.MinFocal));
    } else {
      std::snprintf(focal_buf, sizeof(focal_buf), " %.1f-%.1fmm",
                    static_cast<double>(lens.MinFocal), static_cast<double>(lens.MaxFocal));
    }
    model += focal_buf;
  }
  return model;
}

}  // namespace alcedo::metadata_detail
