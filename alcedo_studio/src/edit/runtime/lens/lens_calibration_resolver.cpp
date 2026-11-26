//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/lens/lens_calibration_resolver.hpp"

#include <lensfun/lensfun.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "utils/logging/log.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace alcedo {
namespace {

constexpr float kFullFrameDiagonalMm = 43.2666153f;
constexpr float kEpsilon             = 1e-7f;
constexpr float kDefaultFarDistanceM = 1000.0f;

struct LensfunDbDeleter {
  void operator()(lfDatabase* db) const {
    if (db) {
      lf_db_destroy(db);
    }
  }
};

struct LensfunModifierDeleter {
  void operator()(lfModifier* modifier) const {
    if (modifier) {
      lf_modifier_destroy(modifier);
    }
  }
};

/// Process-wide cache of one loaded Lensfun database directory.
struct LensfunDbState {
  std::mutex                                    mutex_;
  std::unique_ptr<lfDatabase, LensfunDbDeleter> db_    = {};
  std::filesystem::path                         root_  = {};
  bool                                          valid_ = false;
};

auto GlobalDbState() -> LensfunDbState& {
  static LensfunDbState state;
  return state;
}

auto IsDir(const std::filesystem::path& path) -> bool {
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec && std::filesystem::is_directory(path, ec) && !ec;
}

auto CanonicalPath(const std::filesystem::path& path) -> std::filesystem::path {
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  const auto      canonical = std::filesystem::weakly_canonical(path, ec);
  if (!ec) {
    return canonical;
  }
  return path.lexically_normal();
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
      return CanonicalPath(std::filesystem::path(buffer).parent_path());
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
  return CanonicalPath(std::filesystem::path(buffer.c_str()).parent_path());
#else
  return {};
#endif
}

auto ResolveDefaultDbPath() -> std::filesystem::path {
  std::vector<std::filesystem::path> candidates;

  const auto                         exe_dir = GetExecutableDir();
  if (!exe_dir.empty()) {
    candidates.emplace_back(exe_dir / "lens_calib");
    candidates.emplace_back(exe_dir / "config" / "lens_calib");
  }

#ifdef CONFIG_PATH
  candidates.emplace_back(std::filesystem::path(CONFIG_PATH) / "lens_calib");
#endif
  candidates.emplace_back(std::filesystem::path("config/lens_calib"));
  candidates.emplace_back(std::filesystem::path("src/config/lens_calib"));
  candidates.emplace_back(std::filesystem::path("alcedo/src/config/lens_calib"));

  for (const auto& candidate : candidates) {
    if (IsDir(candidate)) {
      return CanonicalPath(candidate);
    }
  }
  return {};
}

auto ResolveDbRootPath(const std::filesystem::path& configured_root) -> std::filesystem::path {
  if (configured_root.empty()) {
    return ResolveDefaultDbPath();
  }

  if (configured_root.is_absolute() && IsDir(configured_root)) {
    return CanonicalPath(configured_root);
  }

  std::vector<std::filesystem::path> candidates;
  const auto                         exe_dir = GetExecutableDir();
  if (!configured_root.is_absolute()) {
    if (!exe_dir.empty()) {
      candidates.emplace_back(exe_dir / configured_root);
      candidates.emplace_back(exe_dir / "config" / configured_root);
      candidates.emplace_back(exe_dir / configured_root.filename());
      candidates.emplace_back(exe_dir / "config" / configured_root.filename());
    }
    candidates.emplace_back(configured_root);
  } else {
    candidates.emplace_back(configured_root);
  }

  for (const auto& candidate : candidates) {
    if (IsDir(candidate)) {
      return CanonicalPath(candidate);
    }
  }
  return ResolveDefaultDbPath();
}

auto LoadPortableDbXmls(lfDatabase* db, const std::filesystem::path& root) -> bool {
  if (!db || root.empty() || !std::filesystem::exists(root) ||
      !std::filesystem::is_directory(root)) {
    return false;
  }
  return lf_db_load_path(db, root.string().c_str()) == LF_NO_ERROR;
}

auto GetLensfunDb(const std::filesystem::path& preferred_root) -> lfDatabase* {
  auto&                       state = GlobalDbState();
  std::lock_guard<std::mutex> guard(state.mutex_);

  const std::filesystem::path canonical_root = ResolveDbRootPath(preferred_root);
  if (canonical_root.empty()) {
    state.db_.reset();
    state.root_.clear();
    state.valid_ = false;
    return nullptr;
  }
  if (state.db_ && state.valid_ && canonical_root == state.root_) {
    return state.db_.get();
  }

  auto db = std::unique_ptr<lfDatabase, LensfunDbDeleter>(lf_db_create());
  if (!db) {
    state.db_.reset();
    state.root_.clear();
    state.valid_ = false;
    return nullptr;
  }

  const bool ok = LoadPortableDbXmls(db.get(), canonical_root);
  state.valid_  = ok;
  state.root_   = canonical_root;
  if (!ok) {
    state.db_.reset();
    return nullptr;
  }

  state.db_ = std::move(db);
  return state.db_.get();
}

auto IsFinitePositive(float value) -> bool { return std::isfinite(value) && value > 0.0f; }

void RescalePaVignettingTerms(lfLensCalibVignetting* vignette, float real_focal_mm,
                              float crop_factor) {
  if (!vignette || vignette->Model != LF_VIGNETTING_MODEL_PA) {
    return;
  }
  if (!IsFinitePositive(real_focal_mm) || !IsFinitePositive(crop_factor)) {
    return;
  }

  const float hugin_scale_in_mm = (kFullFrameDiagonalMm / crop_factor) * 0.5f;
  if (!IsFinitePositive(hugin_scale_in_mm)) {
    return;
  }

  const float hugin_scaling = real_focal_mm / hugin_scale_in_mm;
  const float hs2           = hugin_scaling * hugin_scaling;
  vignette->Terms[0] *= hs2;
  vignette->Terms[1] *= hs2 * hs2;
  vignette->Terms[2] *= hs2 * hs2 * hs2;
}

auto HuginScaleInMillimeters(float crop_factor, float aspect_ratio) -> float {
  if (!IsFinitePositive(crop_factor) || !IsFinitePositive(aspect_ratio)) {
    return 0.0f;
  }
  return kFullFrameDiagonalMm / crop_factor / std::hypot(aspect_ratio, 1.0f) * 0.5f;
}

void RescaleDistortionTerms(lfLensCalibDistortion* distortion, float real_focal_mm) {
  if (!distortion || !IsFinitePositive(real_focal_mm)) {
    return;
  }
  const float hugin_scale_in_mm =
      HuginScaleInMillimeters(distortion->CalibAttr.CropFactor, distortion->CalibAttr.AspectRatio);
  if (!IsFinitePositive(hugin_scale_in_mm)) {
    return;
  }

  const float hugin_scaling = real_focal_mm / hugin_scale_in_mm;
  switch (distortion->Model) {
    case LF_DIST_MODEL_POLY3: {
      const float d = 1.0f - distortion->Terms[0];
      if (std::fabs(d) <= kEpsilon) {
        return;
      }
      distortion->Terms[0] *= std::pow(hugin_scaling, 2.0f) / std::pow(d, 3.0f);
      break;
    }
    case LF_DIST_MODEL_POLY5:
      distortion->Terms[0] *= std::pow(hugin_scaling, 2.0f);
      distortion->Terms[1] *= std::pow(hugin_scaling, 4.0f);
      break;
    case LF_DIST_MODEL_PTLENS: {
      const float d = 1.0f - distortion->Terms[0] - distortion->Terms[1] - distortion->Terms[2];
      if (std::fabs(d) <= kEpsilon) {
        return;
      }
      distortion->Terms[0] *= std::pow(hugin_scaling, 3.0f) / std::pow(d, 4.0f);
      distortion->Terms[1] *= std::pow(hugin_scaling, 2.0f) / std::pow(d, 3.0f);
      distortion->Terms[2] *= hugin_scaling / std::pow(d, 2.0f);
      break;
    }
    default:
      break;
  }
}

void RescaleTcaTerms(lfLensCalibTCA* tca, float real_focal_mm) {
  if (!tca || !IsFinitePositive(real_focal_mm)) {
    return;
  }
  const float hugin_scale_in_mm =
      HuginScaleInMillimeters(tca->CalibAttr.CropFactor, tca->CalibAttr.AspectRatio);
  if (!IsFinitePositive(hugin_scale_in_mm)) {
    return;
  }

  const float hugin_scaling = real_focal_mm / hugin_scale_in_mm;
  if (tca->Model == LF_TCA_MODEL_POLY3) {
    tca->Terms[2] *= hugin_scaling;
    tca->Terms[3] *= hugin_scaling;
    tca->Terms[4] *= hugin_scaling * hugin_scaling;
    tca->Terms[5] *= hugin_scaling * hugin_scaling;
  }
}

auto CanonicalizeProjectionToken(std::string text) -> std::string {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (char& c : text) {
    if (c == '-' || c == ' ') {
      c = '_';
    }
  }
  return text;
}

auto ProjectionFromString(const std::string& text) -> LensCalibProjectionType {
  const std::string token = CanonicalizeProjectionToken(text);
  if (token == "rectilinear") {
    return LensCalibProjectionType::RECTILINEAR;
  }
  if (token == "fisheye") {
    return LensCalibProjectionType::FISHEYE;
  }
  if (token == "panoramic") {
    return LensCalibProjectionType::PANORAMIC;
  }
  if (token == "equirectangular") {
    return LensCalibProjectionType::EQUIRECTANGULAR;
  }
  if (token == "fisheye_orthographic" || token == "orthographic") {
    return LensCalibProjectionType::FISHEYE_ORTHOGRAPHIC;
  }
  if (token == "fisheye_stereographic" || token == "stereographic") {
    return LensCalibProjectionType::FISHEYE_STEREOGRAPHIC;
  }
  if (token == "fisheye_equisolid" || token == "equisolid") {
    return LensCalibProjectionType::FISHEYE_EQUISOLID;
  }
  if (token == "fisheye_thoby" || token == "thoby") {
    return LensCalibProjectionType::FISHEYE_THOBY;
  }
  return LensCalibProjectionType::UNKNOWN;
}

auto LensTypeFromLensfun(lfLensType type) -> LensCalibProjectionType {
  switch (type) {
    case LF_RECTILINEAR:
      return LensCalibProjectionType::RECTILINEAR;
    case LF_FISHEYE:
      return LensCalibProjectionType::FISHEYE;
    case LF_PANORAMIC:
      return LensCalibProjectionType::PANORAMIC;
    case LF_EQUIRECTANGULAR:
      return LensCalibProjectionType::EQUIRECTANGULAR;
    case LF_FISHEYE_ORTHOGRAPHIC:
      return LensCalibProjectionType::FISHEYE_ORTHOGRAPHIC;
    case LF_FISHEYE_STEREOGRAPHIC:
      return LensCalibProjectionType::FISHEYE_STEREOGRAPHIC;
    case LF_FISHEYE_EQUISOLID:
      return LensCalibProjectionType::FISHEYE_EQUISOLID;
    case LF_FISHEYE_THOBY:
      return LensCalibProjectionType::FISHEYE_THOBY;
    case LF_UNKNOWN:
    default:
      return LensCalibProjectionType::UNKNOWN;
  }
}

auto LensTypeToLensfun(LensCalibProjectionType type) -> lfLensType {
  switch (type) {
    case LensCalibProjectionType::RECTILINEAR:
      return LF_RECTILINEAR;
    case LensCalibProjectionType::FISHEYE:
      return LF_FISHEYE;
    case LensCalibProjectionType::PANORAMIC:
      return LF_PANORAMIC;
    case LensCalibProjectionType::EQUIRECTANGULAR:
      return LF_EQUIRECTANGULAR;
    case LensCalibProjectionType::FISHEYE_ORTHOGRAPHIC:
      return LF_FISHEYE_ORTHOGRAPHIC;
    case LensCalibProjectionType::FISHEYE_STEREOGRAPHIC:
      return LF_FISHEYE_STEREOGRAPHIC;
    case LensCalibProjectionType::FISHEYE_EQUISOLID:
      return LF_FISHEYE_EQUISOLID;
    case LensCalibProjectionType::FISHEYE_THOBY:
      return LF_FISHEYE_THOBY;
    case LensCalibProjectionType::UNKNOWN:
    default:
      return LF_UNKNOWN;
  }
}

auto FindBestCamera(const lfDatabase* db, const std::string& maker, const std::string& model)
    -> const lfCamera* {
  if (!db || maker.empty() || model.empty()) {
    return nullptr;
  }
  const lfCamera** cameras = lf_db_find_cameras(db, maker.c_str(), model.c_str());
  if (!cameras) {
    return nullptr;
  }
  const lfCamera* best = cameras[0];
  lf_free((void*)cameras);
  return best;
}

/// +1 when @p value lies in [@p min_value - @p slack, @p max_value + @p slack], -1 when it lies
/// outside, and 0 when the value or the range is unknown.
auto RangeFit(float value, float min_value, float max_value, float slack) -> int {
  if (!IsFinitePositive(value) || !IsFinitePositive(min_value) || !IsFinitePositive(max_value)) {
    return 0;
  }
  return (value >= min_value - slack && value <= max_value + slack) ? 1 : -1;
}

/**
 * @brief Order of one Lensfun search result; a larger rank is a better match.
 *
 * The Lensfun score comes first: it rates the lens name, maker, mount, and crop factor, and the
 * focal length and aperture that the searched name states. The shooting focal length and
 * aperture only decide between results with equal Lensfun scores, so a lens with another name
 * cannot replace the named lens because its range fits the shot. A result for a RAW lens name
 * that fails @ref PassesShotLensCheck takes no part in the ranking.
 */
auto RankLensCandidate(const lfLens* lens, const LensInputMeta& input)
    -> std::tuple<int, int, int> {
  return {lens->Score, RangeFit(input.focal_length_mm_, lens->MinFocal, lens->MaxFocal, 0.2f),
          RangeFit(input.aperture_f_number_, lens->MinAperture, lens->MaxAperture, 0.1f)};
}

/**
 * @brief @p name with one spelling of the focal range and of the spaces.
 *
 * EXIF names write a focal range as "28-70mm", "28 - 70mm", or with an en dash (U+2013). The
 * result writes each as "28-70mm", with single spaces and no space at either end. Lensfun splits
 * names into the same words in all three forms, but reads the focal range only from "28-70mm":
 * from "28 - 70mm f2.8" it reads a 70 mm lens (issue #1142).
 */
auto CanonicalLensName(std::string_view name) -> std::string {
  constexpr std::string_view kEnDash = "\xE2\x80\x93";

  std::string                spaced;
  spaced.reserve(name.size());
  for (std::size_t i = 0; i < name.size(); ++i) {
    if (name.substr(i, kEnDash.size()) == kEnDash) {
      spaced.push_back('-');
      i += kEnDash.size() - 1;
      continue;
    }
    const auto c = static_cast<unsigned char>(name[i]);
    if (std::isspace(c) != 0) {
      if (!spaced.empty() && spaced.back() != ' ') {
        spaced.push_back(' ');
      }
      continue;
    }
    spaced.push_back(static_cast<char>(c));
  }
  if (!spaced.empty() && spaced.back() == ' ') {
    spaced.pop_back();
  }

  const auto is_digit_at = [&spaced](std::size_t i) {
    return i < spaced.size() && std::isdigit(static_cast<unsigned char>(spaced[i])) != 0;
  };
  std::string canonical;
  canonical.reserve(spaced.size());
  for (std::size_t i = 0; i < spaced.size(); ++i) {
    if (spaced[i] == ' ') {
      // A space before "-" in "28 - 70" or "28 -70".
      const bool before_range_dash =
          !canonical.empty() && std::isdigit(static_cast<unsigned char>(canonical.back())) != 0 &&
          i + 1 < spaced.size() && spaced[i + 1] == '-' &&
          (is_digit_at(i + 2) ||
           (i + 2 < spaced.size() && spaced[i + 2] == ' ' && is_digit_at(i + 3)));
      // A space after "-" in "28 - 70" or "28- 70".
      const bool after_range_dash =
          canonical.size() >= 2 && canonical.back() == '-' &&
          std::isdigit(static_cast<unsigned char>(canonical[canonical.size() - 2])) != 0 &&
          is_digit_at(i + 1);
      if (before_range_dash || after_range_dash) {
        continue;
      }
    }
    canonical.push_back(spaced[i]);
  }
  return canonical;
}

/**
 * @brief @p name with each aperture letter written as @p letter ('f' or 'F').
 *
 * An aperture letter is an "f" or "F" that starts a word and comes before a digit or "/"
 * ("F2.8", "f/2.8"). Lensfun reads the focal range and the aperture from the searched name only
 * when that letter is a lower-case "f"; with an upper-case "F" it compares only the words. The
 * letter does not change the words: Lensfun skips a word of one letter "f".
 */
auto WithApertureLetter(std::string name, char letter) -> std::string {
  for (std::size_t i = 0; i + 1 < name.size(); ++i) {
    if (name[i] != 'F' && name[i] != 'f') {
      continue;
    }
    const auto previous = i == 0 ? ' ' : static_cast<unsigned char>(name[i - 1]);
    const auto next     = static_cast<unsigned char>(name[i + 1]);
    if (std::isalnum(previous) == 0 && (std::isdigit(next) != 0 || next == '/')) {
      name[i] = letter;
    }
  }
  return name;
}

/// Focal range and widest aperture that a lens name states.
struct NamedLensSpec {
  /// Focal range in millimetres; a prime has equal ends.
  float min_focal_mm_ = 0.0f;
  float max_focal_mm_ = 0.0f;
  /// Widest aperture (smallest f-number) after the focal range; zero when the name states none.
  float min_aperture_ = 0.0f;
};

/// Value of a decimal number of digits and one ".". The C locale of the process can use another
/// decimal separator, so the number is read here.
auto DecimalValue(const std::string& digits) -> float {
  float value = 0.0f;
  float scale = 0.0f;
  for (const char c : digits) {
    if (c == '.') {
      scale = 0.1f;
    } else if (scale == 0.0f) {
      value = value * 10.0f + static_cast<float>(c - '0');
    } else {
      value += scale * static_cast<float>(c - '0');
      scale *= 0.1f;
    }
  }
  return value;
}

/**
 * @brief Focal range that @p name states before "mm" ("28-70mm", "28 - 70mm", "RF50mm",
 * "XF18-55mmF2.8"), with the aperture right after it ("F2.8", "f/2.8", "1:2.8"); nothing when the
 * name states no focal range ("LEICA DG 12-60/F2.8-4.0").
 */
auto LensSpecInName(std::string_view name) -> std::optional<NamedLensSpec> {
  // [min focal][-max focal]mm, then an optional [F|f|f/|1:][min aperture].
  static const std::regex kSpec(R"(([0-9]+(?:\.[0-9]+)?)(?:-([0-9]+(?:\.[0-9]+)?))? ?mm)"
                                R"((?: ?(?:[fF]/?|1:) ?([0-9]+(?:\.[0-9]+)?))?)");
  const std::string       canonical = CanonicalLensName(name);
  std::smatch             match;
  if (!std::regex_search(canonical, match, kSpec)) {
    return std::nullopt;
  }
  NamedLensSpec spec;
  spec.min_focal_mm_ = DecimalValue(match[1].str());
  spec.max_focal_mm_ = match[2].matched ? DecimalValue(match[2].str()) : spec.min_focal_mm_;
  spec.min_aperture_ = match[3].matched ? DecimalValue(match[3].str()) : 0.0f;
  return spec;
}

/**
 * @brief Words of a lens name as Lensfun splits them: runs of digits with their decimal point
 * (without a final ".0"), of punctuation, and of other characters, in lower case. A word of one
 * punctuation character or a single "f" is not a word; "+" and "*" are.
 */
auto LensNameWords(std::string_view name) -> std::vector<std::string> {
  const auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
  const auto is_digit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
  const auto is_punct = [](char c) { return std::ispunct(static_cast<unsigned char>(c)) != 0; };

  std::vector<std::string> words;
  std::size_t              i = 0;
  while (i < name.size()) {
    if (is_space(name[i])) {
      ++i;
      continue;
    }
    const std::size_t start = i++;
    std::size_t       strip = 0;
    if (is_digit(name[start])) {
      while (i < name.size() && (is_digit(name[i]) || name[i] == '.')) {
        ++i;
      }
      if (i - start >= 2 && name[i - 2] == '.' && name[i - 1] == '0') {
        strip = 2;
      }
    } else if (is_punct(name[start])) {
      while (i < name.size() && is_punct(name[i])) {
        ++i;
      }
    } else {
      while (i < name.size() && !is_space(name[i]) && !is_digit(name[i]) && !is_punct(name[i])) {
        ++i;
      }
    }
    const char first = name[start];
    if (i - start == 1 && (is_punct(first) || first == 'f' || first == 'F') && first != '*' &&
        first != '+') {
      continue;
    }
    std::string word(name.substr(start, i - start - strip));
    std::transform(word.begin(), word.end(), word.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    words.push_back(std::move(word));
  }
  std::sort(words.begin(), words.end());
  return words;
}

/**
 * @brief Share of the RAW name words @p raw_words that the profile name @p profile_name also has,
 * 0 to 1.
 *
 * Lensfun rates the shared words too, but adds that rating to the points of focal length,
 * aperture, mount, and crop factor, so its score cannot tell a name that shares one word. A
 * profile name may add words that the RAW name leaves out (maker, series, "IF-ED"), so the share
 * counts only the words of the RAW name.
 */
auto ShareOfRawWordsInProfile(const std::vector<std::string>& raw_words,
                              std::string_view                profile_name) -> float {
  const std::vector<std::string> profile_words = LensNameWords(profile_name);
  if (raw_words.empty()) {
    return 0.0f;
  }
  std::vector<std::string> common;
  std::set_intersection(raw_words.begin(), raw_words.end(), profile_words.begin(),
                        profile_words.end(), std::back_inserter(common));
  return static_cast<float>(common.size()) / static_cast<float>(raw_words.size());
}

/// Least share of the RAW name words in a profile name. Of the 7 words of "70mm F2.8 DG Macro |
/// Art 018", "Sigma 70mm f/2.8 DG Macro | Art 018" has all 7, and "LUMIX S 24-60/F2.8" has 1.
constexpr float kMinShareOfRawWords     = 0.6f;
/// Largest difference between the focal lengths of two names that still names the same lens.
constexpr float kNamedFocalToleranceMm  = 0.5f;
/// Largest difference between the f-numbers of two names that still names the same lens.
constexpr float kNamedApertureTolerance = 0.05f;
/// The shooting focal length may lie this far outside the profile range: at least
/// @ref kShotFocalMinSlackMm, and @ref kShotFocalSlackRatio of the longest focal length.
constexpr float kShotFocalMinSlackMm    = 1.0f;
constexpr float kShotFocalSlackRatio    = 0.05f;
/// A recorded focal length below this is no measurement: some lenses on Sony bodies record
/// 0.1 mm or 0.2 mm.
constexpr float kLeastRecordedFocalMm   = 1.0f;

/**
 * @brief What a RAW lens name and the shot state about the lens, which every profile that
 * auto-detect accepts must agree with.
 */
struct ShotLensCheck {
  /// Words of the RAW lens name (@ref LensNameWords).
  std::vector<std::string>     name_words_;
  /// Focal range and aperture of the RAW lens name, when it states a focal range.
  std::optional<NamedLensSpec> name_spec_;
  /// Focal length that the camera recorded for the shot; zero when unknown.
  float                        shot_focal_mm_ = 0.0f;
};

auto DiffersBeyond(float a, float b, float tolerance) -> bool {
  return IsFinitePositive(a) && IsFinitePositive(b) && std::fabs(a - b) > tolerance;
}

/**
 * @brief True when @p lens can be the lens that @p check describes.
 *
 * Lensfun ranks profiles by their words, so a profile whose name shares the words "70mm", "2.8",
 * and "DG" with "28-70mm F2.8 DG DN" can win. Lensfun compares the focal range and the aperture
 * only with values that the profile XML states, and most profiles state none. These facts reject
 * a profile:
 * - its name has less than @ref kMinShareOfRawWords of the RAW name words;
 * - its name states another focal range or another widest aperture than the RAW name;
 * - its focal range (from the XML, else from its name) does not contain the shooting focal
 *   length.
 * A value that one side does not state rejects nothing.
 */
auto PassesShotLensCheck(const lfLens* lens, const ShotLensCheck& check) -> bool {
  if (lens->Model == nullptr ||
      ShareOfRawWordsInProfile(check.name_words_, lens->Model) < kMinShareOfRawWords) {
    return false;
  }
  const std::optional<NamedLensSpec> profile_spec = LensSpecInName(lens->Model);
  if (check.name_spec_ && profile_spec &&
      (DiffersBeyond(profile_spec->min_focal_mm_, check.name_spec_->min_focal_mm_,
                     kNamedFocalToleranceMm) ||
       DiffersBeyond(profile_spec->max_focal_mm_, check.name_spec_->max_focal_mm_,
                     kNamedFocalToleranceMm) ||
       DiffersBeyond(profile_spec->min_aperture_, check.name_spec_->min_aperture_,
                     kNamedApertureTolerance))) {
    return false;
  }

  if (check.shot_focal_mm_ < kLeastRecordedFocalMm) {
    return true;
  }
  float min_mm = lens->MinFocal;
  float max_mm = lens->MaxFocal;
  if ((!IsFinitePositive(min_mm) || !IsFinitePositive(max_mm)) && profile_spec) {
    min_mm = profile_spec->min_focal_mm_;
    max_mm = profile_spec->max_focal_mm_;
  }
  const float slack = std::max(kShotFocalMinSlackMm, kShotFocalSlackRatio * max_mm);
  return RangeFit(check.shot_focal_mm_, min_mm, max_mm, slack) >= 0;
}

/**
 * @brief Best entry of a Lensfun search result by @ref RankLensCandidate; frees @p lenses.
 *
 * With @p check, only entries that pass @ref PassesShotLensCheck compete.
 */
auto TakeBestRankedLens(const lfLens** lenses, const LensInputMeta& input,
                        const ShotLensCheck* check = nullptr) -> const lfLens* {
  if (!lenses) {
    return nullptr;
  }
  const lfLens*             best = nullptr;
  std::tuple<int, int, int> best_rank{};
  for (int i = 0; lenses[i] != nullptr; ++i) {
    if (check && !PassesShotLensCheck(lenses[i], *check)) {
      continue;
    }
    const auto rank = RankLensCandidate(lenses[i], input);
    if (best == nullptr || rank > best_rank) {
      best_rank = rank;
      best      = lenses[i];
    }
  }
  lf_free((void*)lenses);
  return best;
}

/**
 * @brief Profile for the lens that the RAW names, or for the lens that the user selected.
 *
 * A RAW lens name is searched twice, and each result must pass @ref PassesShotLensCheck:
 * 1. with a lower-case aperture letter, so Lensfun also compares the focal range and the aperture
 *    that the name states: "E 15-30mm F2.8" no longer matches the Zeiss "E 21mm F2.8" (#1128);
 * 2. when that finds no profile, with an upper-case aperture letter, so Lensfun compares only the
 *    words. A name that Lensfun reads wrongly still finds its profile this way.
 * When neither search finds a profile, the shot has none, and the user selects the lens.
 */
auto FindBestLens(const lfDatabase* db, const lfCamera* camera, const LensInputMeta& input)
    -> const lfLens* {
  if (!db || input.lens_model_.empty()) {
    return nullptr;
  }

  const char* lens_maker = input.lens_maker_.empty() ? nullptr : input.lens_maker_.c_str();
  if (input.lens_selected_) {
    // A catalog selection is a Lensfun profile name: every word must match, and no camera is
    // given, so Lensfun does not reject the profile for its mount.
    return TakeBestRankedLens(lf_db_find_lenses(db, nullptr, lens_maker, input.lens_model_.c_str(),
                                                LF_SEARCH_SORT_AND_UNIQUIFY),
                              input);
  }

  const std::string   canonical = CanonicalLensName(input.lens_model_);
  const ShotLensCheck check{LensNameWords(canonical), LensSpecInName(canonical),
                            input.focal_length_mm_};
  constexpr int       flags = LF_SEARCH_SORT_AND_UNIQUIFY | LF_SEARCH_LOOSE;
  for (const char letter : {'f', 'F'}) {
    const std::string search_name = WithApertureLetter(canonical, letter);
    if (const lfLens* lens = TakeBestRankedLens(
            lf_db_find_lenses(db, camera, lens_maker, search_name.c_str(), flags), input, &check)) {
      return lens;
    }
  }
  return nullptr;
}

/**
 * @brief True when @p camera has a built-in lens.
 *
 * The Lensfun database names the mount of a fixed-lens camera with a lower-case first letter
 * (`fujiGFX100RF`, `ricohGRIIIx`) and an interchangeable mount with an upper-case first letter
 * (`Fujifilm G`, `Sony E`).
 */
auto HasFixedLens(const lfCamera* camera) -> bool {
  return camera != nullptr && camera->Mount != nullptr &&
         std::islower(static_cast<unsigned char>(camera->Mount[0])) != 0;
}

/// Lensfun word-match score of a camera whose maker and model both equal a database name.
constexpr int kExactMakerAndModelScore = 200;

/**
 * @brief Lensfun camera with a built-in lens named by @p maker and @p model, or null.
 *
 * The exact Lensfun search compares only the first name of a camera entry, which follows the EXIF
 * name ("Ricoh Imaging Company, Ltd." / "Ricoh GR IIIx"). The RAW loader reports the LibRaw names
 * ("Ricoh" / "GR IIIx"), which the entries list as English names. When the exact search finds no
 * camera, a word search over all names accepts a camera whose maker and model words both match.
 */
auto FindFixedLensCamera(const lfDatabase* db, const std::string& maker, const std::string& model)
    -> const lfCamera* {
  if (const lfCamera* exact = FindBestCamera(db, maker, model)) {
    return HasFixedLens(exact) ? exact : nullptr;
  }
  if (!db || maker.empty() || model.empty()) {
    return nullptr;
  }
  const lfCamera** cameras = lf_db_find_cameras_ext(db, maker.c_str(), model.c_str(), 0);
  if (!cameras) {
    return nullptr;
  }
  // The search result is sorted by score, best first.
  const lfCamera* best =
      (cameras[0] && cameras[0]->Score >= kExactMakerAndModelScore) ? cameras[0] : nullptr;
  lf_free((void*)cameras);
  return HasFixedLens(best) ? best : nullptr;
}

/// Profile of the built-in lens of a fixed-lens @p camera: the best ranked profile on its mount.
auto FindFixedLens(const lfDatabase* db, const lfCamera* camera, const LensInputMeta& input)
    -> const lfLens* {
  if (!db || !HasFixedLens(camera)) {
    return nullptr;
  }
  return TakeBestRankedLens(
      lf_db_find_lenses(db, camera, nullptr, nullptr, LF_SEARCH_SORT_AND_UNIQUIFY), input);
}

/// Matched Lensfun camera and lens of one shot. @c camera_ can be null when @c lens_ is not.
struct ShotLens {
  const lfCamera* camera_ = nullptr;
  const lfLens*   lens_   = nullptr;
};

/**
 * @brief Camera and lens profile for the lens that took the shot.
 *
 * The lens name decides first. A fixed-lens camera whose RAW names no lens, or names one that no
 * profile matches, gets the profile of its built-in lens (issue #960).
 */
auto FindShotLens(const lfDatabase* db, const LensInputMeta& input) -> ShotLens {
  const lfCamera* camera = FindBestCamera(db, input.cam_maker_, input.cam_model_);
  if (const lfLens* named = FindBestLens(db, camera, input)) {
    return {camera, named};
  }
  const lfCamera* fixed_camera = FindFixedLensCamera(db, input.cam_maker_, input.cam_model_);
  if (const lfLens* built_in = FindFixedLens(db, fixed_camera, input)) {
    return {fixed_camera, built_in};
  }
  return {camera, nullptr};
}

auto ResolveScaleFromModifier(const lfLens* lens, float focal_mm, float crop_factor, int width,
                              int height, bool apply_distortion, bool apply_tca,
                              bool apply_projection, lfLensType target_projection) -> float {
  if (!lens || !IsFinitePositive(focal_mm) || !IsFinitePositive(crop_factor)) {
    return 1.0f;
  }

  auto modifier = std::unique_ptr<lfModifier, LensfunModifierDeleter>(
      lf_modifier_create(lens, focal_mm, crop_factor, width, height, LF_PF_F32, false));
  if (!modifier) {
    return 1.0f;
  }

  int flags = 0;
  if (apply_distortion) {
    flags |= LF_MODIFY_DISTORTION;
  }
  if (apply_tca) {
    flags |= LF_MODIFY_TCA;
  }
  if (apply_projection) {
    flags |= LF_MODIFY_GEOMETRY;
  }
  if (flags == 0) {
    return 1.0f;
  }

  const lfLensType source_projection = lens->Type;
  const lfLensType resolved_target   = apply_projection ? target_projection : source_projection;
  if (apply_distortion) {
    (void)lf_modifier_enable_distortion_correction(modifier.get());
  }
  if (apply_tca) {
    (void)lf_modifier_enable_tca_correction(modifier.get());
  }
  if (apply_projection) {
    (void)lf_modifier_enable_projection_transform(modifier.get(), resolved_target);
  }

  const float scale = lf_modifier_get_auto_scale(modifier.get(), false);
  if (!IsFinitePositive(scale)) {
    return 1.0f;
  }
  return scale;
}

auto ResolveScaleForImageSize(const LensInputMeta& meta, const std::filesystem::path& db_path,
                              const LensCalibGpuParams& params, int width, int height) -> float {
  const auto  db_root = ResolveDbRootPath(db_path);
  lfDatabase* db      = GetLensfunDb(db_root);
  if (!db) {
    return params.resolved_scale;
  }

  const lfLens* lens = FindShotLens(db, meta).lens_;
  if (!lens) {
    return params.resolved_scale;
  }

  return ResolveScaleFromModifier(
      lens, meta.focal_length_mm_, params.camera_crop_factor, width, height,
      params.apply_distortion != 0, params.apply_tca != 0, params.apply_projection != 0,
      LensTypeToLensfun(static_cast<LensCalibProjectionType>(params.target_projection)));
}

}  // namespace

auto LensCalibrationResolver::Resolve(const DevelopPayload&         develop,
                                      const RawRuntimeColorContext& context, Extent2D extent,
                                      dng::WarpCorrections dng_warp)
    -> std::optional<LensCalibGpuParams> {
  if (!develop.lens_enabled) {
    return std::nullopt;
  }
  if (extent.Empty()) {
    throw std::runtime_error("LensCalibrationResolver: cannot resolve an empty Develop image");
  }

  LensInputMeta meta;
  meta.cam_maker_         = context.camera_make_;
  meta.cam_model_         = context.camera_model_;
  meta.lens_maker_        = develop.lens_maker.empty() ? context.lens_make_ : develop.lens_maker;
  meta.lens_model_        = develop.lens_model.empty() ? context.lens_model_ : develop.lens_model;
  meta.lens_selected_     = !develop.lens_model.empty();
  meta.focal_length_mm_   = context.focal_length_mm_;
  meta.aperture_f_number_ = context.aperture_f_number_;
  meta.distance_m_        = context.focus_distance_m_;
  meta.focal_35mm_mm_     = context.focal_35mm_mm_;
  meta.crop_factor_hint_  = context.crop_factor_hint_;

  LensCorrectionSettings settings;
  settings.database_path_      = std::filesystem::path(develop.lens_profile_db_path);
  settings.apply_vignetting_   = develop.apply_vignetting;
  settings.apply_distortion_   = develop.apply_distortion;
  settings.apply_tca_          = develop.apply_tca;
  settings.apply_crop_         = develop.apply_crop;
  settings.auto_scale_         = develop.auto_scale;
  settings.use_user_scale_     = develop.use_user_scale;
  settings.user_scale_         = develop.user_scale;
  settings.projection_enabled_ = develop.projection_enabled;
  settings.target_projection_  = develop.target_projection;

  const auto profile           = ResolveProfile(meta, settings, dng_warp);
  if (profile.status_ != LensProfileStatus::Resolved) {
    return std::nullopt;
  }

  LensCalibGpuParams runtime = profile.params_;
  BindImageExtent(profile.matched_meta_, settings.database_path_, runtime,
                  static_cast<int>(extent.width), static_cast<int>(extent.height));
  return runtime;
}

auto LensCalibrationResolver::ResolveProfile(const LensInputMeta&          meta,
                                             const LensCorrectionSettings& settings,
                                             dng::WarpCorrections          dng_warp)
    -> LensProfileResolution {
  LensProfileResolution result;
  if (!IsFinitePositive(meta.focal_length_mm_) ||
      (meta.lens_model_.empty() && meta.cam_model_.empty())) {
    result.status_ = LensProfileStatus::MissingLensIdentity;
    return result;
  }

  const auto  db_root = ResolveDbRootPath(settings.database_path_);
  lfDatabase* db      = GetLensfunDb(db_root);
  if (!db) {
    result.status_ = LensProfileStatus::DatabaseUnavailable;
    return result;
  }

  const auto [camera, lens] = FindShotLens(db, meta);
  if (!lens) {
    // Without a lens name only the camera can name the lens, and only when the lens is built in.
    result.status_ = meta.lens_model_.empty() ? LensProfileStatus::MissingLensIdentity
                                              : LensProfileStatus::LensNotInDatabase;
    return result;
  }

  float crop_factor = (camera && IsFinitePositive(camera->CropFactor)) ? camera->CropFactor : 0.0f;
  if (!IsFinitePositive(crop_factor)) {
    crop_factor = meta.crop_factor_hint_;
  }
  if (!IsFinitePositive(crop_factor)) {
    crop_factor = 1.0f;
  }

  lfLensCalibDistortion distortion{};
  const bool            distortion_ok =
      lf_lens_interpolate_distortion(lens, crop_factor, meta.focal_length_mm_, &distortion) != 0;
  float real_focal_mm = meta.focal_length_mm_;
  if (distortion_ok && IsFinitePositive(distortion.RealFocal)) {
    real_focal_mm = distortion.RealFocal;
  }
  if (distortion_ok) {
    RescaleDistortionTerms(&distortion, real_focal_mm);
  }

  lfLensCalibTCA tca{};
  const bool tca_ok = lf_lens_interpolate_tca(lens, crop_factor, meta.focal_length_mm_, &tca) != 0;
  if (tca_ok) {
    RescaleTcaTerms(&tca, real_focal_mm);
  }

  lfLensCalibVignetting vignette{};
  const float           safe_aperture =
      IsFinitePositive(meta.aperture_f_number_) ? meta.aperture_f_number_ : 0.0f;
  const float safe_distance =
      IsFinitePositive(meta.distance_m_) ? meta.distance_m_ : kDefaultFarDistanceM;
  const bool vignette_ok =
      IsFinitePositive(safe_aperture) &&
      lf_lens_interpolate_vignetting(lens, crop_factor, meta.focal_length_mm_, safe_aperture,
                                     safe_distance, &vignette) != 0;
  if (vignette_ok) {
    RescalePaVignettingTerms(&vignette, real_focal_mm, crop_factor);
  }

  lfLensCalibCrop crop{};
  const bool      crop_ok =
      lf_lens_interpolate_crop(lens, crop_factor, meta.focal_length_mm_, &crop) != 0;
  // A DNG warp that corrects distortion owns the geometry. Camera Raw also writes a warp that
  // corrects only lateral chromatic aberration (issue #1144); Lensfun then corrects the rest.
  const bool         dng_geometry_wins = dng_warp.distortion;
  const bool         dng_tca_wins      = dng_warp.distortion || dng_warp.lateral_ca;

  LensCalibGpuParams runtime{};
  result.matched_meta_          = meta;
  runtime.src_width             = 0;
  runtime.src_height            = 0;
  runtime.nominal_focal_mm      = meta.focal_length_mm_;
  runtime.real_focal_mm         = real_focal_mm;
  runtime.camera_crop_factor    = crop_factor;
  runtime.user_scale            = settings.user_scale_;
  runtime.use_user_scale        = settings.use_user_scale_ ? 1 : 0;
  runtime.use_auto_scale        = (settings.auto_scale_ && !settings.use_user_scale_) ? 1 : 0;
  runtime.low_precision_preview = settings.low_precision_preview_ ? 1 : 0;
  runtime.lens_center_x         = lens->CenterX;
  runtime.lens_center_y         = lens->CenterY;

  runtime.source_projection     = static_cast<std::int32_t>(LensTypeFromLensfun(lens->Type));
  const auto target_projection  = settings.projection_enabled_
                                      ? ProjectionFromString(settings.target_projection_)
                                      : LensCalibProjectionType::UNKNOWN;
  runtime.target_projection     = static_cast<std::int32_t>(target_projection);

  runtime.apply_distortion =
      (!dng_geometry_wins && settings.apply_distortion_ && distortion_ok &&
       (distortion.Model == LF_DIST_MODEL_POLY3 || distortion.Model == LF_DIST_MODEL_POLY5 ||
        distortion.Model == LF_DIST_MODEL_PTLENS))
          ? 1
          : 0;
  runtime.apply_tca = (!dng_tca_wins && settings.apply_tca_ && tca_ok &&
                       (tca.Model == LF_TCA_MODEL_LINEAR || tca.Model == LF_TCA_MODEL_POLY3))
                          ? 1
                          : 0;
  runtime.apply_vignetting =
      (settings.apply_vignetting_ && vignette_ok && vignette.Model == LF_VIGNETTING_MODEL_PA) ? 1
                                                                                              : 0;

  const bool projection_valid = target_projection != LensCalibProjectionType::UNKNOWN &&
                                target_projection != LensTypeFromLensfun(lens->Type);
  runtime.apply_projection =
      (!dng_geometry_wins && settings.projection_enabled_ && projection_valid) ? 1 : 0;

  const bool crop_profile_valid =
      crop_ok && (crop.CropMode == LF_CROP_RECTANGLE || crop.CropMode == LF_CROP_CIRCLE);
  runtime.apply_crop = (!dng_geometry_wins && settings.apply_crop_ && crop_profile_valid) ? 1 : 0;
  runtime.apply_crop_circle = (runtime.apply_crop && crop.CropMode == LF_CROP_CIRCLE) ? 1 : 0;

  switch (distortion.Model) {
    case LF_DIST_MODEL_POLY3:
      runtime.distortion_model = static_cast<std::int32_t>(LensCalibDistortionModel::POLY3);
      break;
    case LF_DIST_MODEL_POLY5:
      runtime.distortion_model = static_cast<std::int32_t>(LensCalibDistortionModel::POLY5);
      break;
    case LF_DIST_MODEL_PTLENS:
      runtime.distortion_model = static_cast<std::int32_t>(LensCalibDistortionModel::PTLENS);
      break;
    default:
      runtime.distortion_model = static_cast<std::int32_t>(LensCalibDistortionModel::NONE);
      break;
  }
  std::memcpy(runtime.distortion_terms, distortion.Terms, sizeof(distortion.Terms));

  switch (tca.Model) {
    case LF_TCA_MODEL_LINEAR:
      runtime.tca_model = static_cast<std::int32_t>(LensCalibTCAModel::LINEAR);
      break;
    case LF_TCA_MODEL_POLY3:
      runtime.tca_model = static_cast<std::int32_t>(LensCalibTCAModel::POLY3);
      break;
    default:
      runtime.tca_model = static_cast<std::int32_t>(LensCalibTCAModel::NONE);
      break;
  }
  std::memcpy(runtime.tca_terms, tca.Terms, sizeof(tca.Terms));

  runtime.vignetting_model = static_cast<std::int32_t>(LensCalibVignettingModel::NONE);
  if (vignette.Model == LF_VIGNETTING_MODEL_PA) {
    runtime.vignetting_model = static_cast<std::int32_t>(LensCalibVignettingModel::PA);
  }
  std::memcpy(runtime.vignetting_terms, vignette.Terms, sizeof(runtime.vignetting_terms));

  runtime.crop_mode = static_cast<std::int32_t>(LensCalibCropMode::NONE);
  if (crop_profile_valid && crop.CropMode == LF_CROP_RECTANGLE) {
    runtime.crop_mode = static_cast<std::int32_t>(LensCalibCropMode::RECTANGLE);
  } else if (crop_profile_valid && crop.CropMode == LF_CROP_CIRCLE) {
    runtime.crop_mode = static_cast<std::int32_t>(LensCalibCropMode::CIRCLE);
  }
  std::memcpy(runtime.crop_bounds, crop.Crop, sizeof(runtime.crop_bounds));

  runtime.resolved_scale = 1.0f;
  if (settings.use_user_scale_ && IsFinitePositive(settings.user_scale_)) {
    runtime.resolved_scale = settings.user_scale_;
  } else if (settings.auto_scale_) {
    runtime.resolved_scale = ResolveScaleFromModifier(
        lens, meta.focal_length_mm_, crop_factor, 4096, 4096, runtime.apply_distortion != 0,
        runtime.apply_tca != 0, runtime.apply_projection != 0,
        LensTypeToLensfun(target_projection));
  }
  if (!IsFinitePositive(runtime.resolved_scale)) {
    runtime.resolved_scale = 1.0f;
  }

  runtime.fast_path_vignetting_only =
      (runtime.apply_vignetting != 0 && runtime.apply_distortion == 0 && runtime.apply_tca == 0 &&
       runtime.apply_projection == 0 && runtime.apply_crop == 0)
          ? 1
          : 0;
  runtime.fast_path_distortion_only =
      (runtime.apply_vignetting == 0 && runtime.apply_distortion != 0 && runtime.apply_tca == 0 &&
       runtime.apply_projection == 0 && runtime.apply_crop == 0)
          ? 1
          : 0;

  result.matched_lens_maker_ = lens->Maker ? lens->Maker : "";
  result.matched_lens_model_ = lens->Model ? lens->Model : "";

  ALCEDO_LOG_DEBUG(Lens) << "LensCalibrationResolver: lens=\"" << meta.lens_model_ << "\""
                         << " profile=\"" << result.matched_lens_maker_ << " / "
                         << result.matched_lens_model_ << "\""
                         << " focal=" << meta.focal_length_mm_ << " crop_factor=" << crop_factor
                         << " apply_vignetting=" << runtime.apply_vignetting
                         << " apply_distortion=" << runtime.apply_distortion
                         << " apply_tca=" << runtime.apply_tca
                         << " apply_projection=" << runtime.apply_projection
                         << " apply_crop=" << runtime.apply_crop
                         << " dng_geometry_wins=" << dng_geometry_wins
                         << " crop_mode=" << runtime.crop_mode
                         << " resolved_scale=" << runtime.resolved_scale << " crop_bounds=["
                         << runtime.crop_bounds[0] << ", " << runtime.crop_bounds[1] << ", "
                         << runtime.crop_bounds[2] << ", " << runtime.crop_bounds[3] << "]";

  result.params_ = runtime;
  result.status_ =
      (runtime.apply_vignetting == 0 && runtime.apply_distortion == 0 && runtime.apply_tca == 0 &&
       runtime.apply_projection == 0 && runtime.apply_crop == 0)
          ? LensProfileStatus::NoCorrectionApplies
          : LensProfileStatus::Resolved;
  return result;
}

void LensCalibrationResolver::BindImageExtent(const LensInputMeta&         matched_meta,
                                              const std::filesystem::path& database_path,
                                              LensCalibGpuParams& params, int width, int height) {
  params.src_width  = static_cast<std::int32_t>(width);
  params.src_height = static_cast<std::int32_t>(height);
  params.dst_width  = static_cast<std::int32_t>(width);
  params.dst_height = static_cast<std::int32_t>(height);
  if (params.use_auto_scale != 0 && params.use_user_scale == 0) {
    params.resolved_scale =
        ResolveScaleForImageSize(matched_meta, database_path, params, width, height);
  }

  const double extent_w = width >= 2 ? static_cast<double>(width - 1) : 1.0;
  const double extent_h = height >= 2 ? static_cast<double>(height - 1) : 1.0;
  const double crop_factor =
      IsFinitePositive(params.camera_crop_factor) ? params.camera_crop_factor : 1.0;
  const double real_focal = IsFinitePositive(params.real_focal_mm) ? params.real_focal_mm : 1.0;
  const double norm_scale = static_cast<double>(kFullFrameDiagonalMm) / crop_factor /
                            std::hypot(extent_w + 1.0, extent_h + 1.0) / real_focal;
  params.norm_scale = static_cast<float>(norm_scale);
  params.norm_unscale =
      std::fabs(norm_scale) > kEpsilon ? static_cast<float>(1.0 / norm_scale) : 1.0f;
  const double min_size = std::min(extent_w, extent_h);
  params.center_x =
      static_cast<float>((extent_w * 0.5 + min_size * 0.5 * params.lens_center_x) * norm_scale);
  params.center_y =
      static_cast<float>((extent_h * 0.5 + min_size * 0.5 * params.lens_center_y) * norm_scale);
}

void LensCalibrationResolver::PreloadDatabase(const std::filesystem::path& database_path) {
  (void)GetLensfunDb(ResolveDbRootPath(database_path));
}

auto LensCalibrationResolver::ShotLensProfile(const LensInputMeta&         meta,
                                              const std::filesystem::path& database_path)
    -> std::optional<LensProfileName> {
  if (meta.lens_model_.empty() && meta.cam_model_.empty()) {
    return std::nullopt;
  }
  lfDatabase* db = GetLensfunDb(ResolveDbRootPath(database_path));
  if (!db) {
    return std::nullopt;
  }
  const lfLens* lens = FindShotLens(db, meta).lens_;
  if (!lens) {
    return std::nullopt;
  }
  return LensProfileName{lens->Maker ? lens->Maker : "", lens->Model ? lens->Model : ""};
}

auto LensCalibrationResolver::DefaultDatabasePath() -> std::filesystem::path {
  return ResolveDefaultDbPath();
}

}  // namespace alcedo
