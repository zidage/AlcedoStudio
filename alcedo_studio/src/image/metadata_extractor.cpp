//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "image/metadata_extractor.hpp"

#include <OpenImageIO/imageio.h>
#include <libraw/libraw.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "decoders/libraw_unpack_guard.hpp"
#include "decoders/dng_default_crop.hpp"
#include "edit/operators/basic/camera_matrices.hpp"
#include "json.hpp"
#include "type/supported_file_type.hpp"

namespace alcedo {
namespace {
OIIO_NAMESPACE_USING

auto RationalToFloat(const Exiv2::Rational& value) -> float {
  if (value.second == 0) {
    return 0.0f;
  }
  return static_cast<float>(value.first) / static_cast<float>(value.second);
}

auto IsFinitePositive(float value) -> bool {
  return std::isfinite(value) && value > 0.0f;
}

auto TrimTrailingZeroPadded(const char* s, size_t max_len = 256) -> std::string {
  if (!s) return {};
  size_t len = std::min(std::strlen(s), max_len);
  while (len > 0 && (s[len - 1] == '\0' || std::isspace(static_cast<unsigned char>(s[len - 1])))) {
    --len;
  }
  return {s, len};
}

auto TrimAscii(const std::string& value) -> std::string {
  std::string out = value;
  while (!out.empty() && (out.back() == '\0' || std::isspace(static_cast<unsigned char>(out.back())))) {
    out.pop_back();
  }
  size_t begin = 0;
  while (begin < out.size() &&
         (out[begin] == '\0' || std::isspace(static_cast<unsigned char>(out[begin])))) {
    ++begin;
  }
  if (begin > 0) {
    out.erase(0, begin);
  }
  return out;
}

auto ContainsCaseInsensitive(const std::string& text, const std::string& pattern) -> bool {
  if (text.empty() || pattern.empty()) return false;
  auto to_lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
  std::string lower_text(text.size(), '\0');
  std::string lower_pattern(pattern.size(), '\0');
  std::transform(text.begin(), text.end(), lower_text.begin(), to_lower);
  std::transform(pattern.begin(), pattern.end(), lower_pattern.begin(), to_lower);
  return lower_text.find(lower_pattern) != std::string::npos;
}

auto IsNikonCamera(const std::string& make, const std::string& model) -> bool {
  return ContainsCaseInsensitive(make, "nikon") || ContainsCaseInsensitive(model, "nikon");
}

auto IsDngExtension(const std::filesystem::path& path) -> bool {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == ".dng";
}

auto ResolveCropFactorHint(float focal_mm, float focal_35mm_mm) -> float {
  if (!IsFinitePositive(focal_mm) || !IsFinitePositive(focal_35mm_mm)) return 0.0f;
  return focal_35mm_mm / focal_mm;
}

auto PathToUtf8(const std::filesystem::path& path) -> std::string {
  const auto u8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

auto ToLowerAscii(std::string value) -> std::string {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

auto ContainsAny(const std::string& text, const std::initializer_list<std::string_view>& patterns)
    -> bool {
  for (const auto pattern : patterns) {
    if (text.find(pattern) != std::string::npos) {
      return true;
    }
  }
  return false;
}

auto ExtractDngGeometryMetadataFromFile(const std::filesystem::path& path)
    -> std::optional<dng::Metadata> {
  std::ifstream ifs(path, std::ios::binary | std::ios::ate);
  if (!ifs.is_open()) {
    return std::nullopt;
  }
  const auto size = ifs.tellg();
  if (size <= 0) {
    return std::nullopt;
  }
  std::vector<uint8_t> buffer(static_cast<size_t>(size));
  ifs.seekg(0, std::ios::beg);
  if (!ifs.read(reinterpret_cast<char*>(buffer.data()), size)) {
    return std::nullopt;
  }
  return dng::ExtractMetadata(std::span<const uint8_t>(buffer.data(), buffer.size()));
}

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
  int  offset = 0;
  for (int i = 0; i < 8; ++i) {
    const int  index = little_endian ? i : (7 - i);
    const auto byte  = static_cast<unsigned>((value >> (index * 8)) & 0xFFULL);
    std::snprintf(buffer + offset, sizeof(buffer) - static_cast<size_t>(offset),
                  (i == 0) ? "%02X" : " %02X", byte);
    offset += (i == 0) ? 2 : 3;
  }
  return std::string(buffer);
}

auto LoadNikonLensIdLookup() -> NikonLensIdLookup {
  NikonLensIdLookup db;
  std::vector<std::filesystem::path> candidates;
#ifdef CONFIG_PATH
  candidates.emplace_back(std::filesystem::path(CONFIG_PATH) / "nikon_lens" / "id_map.json");
#endif
  candidates.emplace_back(std::filesystem::path("src/config/nikon_lens/id_map.json"));
  candidates.emplace_back(std::filesystem::path("alcedo/src/config/nikon_lens/id_map.json"));

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
            const uint64_t numeric_key = std::stoull(it.key());
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

  const uint64_t numeric_candidates[] = {
      static_cast<uint64_t>(lens.nikon.LensIDNumber),
      static_cast<uint64_t>(lens.makernotes.LensID),
  };
  for (const uint64_t id : numeric_candidates) {
    if (id == 0) {
      continue;
    }
    const auto it = db.numeric_id_map.find(id);
    if (it != db.numeric_id_map.end() && !it->second.empty()) {
      return it->second;
    }
  }

  const uint64_t hex_candidates[] = {
      static_cast<uint64_t>(lens.makernotes.LensID),
  };
  for (const uint64_t id : hex_candidates) {
    if (id == 0) {
      continue;
    }
    const std::string key_be = UInt64ToHexIdKey(id, false);
    auto it = db.hex_id_map.find(key_be);
    if (it != db.hex_id_map.end() && !it->second.empty()) {
      return it->second;
    }

    const std::string key_le = UInt64ToHexIdKey(id, true);
    it = db.hex_id_map.find(key_le);
    if (it != db.hex_id_map.end() && !it->second.empty()) {
      return it->second;
    }
  }

  return {};
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
  const bool has_nikon_signature = (nikon.LensIDNumber != 0 || nikon.LensType != 0 ||
                                    nikon.MCUVersion != 0 || nikon.LensFStops != 0 ||
                                    IsFinitePositive(nikon.EffectiveMaxAp));
  if (!has_nikon_signature) {
    return {};
  }

  char model_buf[192] = {};
  std::snprintf(model_buf, sizeof(model_buf),
                "Nikon LensID %u (type=0x%02X mcu=%u fStops=%u effMaxAp=%.2f)",
                static_cast<unsigned>(nikon.LensIDNumber),
                static_cast<unsigned>(nikon.LensType),
                static_cast<unsigned>(nikon.MCUVersion),
                static_cast<unsigned>(nikon.LensFStops),
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

// ---------------------------------------------------------------------------
//  Adobe DNG camera colour-matrix database lookup
//  (ported from color_temp_op.cpp – executed once at import time)
// ---------------------------------------------------------------------------

auto NormalizeCameraName(const std::string& input) -> std::string {
  std::string normalized;
  normalized.reserve(input.size() + input.size() / 4);

  bool last_space = true;
  bool last_alpha = false;
  bool last_digit = false;
  for (char ch : input) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch)) {
      const bool is_alpha = std::isalpha(uch) != 0;
      const bool is_digit = std::isdigit(uch) != 0;
      // Insert space at letter↔digit boundary so "Z5" → "z 5", "5D" → "5 d".
      if (!last_space && ((last_alpha && is_digit) || (last_digit && is_alpha))) {
        normalized.push_back(' ');
      }
      normalized.push_back(static_cast<char>(std::tolower(uch)));
      last_space = false;
      last_alpha = is_alpha;
      last_digit = is_digit;
    } else if (!last_space) {
      normalized.push_back(' ');
      last_space = true;
      last_alpha = false;
      last_digit = false;
    }
  }

  while (!normalized.empty() && normalized.back() == ' ') {
    normalized.pop_back();
  }
  return normalized;
}

auto CompactNormalizedCameraName(std::string normalized) -> std::string {
  normalized.erase(std::remove(normalized.begin(), normalized.end(), ' '), normalized.end());
  return normalized;
}

auto CompactCameraName(const std::string& input) -> std::string {
  return CompactNormalizedCameraName(NormalizeCameraName(input));
}

auto TokenizeCameraNameLoose(const std::string& input) -> std::vector<std::string> {
  std::vector<std::string> tokens;
  std::string              token;
  token.reserve(input.size());

  for (char ch : input) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch)) {
      token.push_back(static_cast<char>(std::tolower(uch)));
    } else if (!token.empty()) {
      tokens.push_back(std::move(token));
      token.clear();
    }
  }

  if (!token.empty()) {
    tokens.push_back(std::move(token));
  }
  return tokens;
}

auto IsMeaningfulCameraMatchToken(const std::string& token) -> bool {
  if (token.size() >= 2) {
    return true;
  }
  return !token.empty() && std::isdigit(static_cast<unsigned char>(token.front())) != 0;
}

auto UniqueMeaningfulCameraTokens(const std::vector<std::string>& tokens) -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(tokens.size());
  for (const auto& token : tokens) {
    if (!IsMeaningfulCameraMatchToken(token)) {
      continue;
    }
    if (std::find(out.begin(), out.end(), token) == out.end()) {
      out.push_back(token);
    }
  }
  return out;
}

auto ContainsCameraToken(const std::vector<std::string>& tokens, const std::string& token) -> bool {
  return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
}

auto ContainsCameraTokenFragment(const std::vector<std::string>& tokens, const std::string& fragment)
    -> bool {
  if (fragment.empty()) {
    return false;
  }
  return std::any_of(tokens.begin(), tokens.end(), [&](const std::string& token) {
    return token.find(fragment) != std::string::npos;
  });
}

auto ResolveCameraColorMatrixAlias(const std::string& camera_make,
                                   const std::string& camera_model) -> std::string {
  const auto make_tokens  = TokenizeCameraNameLoose(camera_make);
  const auto model_tokens = TokenizeCameraNameLoose(camera_model);
  const auto model_compact = CompactCameraName(camera_model);

  const bool is_hasselblad =
      ContainsCameraToken(make_tokens, "hasselblad") ||
      ContainsCameraToken(model_tokens, "hasselblad");
  if (!is_hasselblad) {
    return {};
  }

  // Adobe's Hasselblad entries in this database are grouped by sensor generation rather than
  // modern retail model names. Current Hasselblad camera names still expose the sensor tier in
  // the model string (e.g. 39, 50C, 100C), so resolve to the closest Adobe family by that tier.
  const bool has_39 = ContainsCameraToken(model_tokens, "39") ||
                      ContainsCameraTokenFragment(model_tokens, "39") ||
                      model_compact.ends_with("39") ||
                      model_compact.find("39ms") != std::string::npos;
  if (has_39) {
    return NormalizeCameraName("Hasselblad 39-Coated");
  }

  const bool has_50c = ContainsCameraToken(model_tokens, "50c") ||
                       ContainsCameraTokenFragment(model_tokens, "50c") ||
                       model_compact.find("50c") != std::string::npos;
  if (has_50c) {
    return NormalizeCameraName("Hasselblad 50-15-Coated5");
  }

  const bool has_100c = ContainsCameraToken(model_tokens, "100c") ||
                        ContainsCameraTokenFragment(model_tokens, "100c") ||
                        model_compact.find("100c") != std::string::npos;
  if (!has_100c) {
    return {};
  }

  const bool has_x2d = ContainsCameraToken(model_tokens, "x2d") ||
                       ContainsCameraTokenFragment(model_tokens, "x2d") ||
                       model_compact.find("x2d") != std::string::npos;
  if (has_x2d &&
      (ContainsCameraToken(model_tokens, "ii") ||
       ContainsCameraTokenFragment(model_tokens, "x2dii") ||
       model_compact.find("x2dii") != std::string::npos)) {
    return NormalizeCameraName("Hasselblad 100-22-Coated6");
  }
  return NormalizeCameraName("Hasselblad 100-20-Coated6");
}

struct CameraColorMatrixEntry {
  std::string name_;
  std::string compact_name_;
  std::vector<std::string> match_tokens_;
  double      cm1_[9];
  double      cm2_[9];
};

auto CameraColorMatrixDatabaseSorted() -> const std::vector<CameraColorMatrixEntry>& {
  static const std::vector<CameraColorMatrixEntry> index = [] {
    std::vector<CameraColorMatrixEntry> out;
    const size_t count = sizeof(all_camera_matrices) / sizeof(all_camera_matrices[0]);
    out.reserve(count);

    for (size_t i = 0; i < count; ++i) {
      const auto& item = all_camera_matrices[i];
      auto        key  = NormalizeCameraName(item.camera_name_);
      if (key.empty()) {
        continue;
      }
      CameraColorMatrixEntry entry;
      entry.name_         = std::move(key);
      entry.compact_name_ = CompactNormalizedCameraName(entry.name_);
      entry.match_tokens_ = UniqueMeaningfulCameraTokens(TokenizeCameraNameLoose(item.camera_name_));
      std::memcpy(entry.cm1_, item.color_matrix_1_, sizeof(entry.cm1_));
      std::memcpy(entry.cm2_, item.color_matrix_2_, sizeof(entry.cm2_));
      out.push_back(std::move(entry));
    }

    // Normalization can change ordering (e.g. "7D" -> "7 d"), so sort before binary search.
    std::sort(out.begin(), out.end(),
              [](const CameraColorMatrixEntry& a, const CameraColorMatrixEntry& b) {
                return a.name_ < b.name_;
              });

    // Deduplicate normalized keys while keeping the first matrix entry.
    out.erase(std::unique(out.begin(), out.end(),
                          [](const CameraColorMatrixEntry& a, const CameraColorMatrixEntry& b) {
                            return a.name_ == b.name_;
                          }),
              out.end());
    return out;
  }();
  return index;
}

auto FindInSortedColorMatrixDB(const std::vector<CameraColorMatrixEntry>& db,
                               const std::string& key) -> const CameraColorMatrixEntry* {
  if (key.empty()) {
    return nullptr;
  }
  auto it = std::lower_bound(
      db.begin(), db.end(), key,
      [](const CameraColorMatrixEntry& entry, const std::string& k) {
        return entry.name_ < k;
      });
  if (it != db.end() && it->name_ == key) {
    return &(*it);
  }
  return nullptr;
}

struct CameraMatrixCandidateScore {
  int total_score  = 0;
  int model_signal = 0;
};

auto ScoreApproximateColorMatrixCandidate(const CameraColorMatrixEntry& entry,
                                          const std::string& make_key,
                                          const std::string& make_compact,
                                          const std::string& full_compact,
                                          const std::string& model_compact,
                                          const std::vector<std::string>& model_tokens)
    -> CameraMatrixCandidateScore {
  CameraMatrixCandidateScore score;

  if (!make_key.empty()) {
    if (entry.name_ == make_key || entry.name_.starts_with(make_key + " ")) {
      score.total_score += 1200;
    } else if (!make_compact.empty() &&
               entry.compact_name_.find(make_compact) != std::string::npos) {
      score.total_score += 700;
    }
  }

  if (!full_compact.empty()) {
    if (entry.compact_name_ == full_compact) {
      score.model_signal += 900;
    } else if (full_compact.size() >= 6 &&
               (entry.compact_name_.find(full_compact) != std::string::npos ||
                full_compact.find(entry.compact_name_) != std::string::npos)) {
      score.model_signal += 260;
    }
  }

  if (!model_compact.empty()) {
    if (entry.compact_name_ == model_compact ||
        entry.compact_name_.ends_with(model_compact)) {
      score.model_signal += 700;
    } else if (model_compact.size() >= 4 &&
               (entry.compact_name_.find(model_compact) != std::string::npos ||
                model_compact.find(entry.compact_name_) != std::string::npos)) {
      score.model_signal += 260;
    }
  }

  for (const auto& token : model_tokens) {
    if (ContainsCameraToken(entry.match_tokens_, token)) {
      score.model_signal += 120;
      continue;
    }
    if (token.size() < 3) {
      continue;
    }
    const bool partial_match = std::any_of(
        entry.match_tokens_.begin(), entry.match_tokens_.end(),
        [&](const std::string& candidate_token) {
          return candidate_token.find(token) != std::string::npos ||
                 token.find(candidate_token) != std::string::npos;
        });
    if (partial_match) {
      score.model_signal += 40;
    }
  }

  score.total_score += score.model_signal;
  score.total_score -=
      static_cast<int>(std::abs(static_cast<int>(entry.match_tokens_.size()) -
                                static_cast<int>(model_tokens.size()))) *
      15;
  return score;
}

auto FindApproximateColorMatrixMatch(const std::vector<CameraColorMatrixEntry>& db,
                                     const std::string& camera_make,
                                     const std::string& camera_model)
    -> const CameraColorMatrixEntry* {
  const auto make_key     = NormalizeCameraName(camera_make);
  const auto make_compact = CompactNormalizedCameraName(make_key);
  const auto full_compact = CompactCameraName(camera_make + " " + camera_model);
  const auto model_compact = CompactCameraName(camera_model);
  auto       model_tokens  = UniqueMeaningfulCameraTokens(TokenizeCameraNameLoose(camera_model));
  if (model_tokens.empty()) {
    model_tokens = UniqueMeaningfulCameraTokens(TokenizeCameraNameLoose(camera_make + " " +
                                                                        camera_model));
  }
  if (model_tokens.empty() && model_compact.empty()) {
    return nullptr;
  }

  const CameraColorMatrixEntry* best          = nullptr;
  int                           best_score    = -1;
  int                           runner_up     = -1;
  for (const auto& entry : db) {
    const auto score = ScoreApproximateColorMatrixCandidate(
        entry, make_key, make_compact, full_compact, model_compact, model_tokens);
    if (score.model_signal < 120 || score.total_score < 650) {
      continue;
    }
    if (score.total_score > best_score) {
      runner_up  = best_score;
      best_score = score.total_score;
      best       = &entry;
    } else if (score.total_score > runner_up) {
      runner_up = score.total_score;
    }
  }

  if (!best) {
    return nullptr;
  }
  if (runner_up >= 0 && best_score < runner_up + 140) {
    return nullptr;
  }
  return best;
}

/// Look up Adobe DNG colour matrices for a camera make/model pair and store
/// the result directly into the provided double[9] arrays.
auto LookupCameraColorMatrices(const std::string& camera_make,
                               const std::string& camera_model,
                               double cm1_out[9], double cm2_out[9]) -> bool {
  const auto full_key  = NormalizeCameraName(camera_make + " " + camera_model);
  const auto model_key = NormalizeCameraName(camera_model);

  const auto& db = CameraColorMatrixDatabaseSorted();

  const CameraColorMatrixEntry* found = FindInSortedColorMatrixDB(db, full_key);
  if (!found) {
    found = FindInSortedColorMatrixDB(db, model_key);
  }
  if (!found && !model_key.empty()) {
    const auto make_key = NormalizeCameraName(camera_make);
    if (!make_key.empty() && model_key.starts_with(make_key + " ")) {
      found = FindInSortedColorMatrixDB(db, model_key);
    }
  }
  if (!found) {
    const auto alias_key = ResolveCameraColorMatrixAlias(camera_make, camera_model);
    if (!alias_key.empty()) {
      found = FindInSortedColorMatrixDB(db, alias_key);
    }
  }
  if (!found) {
    found = FindApproximateColorMatrixMatch(db, camera_make, camera_model);
  }

  if (found) {
    std::memcpy(cm1_out, found->cm1_, 9 * sizeof(double));
    std::memcpy(cm2_out, found->cm2_, 9 * sizeof(double));
    return true;
  }
  return false;
}

void MarkAdobeCameraMatrixDatabaseIlluminants(RawRuntimeColorContext& ctx) {
  ctx.calibration_illuminants_valid_ = true;
  ctx.color_matrix_1_cct_            = 2856.0;
  ctx.color_matrix_2_cct_            = 6504.0;
}

auto ParseExifNumericToken(std::string token, double& out_value) -> bool {
  token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char ch) {
                return ch == '[' || ch == ']' || ch == '(' || ch == ')' || ch == ',';
              }),
              token.end());
  if (token.empty()) {
    return false;
  }

  const size_t slash_pos = token.find('/');
  if (slash_pos != std::string::npos) {
    const std::string numerator_text   = token.substr(0, slash_pos);
    const std::string denominator_text = token.substr(slash_pos + 1);
    char*             numerator_end    = nullptr;
    char*             denominator_end  = nullptr;
    const double      numerator        = std::strtod(numerator_text.c_str(), &numerator_end);
    const double      denominator      = std::strtod(denominator_text.c_str(), &denominator_end);
    if (numerator_end == numerator_text.c_str() || denominator_end == denominator_text.c_str() ||
        !std::isfinite(numerator) || !std::isfinite(denominator) || std::abs(denominator) < 1e-12) {
      return false;
    }
    out_value = numerator / denominator;
    return std::isfinite(out_value);
  }

  char*        value_end = nullptr;
  const double value     = std::strtod(token.c_str(), &value_end);
  if (value_end == token.c_str() || !std::isfinite(value)) {
    return false;
  }
  out_value = value;
  return true;
}

auto ParseExifNumericList(const std::string& text) -> std::vector<double> {
  std::istringstream     iss(text);
  std::string            token;
  std::vector<double>    values;

  while (iss >> token) {
    double parsed_value = 0.0;
    if (ParseExifNumericToken(token, parsed_value)) {
      values.push_back(parsed_value);
    }
  }

  return values;
}

auto ReadExifStringTag(const Exiv2::ExifData& exif_data, const char* key) -> std::string {
  const auto it = exif_data.findKey(Exiv2::ExifKey(key));
  if (it == exif_data.end()) {
    return {};
  }
  return TrimAscii(it->toString());
}

auto ReadExifNumericArrayTag(const Exiv2::ExifData& exif_data, const char* key, const int count,
                             double* values_out) -> bool {
  if (!values_out || count <= 0) {
    return false;
  }

  const auto it = exif_data.findKey(Exiv2::ExifKey(key));
  if (it == exif_data.end()) {
    return false;
  }

  const auto values = ParseExifNumericList(it->toString());
  if (static_cast<int>(values.size()) < count) {
    return false;
  }

  for (int i = 0; i < count; ++i) {
    values_out[i] = values[static_cast<size_t>(i)];
  }
  return true;
}

auto ReadExifUnsignedIntTag(const Exiv2::ExifData& exif_data, const char* key, uint32_t& out_value)
    -> bool {
  const auto it = exif_data.findKey(Exiv2::ExifKey(key));
  if (it == exif_data.end()) {
    return false;
  }

  try {
    out_value = static_cast<uint32_t>(it->toUint32());
    return true;
  } catch (...) {
    try {
      out_value = static_cast<uint32_t>(it->toInt64());
      return true;
    } catch (...) {
      return false;
    }
  }
}

auto CalibrationIlluminantToCct(const uint32_t illuminant) -> double {
  switch (illuminant) {
    case 1:   // Daylight
    case 21:  // D65
      return 6504.0;
    case 2:   // Fluorescent
      return 4150.0;
    case 3:   // Tungsten
    case 17:  // Standard Light A
      return 2856.0;
    case 4:   // Flash
    case 9:   // Fine weather
      return 5500.0;
    case 10:  // Cloudy
      return 6500.0;
    case 11:  // Shade
      return 7500.0;
    case 12:  // Daylight fluorescent
      return 6430.0;
    case 13:  // Day white fluorescent
      return 5300.0;
    case 14:  // Cool white fluorescent
      return 4230.0;
    case 15:  // White fluorescent
      return 3450.0;
    case 18:  // Standard Light B
      return 4874.0;
    case 19:  // Standard Light C
      return 6774.0;
    case 20:  // D55
      return 5503.0;
    case 22:  // D75
      return 7504.0;
    case 23:  // D50
      return 5003.0;
    case 24:  // ISO studio tungsten
      return 3200.0;
    default:
      return std::numeric_limits<double>::quiet_NaN();
  }
}

auto HasMeaningfulCameraModelToken(const std::string& camera_model) -> bool {
  const auto normalized = NormalizeCameraName(camera_model);
  if (normalized.empty()) {
    return false;
  }

  const auto tokens = UniqueMeaningfulCameraTokens(TokenizeCameraNameLoose(normalized));
  if (!tokens.empty()) {
    return true;
  }

  return normalized.find(' ') != std::string::npos;
}

auto ShouldPreferUniqueCameraModel(const std::string& current_model,
                                   const std::string& exif_model,
                                   const std::string& unique_camera_model) -> bool {
  if (unique_camera_model.empty()) {
    return false;
  }

  if (!HasMeaningfulCameraModelToken(current_model)) {
    return true;
  }

  const std::string current_compact = CompactCameraName(current_model);
  const std::string exif_compact    = CompactCameraName(exif_model);
  const std::string unique_compact  = CompactCameraName(unique_camera_model);

  if (current_compact.empty() || exif_compact.empty() || unique_compact.empty()) {
    return false;
  }

  const bool current_matches_exif =
      current_model == exif_model || current_compact == exif_compact;
  if (!current_matches_exif) {
    return false;
  }

  return unique_compact.find(exif_compact) != std::string::npos ||
         exif_compact.find(unique_compact) != std::string::npos;
}

auto HasEmbeddedDngProfileTables(const Exiv2::ExifData& exif_data) -> bool {
  for (const auto& datum : exif_data) {
    const std::string key = datum.key();
    if (key.find("ProfileHueSatMapDims") != std::string::npos ||
        key.find("ProfileHueSatMapData") != std::string::npos ||
        key.find("ProfileLookTableDims") != std::string::npos ||
        key.find("ProfileLookTableData") != std::string::npos) {
      return true;
    }
  }
  return false;
}

void PopulateDngColorMetadataFromExif(const Exiv2::ExifData& exif_data, RawRuntimeColorContext& ctx) {
  const std::string exif_make         = ReadExifStringTag(exif_data, "Exif.Image.Make");
  const std::string exif_model        = ReadExifStringTag(exif_data, "Exif.Image.Model");
  const std::string unique_camera_model =
      ReadExifStringTag(exif_data, "Exif.Image.UniqueCameraModel");

  if (ctx.camera_make_.empty() && !exif_make.empty()) {
    ctx.camera_make_ = exif_make;
  }
  if (ShouldPreferUniqueCameraModel(ctx.camera_model_, exif_model, unique_camera_model)) {
    ctx.camera_model_ = unique_camera_model;
  } else if (ctx.camera_model_.empty() && !exif_model.empty()) {
    ctx.camera_model_ = exif_model;
  }

  double cm1[9] = {};
  double cm2[9] = {};
  const bool has_cm1 = ReadExifNumericArrayTag(exif_data, "Exif.Image.ColorMatrix1", 9, cm1);
  const bool has_cm2 = ReadExifNumericArrayTag(exif_data, "Exif.Image.ColorMatrix2", 9, cm2);
  if (!has_cm1 && !has_cm2) {
    return;
  }

  if (!has_cm1 && has_cm2) {
    std::memcpy(cm1, cm2, sizeof(cm1));
  } else if (has_cm1 && !has_cm2) {
    std::memcpy(cm2, cm1, sizeof(cm2));
  }

  std::memcpy(ctx.color_matrix_1_, cm1, sizeof(ctx.color_matrix_1_));
  std::memcpy(ctx.color_matrix_2_, cm2, sizeof(ctx.color_matrix_2_));
  ctx.color_matrices_valid_ = true;

  double fm1[9] = {};
  double fm2[9] = {};
  const bool has_fm1 = ReadExifNumericArrayTag(exif_data, "Exif.Image.ForwardMatrix1", 9, fm1);
  const bool has_fm2 = ReadExifNumericArrayTag(exif_data, "Exif.Image.ForwardMatrix2", 9, fm2);
  if ((has_fm1 || has_fm2) && !HasEmbeddedDngProfileTables(exif_data)) {
    if (!has_fm1 && has_fm2) {
      std::memcpy(fm1, fm2, sizeof(fm1));
    } else if (has_fm1 && !has_fm2) {
      std::memcpy(fm2, fm1, sizeof(fm2));
    }

    std::memcpy(ctx.forward_matrix_1_, fm1, sizeof(ctx.forward_matrix_1_));
    std::memcpy(ctx.forward_matrix_2_, fm2, sizeof(ctx.forward_matrix_2_));
    ctx.forward_matrices_valid_ = true;
  }

  double as_shot_neutral[3] = {};
  if (ReadExifNumericArrayTag(exif_data, "Exif.Image.AsShotNeutral", 3, as_shot_neutral)) {
    std::memcpy(ctx.as_shot_neutral_, as_shot_neutral, sizeof(ctx.as_shot_neutral_));
    ctx.as_shot_neutral_valid_ = std::all_of(
        std::begin(ctx.as_shot_neutral_), std::end(ctx.as_shot_neutral_),
        [](const double value) { return std::isfinite(value) && value > 0.0; });
  }

  uint32_t illuminant1 = 0;
  uint32_t illuminant2 = 0;
  const bool has_illuminant1 =
      ReadExifUnsignedIntTag(exif_data, "Exif.Image.CalibrationIlluminant1", illuminant1);
  const bool has_illuminant2 =
      ReadExifUnsignedIntTag(exif_data, "Exif.Image.CalibrationIlluminant2", illuminant2);
  if (has_illuminant1 || has_illuminant2) {
    const double cct1 = has_illuminant1 ? CalibrationIlluminantToCct(illuminant1)
                                        : std::numeric_limits<double>::quiet_NaN();
    const double cct2 = has_illuminant2 ? CalibrationIlluminantToCct(illuminant2)
                                        : std::numeric_limits<double>::quiet_NaN();
    if (std::isfinite(cct1)) {
      ctx.color_matrix_1_cct_ = cct1;
    }
    if (std::isfinite(cct2)) {
      ctx.color_matrix_2_cct_ = cct2;
    }
    ctx.calibration_illuminants_valid_ =
        std::isfinite(ctx.color_matrix_1_cct_) && std::isfinite(ctx.color_matrix_2_cct_) &&
        ctx.color_matrix_1_cct_ > 0.0 && ctx.color_matrix_2_cct_ > 0.0;
  }
}

auto PopulateDisplayDimensionsFromOiio(const image_path_t& image_path,
                                       ExifDisplayMetaData& display) -> bool {
  try {
    auto input = ImageInput::open(PathToUtf8(image_path));
    if (!input) {
      return false;
    }

    const ImageSpec& spec = input->spec();
    const int        width =
        spec.full_width > 0 ? spec.full_width : spec.width;
    const int        height =
        spec.full_height > 0 ? spec.full_height : spec.height;
    if (width <= 0 || height <= 0) {
      return false;
    }

    display.width_  = static_cast<uint32_t>(width);
    display.height_ = static_cast<uint32_t>(height);
    return true;
  } catch (...) {
    return false;
  }
}

auto OiioMetadataSuggestsHdr(const ImageSpec& spec) -> bool {
  const auto inspect = [](std::string text) {
    text = ToLowerAscii(std::move(text));
    return ContainsAny(text, {"ultrahdr", "ultra hdr", "hdrgm", "gainmap", "gain map",
                              "iso 21496", "iso:ts:21496", "rec2100", "rec.2100", "bt2100",
                              "bt.2100", "itur_2100", "itur-2100", "st2084", "st 2084",
                              "smpte2084", "smpte 2084", "hlg"});
  };

  if (inspect(spec.get_string_attribute("oiio:ColorSpace")) ||
      inspect(spec.get_string_attribute("colorspace")) ||
      inspect(spec.get_string_attribute("jpeg:subimages"))) {
    return true;
  }

  for (const auto& param : spec.extra_attribs) {
    const std::string key = param.name().string();
    if (inspect(key) || inspect(param.get_string(0))) {
      return true;
    }
  }
  return false;
}

auto FileSignaturesSuggestHdr(const image_path_t& image_path) -> bool {
  std::ifstream ifs(image_path, std::ios::binary);
  if (!ifs.is_open()) {
    return false;
  }

  constexpr size_t kMaxProbeBytes = 4 * 1024 * 1024;
  std::string      bytes(kMaxProbeBytes, '\0');
  ifs.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<size_t>(std::max<std::streamsize>(0, ifs.gcount())));
  if (bytes.empty()) {
    return false;
  }

  const std::string text = ToLowerAscii(std::move(bytes));
  return ContainsAny(text, {"http://ns.adobe.com/hdr-gain-map", "hdrgm:version",
                            "hdrgm:gainmapmin", "gainmap:version", "ultrahdr",
                            "ultra hdr", "iso 21496", "iso:ts:21496",
                            "urn:iso:std:iso:ts:21496", "gain map"});
}

auto DetectHdrMetadata(const image_path_t& image_path, const Exiv2::Image* exif_image = nullptr)
    -> bool {
  if (exif_image) {
    for (const auto& datum : exif_image->xmpData()) {
      const std::string text = ToLowerAscii(datum.key() + " " + datum.toString());
      if (ContainsAny(text, {"ultrahdr", "ultra hdr", "hdrgm", "gainmap", "gain map",
                             "iso 21496", "iso:ts:21496", "rec2100", "rec.2100", "bt2100",
                             "bt.2100", "st2084", "st 2084", "smpte2084", "smpte 2084",
                             "hlg"})) {
        return true;
      }
    }
  }

  try {
    auto input = ImageInput::open(PathToUtf8(image_path));
    if (input && OiioMetadataSuggestsHdr(input->spec())) {
      return true;
    }
  } catch (...) {
  }

  return FileSignaturesSuggestHdr(image_path);
}

void PopulateDngMetadataHintFromOpenLibRaw(LibRaw& raw_processor, ExifDisplayMetaData& display,
                                           RawRuntimeColorContext& ctx) {
  const std::string raw_make       = TrimAscii(raw_processor.imgdata.idata.make);
  const std::string raw_model      = TrimAscii(raw_processor.imgdata.idata.model);
  const std::string raw_lens_make  = TrimTrailingZeroPadded(raw_processor.imgdata.lens.LensMake);
  std::string       raw_lens_model = TrimTrailingZeroPadded(raw_processor.imgdata.lens.Lens);
  if (raw_lens_model.empty()) {
    raw_lens_model = TrimTrailingZeroPadded(raw_processor.imgdata.lens.makernotes.Lens);
  }

  if (!raw_make.empty()) {
    ctx.camera_make_ = raw_make;
  }
  if (!raw_model.empty()) {
    ctx.camera_model_ = raw_model;
  }
  if (!raw_lens_make.empty()) {
    ctx.lens_make_ = raw_lens_make;
  }
  if (!raw_lens_model.empty()) {
    ctx.lens_model_ = raw_lens_model;
  }

  if (!ctx.camera_make_.empty()) {
    display.make_ = ctx.camera_make_;
  }
  if (!ctx.camera_model_.empty()) {
    display.model_ = ctx.camera_model_;
  }
  if (!ctx.lens_make_.empty()) {
    display.lens_make_ = ctx.lens_make_;
  }
  if (!ctx.lens_model_.empty()) {
    display.lens_ = ctx.lens_model_;
  }

  const float focal_length_mm = raw_processor.imgdata.other.focal_len;
  if (IsFinitePositive(focal_length_mm)) {
    ctx.focal_length_mm_ = focal_length_mm;
    display.focal_       = focal_length_mm;
  }

  const float aperture_f_number = raw_processor.imgdata.other.aperture;
  if (IsFinitePositive(aperture_f_number)) {
    ctx.aperture_f_number_ = aperture_f_number;
    display.aperture_      = aperture_f_number;
  }

  const float focal_35mm_mm =
      raw_processor.imgdata.lens.FocalLengthIn35mmFormat > 0
          ? static_cast<float>(raw_processor.imgdata.lens.FocalLengthIn35mmFormat)
          : 0.0f;
  if (IsFinitePositive(focal_35mm_mm)) {
    ctx.focal_35mm_mm_ = focal_35mm_mm;
    display.focal_35mm_ = focal_35mm_mm;
  }

  const auto iso_speed = static_cast<uint64_t>(raw_processor.imgdata.other.iso_speed);
  if (iso_speed > 0) {
    display.iso_ = iso_speed;
  }

  const float shutter_sec = raw_processor.imgdata.other.shutter;
  if (std::isfinite(shutter_sec) && shutter_sec > 0.0f) {
    if (shutter_sec >= 1.0f) {
      display.shutter_speed_ = {static_cast<int>(shutter_sec), 1};
    } else {
      display.shutter_speed_ = {1, static_cast<int>(1.0f / shutter_sec + 0.5f)};
    }
  }

  if (raw_processor.imgdata.sizes.width > 0) {
    display.width_ = static_cast<uint32_t>(raw_processor.imgdata.sizes.width);
  }
  if (raw_processor.imgdata.sizes.height > 0) {
    display.height_ = static_cast<uint32_t>(raw_processor.imgdata.sizes.height);
  }

  const time_t ts = raw_processor.imgdata.other.timestamp;
  if (ts > 0) {
    struct tm t {};
#if defined(_WIN32)
    gmtime_s(&t, &ts);
#else
    gmtime_r(&ts, &t);
#endif
    char buf[64] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    display.date_time_str_ = buf;
  }
}

auto ExtractDngMetadataToImageFast(const image_path_t& image_path, Image& image) -> bool {
  Exiv2::Image::UniquePtr exif_image;
  try {
    exif_image = MetadataExtractor::ExtractEXIF(image_path);
  } catch (...) {
    return false;
  }

  if (!exif_image || exif_image->exifData().empty()) {
    return false;
  }

  ExifDisplayMetaData  display = MetadataExtractor::EXIFToDisplayMetaData(exif_image);
  RawRuntimeColorContext ctx{};

  // LibRaw open_file is cheap and preserves the same camera/model strings
  // the old import path used, without paying the DNG unpack cost.
  auto raw_processor = std::make_unique<LibRaw>();
#if defined(_WIN32)
  const int open_ret = raw_processor->open_file(image_path.wstring().c_str());
#else
  const int open_ret = raw_processor->open_file(image_path.string().c_str());
#endif
  if (open_ret == LIBRAW_SUCCESS) {
    PopulateDngMetadataHintFromOpenLibRaw(*raw_processor, display, ctx);
  }
  raw_processor->recycle();

  MetadataExtractor::MergeMetadataHint(&display, ctx);

  PopulateDngColorMetadataFromExif(exif_image->exifData(), ctx);
  if (!ctx.color_matrices_valid_) {
    ctx.color_matrices_valid_ = LookupCameraColorMatrices(
        ctx.camera_make_, ctx.camera_model_, ctx.color_matrix_1_, ctx.color_matrix_2_);
    if (ctx.color_matrices_valid_) {
      MarkAdobeCameraMatrixDatabaseIlluminants(ctx);
    }
  }

  ctx.crop_factor_hint_ = ResolveCropFactorHint(ctx.focal_length_mm_, ctx.focal_35mm_mm_);
  ctx.lens_metadata_valid_ =
      !ctx.lens_model_.empty() && std::isfinite(ctx.focal_length_mm_) && ctx.focal_length_mm_ > 0.0f;
  if (const auto dng_metadata = ExtractDngGeometryMetadataFromFile(image_path);
      dng_metadata.has_value()) {
    ctx.dng_warp_rectilinear_present_ = dng_metadata->warp_rectilinear.has_value();
  }

  ctx.valid_                  = true;
  ctx.output_in_camera_space_ = true;

  display.make_             = ctx.camera_make_;
  display.model_            = ctx.camera_model_;
  display.lens_make_        = ctx.lens_make_;
  display.lens_             = ctx.lens_model_;
  display.focal_            = ctx.focal_length_mm_;
  display.aperture_         = ctx.aperture_f_number_;
  display.focus_distance_m_ = ctx.focus_distance_m_;
  display.focal_35mm_       = ctx.focal_35mm_mm_;

  if (display.width_ == 0 || display.height_ == 0) {
    // Exiv2 often reports the embedded preview dimensions for DNG. Use OIIO
    // only as a last-resort size probe if LibRaw open_file did not expose it.
    PopulateDisplayDimensionsFromOiio(image_path, display);
  }
  display.is_hdr_ = DetectHdrMetadata(image_path, exif_image.get());

  image.SetExifDisplayMetaData(std::move(display));
  image.SetRawColorContext(std::move(ctx));
  return true;
}

/// Populate a RawRuntimeColorContext directly from libraw's open-but-not-processed state.
/// Only requires open_file / unpack to have been called so that imgdata.rawdata.color,
/// imgdata.idata, imgdata.other, imgdata.lens are populated.
void PopulateMetadataRuntimeContext(LibRaw& raw_processor, RawRuntimeColorContext& ctx) {
  const auto& color = raw_processor.imgdata.rawdata.color;
  for (int i = 0; i < 3; ++i) {
    ctx.cam_mul_[i] = color.cam_mul[i];
    ctx.pre_mul_[i] = color.pre_mul[i];
  }
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      ctx.cam_xyz_[r * 3 + c] = color.cam_xyz[r][c];
      ctx.rgb_cam_[r * 3 + c] = color.rgb_cam[r][c];
    }
  }

  ctx.camera_make_  = TrimAscii(raw_processor.imgdata.idata.make);
  ctx.camera_model_ = TrimAscii(raw_processor.imgdata.idata.model);

  ctx.lens_make_  = TrimTrailingZeroPadded(raw_processor.imgdata.lens.LensMake);
  ctx.lens_model_ = TrimTrailingZeroPadded(raw_processor.imgdata.lens.Lens);
  if (ctx.lens_model_.empty()) {
    ctx.lens_model_ = TrimTrailingZeroPadded(raw_processor.imgdata.lens.makernotes.Lens);
  }

  if (IsNikonCamera(ctx.camera_make_, ctx.camera_model_)) {
    if (ctx.lens_make_.empty()) {
      ctx.lens_make_ = "Nikon";
    }
    if (ctx.lens_model_.empty()) {
      ctx.lens_model_ = ResolveNikonLensModel(raw_processor.imgdata.lens);
    }
  }

  ctx.focal_length_mm_ = raw_processor.imgdata.other.focal_len;
  if (!IsFinitePositive(ctx.focal_length_mm_)) {
    ctx.focal_length_mm_ = raw_processor.imgdata.lens.makernotes.CurFocal;
  }
  ctx.aperture_f_number_ = raw_processor.imgdata.other.aperture;
  if (!IsFinitePositive(ctx.aperture_f_number_)) {
    ctx.aperture_f_number_ = raw_processor.imgdata.lens.makernotes.CurAp;
  }
  ctx.focus_distance_m_ = 0.0f;
  if (std::isfinite(raw_processor.imgdata.lens.makernotes.FocusRangeIndex) &&
      raw_processor.imgdata.lens.makernotes.FocusRangeIndex > 0.0f) {
    ctx.focus_distance_m_ = raw_processor.imgdata.lens.makernotes.FocusRangeIndex;
  }

  ctx.focal_35mm_mm_ = 0.0f;
  if (raw_processor.imgdata.lens.FocalLengthIn35mmFormat > 0) {
    ctx.focal_35mm_mm_ = static_cast<float>(raw_processor.imgdata.lens.FocalLengthIn35mmFormat);
  } else if (raw_processor.imgdata.lens.makernotes.FocalLengthIn35mmFormat > 0) {
    ctx.focal_35mm_mm_ =
        static_cast<float>(raw_processor.imgdata.lens.makernotes.FocalLengthIn35mmFormat);
  }
  ctx.crop_factor_hint_ = ResolveCropFactorHint(ctx.focal_length_mm_, ctx.focal_35mm_mm_);

  ctx.lens_metadata_valid_ = !ctx.lens_model_.empty() && std::isfinite(ctx.focal_length_mm_) &&
                             ctx.focal_length_mm_ > 0.0f;

  // Resolve Adobe DNG colour matrices from the camera-model database once and
  // store them in the context so that downstream operators (ColorTempOp) never
  // need to repeat the lookup.
  ctx.color_matrices_valid_ = LookupCameraColorMatrices(
      ctx.camera_make_, ctx.camera_model_,
      ctx.color_matrix_1_, ctx.color_matrix_2_);
  if (ctx.color_matrices_valid_) {
    MarkAdobeCameraMatrixDatabaseIlluminants(ctx);
  }

  // Mark as valid for downstream consumers (not an actual decode, just metadata)
  ctx.valid_                  = true;
  ctx.output_in_camera_space_ = true;  // no processing done, color data is camera-space
}

/// Populate ExifDisplayMetaData from a RawRuntimeColorContext + libraw other/sizes data.
void PopulateDisplayMetadataFromLibRaw(LibRaw& raw_processor, const RawRuntimeColorContext& ctx,
                                       ExifDisplayMetaData& display) {
  display.make_          = ctx.camera_make_;
  display.model_         = ctx.camera_model_;
  display.lens_          = ctx.lens_model_;
  display.lens_make_     = ctx.lens_make_;
  display.focal_         = ctx.focal_length_mm_;
  display.focal_35mm_    = ctx.focal_35mm_mm_;
  display.aperture_      = ctx.aperture_f_number_;
  display.focus_distance_m_ = ctx.focus_distance_m_;

  display.iso_ = static_cast<uint64_t>(raw_processor.imgdata.other.iso_speed);

  const float shutter_sec = raw_processor.imgdata.other.shutter;
  if (std::isfinite(shutter_sec) && shutter_sec > 0.0f) {
    if (shutter_sec >= 1.0f) {
      display.shutter_speed_ = {static_cast<int>(shutter_sec), 1};
    } else {
      display.shutter_speed_ = {1, static_cast<int>(1.0f / shutter_sec + 0.5f)};
    }
  }

  display.width_  = static_cast<uint32_t>(raw_processor.imgdata.sizes.width);
  display.height_ = static_cast<uint32_t>(raw_processor.imgdata.sizes.height);

  // Timestamp
  const time_t ts = raw_processor.imgdata.other.timestamp;
  if (ts > 0) {
    struct tm t {};
#if defined(_WIN32)
    gmtime_s(&t, &ts);
#else
    gmtime_r(&ts, &t);
#endif
    char buf[64] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    display.date_time_str_ = buf;
  }
}

/// Set of raw file extensions (lowercase).
static const std::unordered_set<std::string> kRawExtensions = {
    ".arw", ".cr2", ".cr3", ".nef", ".dng", ".raw", ".raf", ".3fr", ".rw2", ".fff"};

auto IsRawExtension(const std::filesystem::path& path) -> bool {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return kRawExtensions.count(ext) > 0;
}
}  // namespace

void MetadataExtractor::MergeMetadataHint(const ExifDisplayMetaData* metadata_hint,
                                          RawRuntimeColorContext&    ctx) {
  if (!metadata_hint) {
    return;
  }

  const std::string hint_make      = TrimAscii(metadata_hint->make_);
  const std::string hint_model     = TrimAscii(metadata_hint->model_);
  const std::string hint_lens_make = TrimAscii(metadata_hint->lens_make_);
  const std::string hint_lens      = TrimAscii(metadata_hint->lens_);

  if (ctx.camera_make_.empty() && !hint_make.empty()) {
    ctx.camera_make_ = hint_make;
  }
  if (ctx.camera_model_.empty() && !hint_model.empty()) {
    ctx.camera_model_ = hint_model;
  }
  if (ctx.lens_make_.empty() && !hint_lens_make.empty()) {
    ctx.lens_make_ = hint_lens_make;
  }
  if (ctx.lens_model_.empty() && !hint_lens.empty()) {
    ctx.lens_model_ = hint_lens;
  }

  if (!IsFinitePositive(ctx.focal_length_mm_) && IsFinitePositive(metadata_hint->focal_)) {
    ctx.focal_length_mm_ = metadata_hint->focal_;
  }
  if (!IsFinitePositive(ctx.aperture_f_number_) && IsFinitePositive(metadata_hint->aperture_)) {
    ctx.aperture_f_number_ = metadata_hint->aperture_;
  }
  if (!IsFinitePositive(ctx.focus_distance_m_) &&
      IsFinitePositive(metadata_hint->focus_distance_m_)) {
    ctx.focus_distance_m_ = metadata_hint->focus_distance_m_;
  }
  if (!IsFinitePositive(ctx.focal_35mm_mm_) && IsFinitePositive(metadata_hint->focal_35mm_)) {
    ctx.focal_35mm_mm_ = metadata_hint->focal_35mm_;
  }

  if (!IsFinitePositive(ctx.crop_factor_hint_)) {
    ctx.crop_factor_hint_ = ResolveCropFactorHint(ctx.focal_length_mm_, ctx.focal_35mm_mm_);
  }
}

void MetadataExtractor::PopulateRuntimeContextFromOpenLibRaw(LibRaw&                 raw_processor,
                                                             RawRuntimeColorContext& ctx) {
  PopulateMetadataRuntimeContext(raw_processor, ctx);
}

static void GetDisplayMetadataFromExif(Exiv2::ExifData&     exif_data,
                                       ExifDisplayMetaData& display_metadata) {
  if (exif_data.empty()) {
    return;
  }
  const auto find_key = [&exif_data](const char* key) {
    return exif_data.findKey(Exiv2::ExifKey(key));
  };
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Image.Make")) != exif_data.end()) {
    display_metadata.make_ = exif_data["Exif.Image.Make"].toString();
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Image.Model")) != exif_data.end()) {
    display_metadata.model_ = exif_data["Exif.Image.Model"].toString();
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Photo.LensModel")) != exif_data.end()) {
    display_metadata.lens_ = exif_data["Exif.Photo.LensModel"].toString();
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Photo.LensMake")) != exif_data.end()) {
    display_metadata.lens_make_ = exif_data["Exif.Photo.LensMake"].toString();
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Photo.FNumber")) != exif_data.end()) {
    display_metadata.aperture_ = RationalToFloat(exif_data["Exif.Photo.FNumber"].toRational());
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Photo.FocalLength")) != exif_data.end()) {
    display_metadata.focal_ = RationalToFloat(exif_data["Exif.Photo.FocalLength"].toRational());
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Photo.FocalLengthIn35mmFilm")) != exif_data.end()) {
    display_metadata.focal_35mm_ =
        static_cast<float>(exif_data["Exif.Photo.FocalLengthIn35mmFilm"].toInt64());
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Photo.SubjectDistance")) != exif_data.end()) {
    display_metadata.focus_distance_m_ =
        RationalToFloat(exif_data["Exif.Photo.SubjectDistance"].toRational());
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Photo.ISOSpeedRatings")) != exif_data.end()) {
    display_metadata.iso_ = exif_data["Exif.Photo.ISOSpeedRatings"].toInt64();
  } else if (find_key("Exif.Photo.ISOSpeed") != exif_data.end()) {
    display_metadata.iso_ = exif_data["Exif.Photo.ISOSpeed"].toInt64();
  }
  if (find_key("Exif.Photo.ExposureTime") != exif_data.end()) {
    display_metadata.shutter_speed_ = exif_data["Exif.Photo.ExposureTime"].toRational();
  } else if (find_key("Exif.Photo.ShutterSpeedValue") != exif_data.end()) {
    display_metadata.shutter_speed_ = exif_data["Exif.Photo.ShutterSpeedValue"].toRational();
  } else if (find_key("Exif.Image.ShutterSpeedValue") != exif_data.end()) {
    display_metadata.shutter_speed_ = exif_data["Exif.Image.ShutterSpeedValue"].toRational();
  }
  if (find_key("Exif.Photo.PixelYDimension") != exif_data.end()) {
    display_metadata.height_ = exif_data["Exif.Photo.PixelYDimension"].toUint32();
  } else if (exif_data.findKey(Exiv2::ExifKey("Exif.Image.ImageLength")) != exif_data.end()) {
    display_metadata.height_ = exif_data["Exif.Image.ImageLength"].toUint32();
  }
  if (find_key("Exif.Photo.PixelXDimension") != exif_data.end()) {
    display_metadata.width_ = exif_data["Exif.Photo.PixelXDimension"].toUint32();
  } else if (exif_data.findKey(Exiv2::ExifKey("Exif.Image.ImageWidth")) != exif_data.end()) {
    display_metadata.width_ = exif_data["Exif.Image.ImageWidth"].toUint32();
  }
  if (find_key("Exif.Photo.DateTimeOriginal") != exif_data.end()) {
    display_metadata.date_time_str_ = exif_data["Exif.Photo.DateTimeOriginal"].toString();
  } else if (exif_data.findKey(Exiv2::ExifKey("Exif.Image.DateTime")) != exif_data.end()) {
    display_metadata.date_time_str_ = exif_data["Exif.Image.DateTime"].toString();
  }
  if (display_metadata.date_time_str_.size() >= 10) {
    display_metadata.date_time_str_[4] =
        '-';  // Change from "YYYY:MM:DD HH:MM:SS" to "YYYY-MM-DD HH:MM:SS"
    display_metadata.date_time_str_[7] = '-';
  }
}

auto ReadRatingValue(int64_t rating) -> int {
  if (rating < ExifDisplayMetaData::kMinRating || rating > ExifDisplayMetaData::kMaxRating) {
    return ExifDisplayMetaData::kMinRating;
  }
  return static_cast<int>(rating);
}

auto ReadRatingFromXmp(const Exiv2::XmpData& xmp_data) -> std::optional<int> {
  const auto it = xmp_data.findKey(Exiv2::XmpKey("Xmp.xmp.Rating"));
  if (it == xmp_data.end()) {
    return std::nullopt;
  }
  return ReadRatingValue(it->toInt64());
}

auto ReadRatingFromExif(const Exiv2::ExifData& exif_data) -> std::optional<int> {
  const auto it = exif_data.findKey(Exiv2::ExifKey("Exif.Image.Rating"));
  if (it == exif_data.end()) {
    return std::nullopt;
  }
  return ReadRatingValue(it->toInt64());
}

void PopulateStandardRating(const Exiv2::Image& exif_image,
                            ExifDisplayMetaData& display_metadata) {
  if (const auto xmp_rating = ReadRatingFromXmp(exif_image.xmpData()); xmp_rating.has_value()) {
    display_metadata.rating_ = *xmp_rating;
    return;
  }
  if (const auto exif_rating = ReadRatingFromExif(exif_image.exifData()); exif_rating.has_value()) {
    display_metadata.rating_ = *exif_rating;
  }
}

auto MetadataExtractor::ExtractEXIF(const image_path_t& image_path) -> Exiv2::Image::UniquePtr {
  Exiv2::Image::UniquePtr image = Exiv2::ImageFactory::open(image_path.string());
  image->readMetadata();
  return image;
}

auto MetadataExtractor::ExtractEXIFFromBuffer(const uint8_t* buffer, size_t size)
    -> Exiv2::Image::UniquePtr {
  if (!buffer || size == 0) {
    throw std::runtime_error("MetadataExtractor: empty buffer");
  }
  Exiv2::Image::UniquePtr image =
      Exiv2::ImageFactory::open(reinterpret_cast<const Exiv2::byte*>(buffer), size);
  image->readMetadata();
  return image;
}

auto MetadataExtractor::EXIFToDisplayMetaData(const Exiv2::Image::UniquePtr& exif_data)
    -> ExifDisplayMetaData {
  ExifDisplayMetaData display_metadata;
  if (!exif_data) {
    return display_metadata;
  }
  if (!exif_data->exifData().empty()) {
    GetDisplayMetadataFromExif(exif_data->exifData(), display_metadata);
  }
  PopulateStandardRating(*exif_data, display_metadata);
  return display_metadata;
}

auto MetadataExtractor::BufferToDisplayMetaData(const uint8_t* buffer, size_t size)
    -> ExifDisplayMetaData {
  ExifDisplayMetaData display_metadata;
  try {
    auto exif_data = ExtractEXIFFromBuffer(buffer, size);
    if (!exif_data) {
      return display_metadata;
    }
    if (!exif_data->exifData().empty()) {
      GetDisplayMetadataFromExif(exif_data->exifData(), display_metadata);
    }
    PopulateStandardRating(*exif_data, display_metadata);
  } catch (...) {
    return ExifDisplayMetaData{};
  }
  return display_metadata;
}


auto MetadataExtractor::EXIFToJSON(const Exiv2::Image::UniquePtr& exif_data) -> nlohmann::json {
  // The full EXIF is too large, we only convert the display-friendly metadata to JSON
  nlohmann::json exif_json;
  auto           display_metadata = EXIFToDisplayMetaData(exif_data);
  exif_json                       = display_metadata.ToJson();
  return exif_json;
}

void MetadataExtractor::ExtractEXIF_ToImage(const image_path_t& image_path, Image& image) {
  // For raw files, prefer the libraw-based extraction path which also provides
  // RawRuntimeColorContext for pipeline operators.
  if (IsRawExtension(image_path)) {
    if (ExtractRawMetadata_ToImage(image_path, image)) {
      return;
    }
    // Fall through to Exiv2 if libraw extraction fails.
  }

  auto exif_data = ExtractEXIF(image_path);
  if (!exif_data) {
    return;
  }
  ExifDisplayMetaData display_metadata;
  if (!exif_data->exifData().empty()) {
    GetDisplayMetadataFromExif(exif_data->exifData(), display_metadata);
  }
  PopulateStandardRating(*exif_data, display_metadata);
  display_metadata.is_hdr_ = DetectHdrMetadata(image_path, exif_data.get());
  image.SetExifDisplayMetaData(std::move(display_metadata));
}

auto MetadataExtractor::ExtractRawMetadata_ToImage(const image_path_t& image_path, Image& image)
    -> bool {
  if (IsDngExtension(image_path) && ExtractDngMetadataToImageFast(image_path, image)) {
    return true;
  }

  // LibRaw is large enough to blow worker-thread stacks under ASan if allocated locally.
  auto raw_processor = std::make_unique<LibRaw>();

#if defined(_WIN32)
  int ret = raw_processor->open_file(image_path.wstring().c_str());
#else
  int ret = raw_processor->open_file(image_path.string().c_str());
#endif
  if (ret != LIBRAW_SUCCESS) {
    std::cerr << "MetadataExtractor: libraw open_file failed for '"
              << image_path.string() << "' (error " << ret << ")" << std::endl;
    return false;
  }

  ret = libraw_guard::Unpack(*raw_processor);
  if (ret != LIBRAW_SUCCESS) {
    std::cerr << "MetadataExtractor: libraw unpack failed for '"
              << image_path.string() << "' (error " << ret << ")" << std::endl;
    raw_processor->recycle();
    return false;
  }

  RawRuntimeColorContext ctx{};
  PopulateMetadataRuntimeContext(*raw_processor, ctx);

  if (IsDngExtension(image_path)) {
    if (const auto dng_metadata = ExtractDngGeometryMetadataFromFile(image_path);
        dng_metadata.has_value()) {
      ctx.dng_warp_rectilinear_present_ = dng_metadata->warp_rectilinear.has_value();
    }
    try {
      auto exif_data = ExtractEXIF(image_path);
      if (exif_data && !exif_data->exifData().empty()) {
        PopulateDngColorMetadataFromExif(exif_data->exifData(), ctx);
      }
    } catch (...) {
      // Non-fatal: keep the LibRaw-derived context if Exiv2 cannot read the DNG tags.
    }
  }

  ExifDisplayMetaData display{};
  PopulateDisplayMetadataFromLibRaw(*raw_processor, ctx, display);
  display.is_hdr_ = DetectHdrMetadata(image_path);

  raw_processor->recycle();

  image.SetExifDisplayMetaData(std::move(display));
  image.SetRawColorContext(std::move(ctx));
  return true;
}
}  // namespace alcedo
