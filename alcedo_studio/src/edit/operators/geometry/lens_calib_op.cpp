//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/geometry/lens_calib_op.hpp"

#include <lensfun/lensfun.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#ifdef HAVE_CUDA
#include "edit/operators/geometry/cuda_lens_calib_ops.hpp"
#endif
#ifdef HAVE_METAL
#include "edit/operators/geometry/metal_lens_calib.hpp"
#endif
#ifdef HAVE_OPENCL
#include "edit/operators/geometry/opencl_lens_calib_ops.hpp"
#endif
#include "utils/string/convert.hpp"

#if defined(LF_VERSION) && LF_VERSION >= 0x00030400
#define ALCEDO_LENSFUN_MODERN_C_API 1
#else
#define ALCEDO_LENSFUN_MODERN_C_API 0
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

struct LensfunDbState {
  std::mutex                                    mutex_;
  std::unique_ptr<lfDatabase, LensfunDbDeleter> db_    = {};
  std::filesystem::path                         root_  = {};
  bool                                          valid_ = false;
};

auto CreateLensfunDatabase() -> lfDatabase* {
#if ALCEDO_LENSFUN_MODERN_C_API
  return lf_db_new();
#else
  return lf_db_create();
#endif
}

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

auto TrimWhitespace(std::string text) -> std::string {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.erase(text.begin());
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.pop_back();
  }
  return text;
}

auto StripWrappedQuotes(std::string text) -> std::string {
  text = TrimWhitespace(std::move(text));
  while (text.size() >= 2) {
    const char first = text.front();
    const char last  = text.back();
    if (!((first == '"' && last == '"') || (first == '\'' && last == '\''))) {
      break;
    }
    text = TrimWhitespace(text.substr(1, text.size() - 2));
  }
  return text;
}

auto PathToDisplayString(const std::filesystem::path& path) -> std::string {
  return conv::ToBytes(path.wstring());
}

auto LoadPortableDbXmls(lfDatabase* db, const std::filesystem::path& root) -> bool {
  if (!db || root.empty() || !std::filesystem::exists(root) ||
      !std::filesystem::is_directory(root)) {
    return false;
  }
#if ALCEDO_LENSFUN_MODERN_C_API
  return lf_db_load_directory(db, root.string().c_str()) != 0;
#else
  return lf_db_load_path(db, root.string().c_str()) == LF_NO_ERROR;
#endif
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

  auto db = std::unique_ptr<lfDatabase, LensfunDbDeleter>(CreateLensfunDatabase());
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

#if ALCEDO_LENSFUN_MODERN_C_API
void RescaleDistortionTerms(lfLensCalibDistortion* distortion, float real_focal_mm,
                            const lfLens* lens) {
  if (!distortion || !lens || !IsFinitePositive(real_focal_mm)) {
    return;
  }
  const float hugin_scale_in_mm =
      HuginScaleInMillimeters(lens->CropFactor, lens->AspectRatio);
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

void RescaleTcaTerms(lfLensCalibTCA* tca, float real_focal_mm, const lfLens* lens) {
  if (!tca || !lens || !IsFinitePositive(real_focal_mm)) {
    return;
  }
  const float hugin_scale_in_mm =
      HuginScaleInMillimeters(lens->CropFactor, lens->AspectRatio);
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
#else
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
#endif

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

auto FindLensCandidates(const lfDatabase* db, const lfCamera* camera, const char* lens_maker,
                        const char* lens_model, int flags) -> const lfLens** {
#if ALCEDO_LENSFUN_MODERN_C_API
  return lf_db_find_lenses_hd(db, camera, lens_maker, lens_model, flags);
#else
  return lf_db_find_lenses(db, camera, lens_maker, lens_model, flags);
#endif
}

auto ScoreLensCandidate(const lfLens* lens, const InputMeta& input) -> int {
  if (!lens) {
    return std::numeric_limits<int>::min();
  }
  int score = lens->Score;
  if (IsFinitePositive(input.focal_length_mm_) && IsFinitePositive(lens->MinFocal) &&
      IsFinitePositive(lens->MaxFocal)) {
    if (input.focal_length_mm_ >= lens->MinFocal - 0.2f &&
        input.focal_length_mm_ <= lens->MaxFocal + 0.2f) {
      score += 2000;
    } else {
      score -= 2000;
    }
  }
  if (IsFinitePositive(input.aperture_f_number_) && IsFinitePositive(lens->MinAperture) &&
      IsFinitePositive(lens->MaxAperture)) {
    if (input.aperture_f_number_ >= lens->MinAperture - 0.1f &&
        input.aperture_f_number_ <= lens->MaxAperture + 0.1f) {
      score += 200;
    } else {
      score -= 200;
    }
  }
  return score;
}

auto FindBestLens(const lfDatabase* db, const lfCamera* camera, const InputMeta& input)
    -> const lfLens* {
  if (!db || input.lens_model_.empty()) {
    return nullptr;
  }

  const char*    lens_maker = input.lens_maker_.empty() ? nullptr : input.lens_maker_.c_str();
  constexpr int  flags      = LF_SEARCH_SORT_AND_UNIQUIFY | LF_SEARCH_LOOSE;
  const lfLens** lenses =
      FindLensCandidates(db, camera, lens_maker, input.lens_model_.c_str(), flags);
  if (!lenses) {
    return nullptr;
  }

  const lfLens* best       = nullptr;
  int           best_score = std::numeric_limits<int>::min();
  for (int i = 0; lenses[i] != nullptr; ++i) {
    const int score = ScoreLensCandidate(lenses[i], input);
    if (score > best_score) {
      best_score = score;
      best       = lenses[i];
    }
  }

  lf_free((void*)lenses);
  return best;
}

auto ResolveScaleFromModifier(const lfLens* lens, float focal_mm, float crop_factor, int width,
                              int height, bool apply_distortion, bool apply_tca,
                              bool apply_projection, lfLensType target_projection) -> float {
  if (!lens || !IsFinitePositive(focal_mm) || !IsFinitePositive(crop_factor)) {
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
#if ALCEDO_LENSFUN_MODERN_C_API
  auto modifier = std::unique_ptr<lfModifier, LensfunModifierDeleter>(
      lf_modifier_new(lens, crop_factor, width, height));
  if (!modifier) {
    return 1.0f;
  }
  (void)lf_modifier_initialize(modifier.get(), lens, LF_PF_F32, focal_mm, 1.0f,
                               kDefaultFarDistanceM, 0.0f, resolved_target, flags, false);
#else
  auto modifier = std::unique_ptr<lfModifier, LensfunModifierDeleter>(
      lf_modifier_create(lens, focal_mm, crop_factor, width, height, LF_PF_F32, false));
  if (!modifier) {
    return 1.0f;
  }
  if (apply_distortion) {
    (void)lf_modifier_enable_distortion_correction(modifier.get());
  }
  if (apply_tca) {
    (void)lf_modifier_enable_tca_correction(modifier.get());
  }
  if (apply_projection) {
    (void)lf_modifier_enable_projection_transform(modifier.get(), resolved_target);
  }
#endif

  const float scale = lf_modifier_get_auto_scale(modifier.get(), false);
  if (!IsFinitePositive(scale)) {
    return 1.0f;
  }
  return scale;
}

auto ResolveScaleForImageSize(const InputMeta& meta, const std::filesystem::path& db_path,
                              const LensCalibGpuParams& params, int width, int height) -> float {
  const auto  db_root = ResolveDbRootPath(db_path);
  lfDatabase* db      = GetLensfunDb(db_root);
  if (!db) {
    return params.resolved_scale;
  }

  const lfCamera* camera = FindBestCamera(db, meta.cam_maker_, meta.cam_model_);
  const lfLens*   lens   = FindBestLens(db, camera, meta);
  if (!lens) {
    return params.resolved_scale;
  }

  return ResolveScaleFromModifier(
      lens, meta.focal_length_mm_, params.camera_crop_factor, width, height,
      params.apply_distortion != 0, params.apply_tca != 0, params.apply_projection != 0,
      LensTypeToLensfun(static_cast<LensCalibProjectionType>(params.target_projection)));
}

void HashCombine(std::uint64_t& seed, std::uint64_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
}

auto HashFloatBits(float value) -> std::uint64_t {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

}  // namespace

LensCalibOp::LensCalibOp(const nlohmann::json& params) { SetParams(params); }

auto LensCalibOp::ProjectionFromString(const std::string& text) -> LensCalibProjectionType {
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

auto LensCalibOp::ProjectionToString(LensCalibProjectionType projection) -> std::string {
  switch (projection) {
    case LensCalibProjectionType::RECTILINEAR:
      return "rectilinear";
    case LensCalibProjectionType::FISHEYE:
      return "fisheye";
    case LensCalibProjectionType::PANORAMIC:
      return "panoramic";
    case LensCalibProjectionType::EQUIRECTANGULAR:
      return "equirectangular";
    case LensCalibProjectionType::FISHEYE_ORTHOGRAPHIC:
      return "fisheye_orthographic";
    case LensCalibProjectionType::FISHEYE_STEREOGRAPHIC:
      return "fisheye_stereographic";
    case LensCalibProjectionType::FISHEYE_EQUISOLID:
      return "fisheye_equisolid";
    case LensCalibProjectionType::FISHEYE_THOBY:
      return "fisheye_thoby";
    case LensCalibProjectionType::UNKNOWN:
    default:
      return "unknown";
  }
}

void LensCalibOp::Apply(std::shared_ptr<ImageBuffer>) {
  // CPU builds keep lens calibration as a no-op. GPU backends are implemented
  // in ApplyGPU().
}

void LensCalibOp::ApplyGPU(std::shared_ptr<ImageBuffer> input) {
  if (!enabled_ || !has_resolved_params_ || !input) {
    return;
  }
  // if (resolved_params_.src_width <= 0 || resolved_params_.src_height <= 0) {
  //   std::cout << "LensCalibOp: Invalid source dimensions (" << resolved_params_.src_width << "x"
  //             << resolved_params_.src_height << "), skipping lens calibration." << std::endl;
  //   return;
  // }
  if (!input->gpu_data_valid_) {
    input->SyncToGPU();
  }

#if !defined(HAVE_CUDA) && !defined(HAVE_METAL) && !defined(HAVE_OPENCL)
  throw std::runtime_error("LensCalibOp::ApplyGPU requires HAVE_CUDA, HAVE_METAL, or HAVE_OPENCL");
#else

#ifdef HAVE_OPENCL
  if (input->GetGPUBackend() == GpuBackendKind::OpenCL) {
    auto& gpu = input->GetOpenClImage();
    if (gpu.Empty()) {
      std::cout << "LensCalibOp: Input GPU data is empty, skipping lens calibration." << std::endl;
      return;
    }

    resolved_params_.src_width  = static_cast<std::int32_t>(gpu.Width());
    resolved_params_.src_height = static_cast<std::int32_t>(gpu.Height());
    resolved_params_.dst_width  = static_cast<std::int32_t>(gpu.Width());
    resolved_params_.dst_height = static_cast<std::int32_t>(gpu.Height());
    if (resolved_params_.use_auto_scale != 0 && resolved_params_.use_user_scale == 0) {
      resolved_params_.resolved_scale = ResolveScaleForImageSize(
          resolved_input_meta_, lens_profile_db_path_, resolved_params_, gpu.Width(), gpu.Height());
    }

    const double width       = (gpu.Width() >= 2) ? static_cast<double>(gpu.Width() - 1) : 1.0;
    const double height      = (gpu.Height() >= 2) ? static_cast<double>(gpu.Height() - 1) : 1.0;
    const double crop_factor = IsFinitePositive(resolved_params_.camera_crop_factor)
                                   ? resolved_params_.camera_crop_factor
                                   : 1.0;
    const double real_focal =
        IsFinitePositive(resolved_params_.real_focal_mm) ? resolved_params_.real_focal_mm : 1.0;

    const double norm_scale = static_cast<double>(kFullFrameDiagonalMm) / crop_factor /
                              std::hypot(width + 1.0, height + 1.0) / real_focal;
    resolved_params_.norm_scale = static_cast<float>(norm_scale);
    resolved_params_.norm_unscale =
        (std::fabs(norm_scale) > kEpsilon) ? static_cast<float>(1.0 / norm_scale) : 1.0f;
    const double min_size     = std::min(width, height);
    resolved_params_.center_x = static_cast<float>(
        (width * 0.5 + min_size * 0.5 * resolved_params_.lens_center_x) * norm_scale);
    resolved_params_.center_y = static_cast<float>(
        (height * 0.5 + min_size * 0.5 * resolved_params_.lens_center_y) * norm_scale);

    OpenCL::Geometry::ApplyLensCalibration(gpu, resolved_params_);
    input->gpu_data_valid_ = true;
    return;
  }
#endif

#ifdef HAVE_CUDA
  if (input->GetGPUBackend() == GpuBackendKind::CUDA) {
    auto& gpu = input->GetCUDAImage();
    if (gpu.empty()) {
      std::cout << "LensCalibOp: Input GPU data is empty, skipping lens calibration." << std::endl;
      return;
    }

    resolved_params_.src_width  = gpu.cols;
    resolved_params_.src_height = gpu.rows;
    resolved_params_.dst_width  = gpu.cols;
    resolved_params_.dst_height = gpu.rows;
    if (resolved_params_.use_auto_scale != 0 && resolved_params_.use_user_scale == 0) {
      resolved_params_.resolved_scale = ResolveScaleForImageSize(
          resolved_input_meta_, lens_profile_db_path_, resolved_params_, gpu.cols, gpu.rows);
    }

    const double width       = (gpu.cols >= 2) ? static_cast<double>(gpu.cols - 1) : 1.0;
    const double height      = (gpu.rows >= 2) ? static_cast<double>(gpu.rows - 1) : 1.0;
    const double crop_factor = IsFinitePositive(resolved_params_.camera_crop_factor)
                                   ? resolved_params_.camera_crop_factor
                                   : 1.0;
    const double real_focal =
        IsFinitePositive(resolved_params_.real_focal_mm) ? resolved_params_.real_focal_mm : 1.0;

    const double norm_scale = static_cast<double>(kFullFrameDiagonalMm) / crop_factor /
                              std::hypot(width + 1.0, height + 1.0) / real_focal;
    resolved_params_.norm_scale = static_cast<float>(norm_scale);
    resolved_params_.norm_unscale =
        (std::fabs(norm_scale) > kEpsilon) ? static_cast<float>(1.0 / norm_scale) : 1.0f;
    const double min_size     = std::min(width, height);
    resolved_params_.center_x = static_cast<float>(
        (width * 0.5 + min_size * 0.5 * resolved_params_.lens_center_x) * norm_scale);
    resolved_params_.center_y = static_cast<float>(
        (height * 0.5 + min_size * 0.5 * resolved_params_.lens_center_y) * norm_scale);

    CUDA::ApplyLensCalibration(gpu, resolved_params_);
    input->gpu_data_valid_ = true;
    return;
  }
#endif

#ifdef HAVE_METAL
  if (input->GetGPUBackend() == GpuBackendKind::Metal) {
    auto& gpu = input->GetMetalImage();
    if (gpu.Empty()) {
      std::cout << "LensCalibOp: Input GPU data is empty, skipping lens calibration." << std::endl;
      return;
    }

    resolved_params_.src_width  = static_cast<std::int32_t>(gpu.Width());
    resolved_params_.src_height = static_cast<std::int32_t>(gpu.Height());
    resolved_params_.dst_width  = static_cast<std::int32_t>(gpu.Width());
    resolved_params_.dst_height = static_cast<std::int32_t>(gpu.Height());
    if (resolved_params_.use_auto_scale != 0 && resolved_params_.use_user_scale == 0) {
      resolved_params_.resolved_scale =
          ResolveScaleForImageSize(resolved_input_meta_, lens_profile_db_path_, resolved_params_,
                                   static_cast<int>(gpu.Width()), static_cast<int>(gpu.Height()));
    }

    const double width       = (gpu.Width() >= 2U) ? static_cast<double>(gpu.Width() - 1U) : 1.0;
    const double height      = (gpu.Height() >= 2U) ? static_cast<double>(gpu.Height() - 1U) : 1.0;
    const double crop_factor = IsFinitePositive(resolved_params_.camera_crop_factor)
                                   ? resolved_params_.camera_crop_factor
                                   : 1.0;
    const double real_focal =
        IsFinitePositive(resolved_params_.real_focal_mm) ? resolved_params_.real_focal_mm : 1.0;

    const double norm_scale = static_cast<double>(kFullFrameDiagonalMm) / crop_factor /
                              std::hypot(width + 1.0, height + 1.0) / real_focal;
    resolved_params_.norm_scale = static_cast<float>(norm_scale);
    resolved_params_.norm_unscale =
        (std::fabs(norm_scale) > kEpsilon) ? static_cast<float>(1.0 / norm_scale) : 1.0f;
    const double min_size     = std::min(width, height);
    resolved_params_.center_x = static_cast<float>(
        (width * 0.5 + min_size * 0.5 * resolved_params_.lens_center_x) * norm_scale);
    resolved_params_.center_y = static_cast<float>(
        (height * 0.5 + min_size * 0.5 * resolved_params_.lens_center_y) * norm_scale);

    metal::ApplyLensCalibration(gpu, resolved_params_);
    input->gpu_data_valid_ = true;
    return;
  }
#endif

  throw std::runtime_error("LensCalibOp::ApplyGPU: active GPU backend is not supported");
#endif
}

auto LensCalibOp::GetParams() const -> nlohmann::json {
  nlohmann::json inner;
  inner["enabled"]               = enabled_;
  inner["apply_vignetting"]      = apply_vignetting_;
  inner["apply_distortion"]      = apply_distortion_;
  inner["apply_tca"]             = apply_tca_;
  inner["apply_crop"]            = apply_crop_;
  inner["auto_scale"]            = auto_scale_;
  inner["use_user_scale"]        = use_user_scale_;
  inner["user_scale"]            = user_scale_;
  inner["projection_enabled"]    = projection_enabled_;
  inner["target_projection"]     = target_projection_;
  inner["low_precision_preview"] = low_precision_preview_;

  inner["cam_maker"]             = input_meta_.cam_maker_;
  inner["cam_model"]             = input_meta_.cam_model_;
  inner["lens_maker"]            = input_meta_.lens_maker_;
  inner["lens_model"]            = input_meta_.lens_model_;
  inner["focal_length_mm"]       = input_meta_.focal_length_mm_;
  inner["aperture_f_number"]     = input_meta_.aperture_f_number_;
  inner["distance_m"]            = input_meta_.distance_m_;
  inner["focal_35mm_mm"]         = input_meta_.focal_35mm_mm_;
  inner["crop_factor_hint"]      = input_meta_.crop_factor_hint_;
  inner["lens_profile_db_path"]  = conv::ToBytes(lens_profile_db_path_.wstring());
  return {{std::string(script_name_), std::move(inner)}};
}

void LensCalibOp::SetParams(const nlohmann::json& params) {
  nlohmann::json inner =
      params.contains(script_name_) ? params[script_name_] : nlohmann::json::object();
  enabled_                       = inner.value("enabled", enabled_);
  apply_vignetting_              = inner.value("apply_vignetting", apply_vignetting_);
  apply_distortion_              = inner.value("apply_distortion", apply_distortion_);
  apply_tca_                     = inner.value("apply_tca", apply_tca_);
  apply_crop_                    = inner.value("apply_crop", apply_crop_);
  auto_scale_                    = inner.value("auto_scale", auto_scale_);
  use_user_scale_                = inner.value("use_user_scale", use_user_scale_);
  user_scale_                    = inner.value("user_scale", user_scale_);
  projection_enabled_            = inner.value("projection_enabled", projection_enabled_);
  target_projection_             = inner.value("target_projection", target_projection_);
  low_precision_preview_         = inner.value("low_precision_preview", low_precision_preview_);

  // Image-local EXIF/runtime metadata: keep existing values when the transfer
  // package omits them (SanitizeLens strips these keys on capture).
  input_meta_.cam_maker_         = inner.value("cam_maker", input_meta_.cam_maker_);
  input_meta_.cam_model_         = inner.value("cam_model", input_meta_.cam_model_);
  input_meta_.lens_maker_        = inner.value("lens_maker", input_meta_.lens_maker_);
  input_meta_.lens_model_        = inner.value("lens_model", input_meta_.lens_model_);
  input_meta_.focal_length_mm_   = inner.value("focal_length_mm", input_meta_.focal_length_mm_);
  input_meta_.aperture_f_number_ = inner.value("aperture_f_number", input_meta_.aperture_f_number_);
  input_meta_.distance_m_        = inner.value("distance_m", input_meta_.distance_m_);
  input_meta_.focal_35mm_mm_     = inner.value("focal_35mm_mm", input_meta_.focal_35mm_mm_);
  input_meta_.crop_factor_hint_  = inner.value("crop_factor_hint", input_meta_.crop_factor_hint_);

  if (inner.contains("lens_profile_db_path")) {
    std::string configured = inner.value("lens_profile_db_path", std::string{});
    configured             = StripWrappedQuotes(std::move(configured));
    lens_profile_db_path_  = configured.empty() ? std::filesystem::path{}
                                                : std::filesystem::path(conv::FromBytes(configured));
  }
  if (lens_profile_db_path_.empty()) {
    lens_profile_db_path_ = ResolveDefaultDbPath();
  }
  has_resolved_params_ = false;
  resolved_input_meta_ = {};
}

namespace {

auto LensCalibInner(const nlohmann::json& params) -> nlohmann::json {
  if (params.contains("lens_calib") && params["lens_calib"].is_object()) {
    return params["lens_calib"];
  }
  return nlohmann::json::object();
}

auto IsLensImageLocalKey(std::string_view key) -> bool {
  return key == "cam_maker" || key == "cam_model" || key == "lens_maker" || key == "lens_model" ||
         key == "focal_length_mm" || key == "aperture_f_number" || key == "distance_m" ||
         key == "focal_35mm_mm" || key == "crop_factor_hint" || key == "lens_profile_db_path";
}

auto LensPortableParams(const nlohmann::json& params) -> nlohmann::json {
  const auto inner = LensCalibInner(params);
  nlohmann::json portable = nlohmann::json::object();
  for (const auto& [key, value] : inner.items()) {
    if (!IsLensImageLocalKey(key)) {
      portable[key] = value;
    }
  }
  return portable;
}

}  // namespace

auto LensCalibOp::DetectMergeConflict(const nlohmann::json& current,
                                      const nlohmann::json& incoming) const -> bool {
  // Only portable correction intent participates in merge conflicts. EXIF /
  // profile paths stay with the target image and must not force a conflict.
  return LensPortableParams(current) != LensPortableParams(incoming);
}

auto LensCalibOp::MergeParams(const nlohmann::json& current, const nlohmann::json& incoming,
                              OperatorMergeChoice choice) const -> nlohmann::json {
  if (choice == OperatorMergeChoice::kKeepCurrent) {
    return current;
  }

  nlohmann::json result =
      current.is_object() ? current : nlohmann::json{{std::string(script_name_), nlohmann::json::object()}};
  if (!result.contains(std::string(script_name_)) || !result[std::string(script_name_)].is_object()) {
    result[std::string(script_name_)] = nlohmann::json::object();
  }
  auto& out = result[std::string(script_name_)];
  const auto inc = LensCalibInner(incoming);
  for (const auto& [key, value] : inc.items()) {
    if (!IsLensImageLocalKey(key)) {
      out[key] = value;
    }
  }
  return result;
}

auto LensCalibOp::BuildRuntimeCacheKey(const OperatorParams& params) const -> uint64_t {
  std::uint64_t key = 0xcbf29ce484222325ULL;
  HashCombine(key, static_cast<std::uint64_t>(enabled_));
  HashCombine(key, static_cast<std::uint64_t>(apply_vignetting_));
  HashCombine(key, static_cast<std::uint64_t>(apply_distortion_));
  HashCombine(key, static_cast<std::uint64_t>(apply_tca_));
  HashCombine(key, static_cast<std::uint64_t>(apply_crop_));
  HashCombine(key, static_cast<std::uint64_t>(auto_scale_));
  HashCombine(key, static_cast<std::uint64_t>(use_user_scale_));
  HashCombine(key, HashFloatBits(user_scale_));
  HashCombine(key, static_cast<std::uint64_t>(projection_enabled_));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(target_projection_)));
  HashCombine(key, static_cast<std::uint64_t>(low_precision_preview_));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(params.raw_camera_make_)));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(params.raw_camera_model_)));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(params.raw_lens_make_)));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(params.raw_lens_model_)));
  HashCombine(key, HashFloatBits(params.raw_lens_focal_mm_));
  HashCombine(key, HashFloatBits(params.raw_lens_aperture_f_));
  HashCombine(key, HashFloatBits(params.raw_lens_focus_distance_m_));
  HashCombine(key, HashFloatBits(params.raw_lens_focal_35mm_));
  HashCombine(key, HashFloatBits(params.raw_lens_crop_factor_hint_));
  HashCombine(key, static_cast<std::uint64_t>(params.raw_dng_warp_rectilinear_present_));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(input_meta_.cam_maker_)));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(input_meta_.cam_model_)));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(input_meta_.lens_maker_)));
  HashCombine(key, static_cast<std::uint64_t>(std::hash<std::string>{}(input_meta_.lens_model_)));
  HashCombine(key, HashFloatBits(input_meta_.focal_length_mm_));
  HashCombine(key, HashFloatBits(input_meta_.aperture_f_number_));
  HashCombine(key, HashFloatBits(input_meta_.distance_m_));
  HashCombine(key, HashFloatBits(input_meta_.focal_35mm_mm_));
  HashCombine(key, HashFloatBits(input_meta_.crop_factor_hint_));
  HashCombine(key,
              static_cast<std::uint64_t>(std::hash<std::string>{}(lens_profile_db_path_.string())));
  return key;
}

static void PrintResolvedParams(const OperatorParams& params) {
  std::cout << "Current raw parameters: " << std::endl;
  std::cout << "  Camera Make: " << params.raw_camera_make_ << std::endl;
  std::cout << "  Camera Model: " << params.raw_camera_model_ << std::endl;
  std::cout << "  Lens Make: " << params.raw_lens_make_ << std::endl;
  std::cout << "  Lens Model: " << params.raw_lens_model_ << std::endl;
  std::cout << "  Focal Length (mm): " << params.raw_lens_focal_mm_ << std::endl;
  std::cout << "  Aperture (f-number): " << params.raw_lens_aperture_f_ << std::endl;
  std::cout << "  Focus Distance (m): " << params.raw_lens_focus_distance_m_ << std::endl;
  std::cout << "  Crop Factor Hint: " << params.raw_lens_crop_factor_hint_ << std::endl;
}

void LensCalibOp::ResolveRuntime(OperatorParams& params) const {
  if (!params.raw_runtime_valid_) {
    params.lens_calib_runtime_valid_  = false;
    params.lens_calib_runtime_failed_ = true;
    params.lens_calib_runtime_dirty_  = false;
    has_resolved_params_              = false;
    std::cout << "Warning: Raw decoding parameters are not valid. Lens calibration will be skipped."
              << std::endl;
    PrintResolvedParams(params);
    return;
  }

  const auto cache_key = BuildRuntimeCacheKey(params);
  if (!params.lens_calib_runtime_dirty_ && params.lens_calib_cache_key_valid_ &&
      params.lens_calib_cache_key_ == cache_key) {
    resolved_params_     = params.lens_calib_runtime_params_;
    has_resolved_params_ = params.lens_calib_runtime_valid_;
    return;
  }

  params.lens_calib_cache_key_valid_ = true;
  params.lens_calib_cache_key_       = cache_key;

  InputMeta meta;
  meta.cam_maker_         = params.raw_camera_make_;
  meta.cam_model_         = params.raw_camera_model_;
  meta.lens_maker_        = params.raw_lens_make_;
  meta.lens_model_        = params.raw_lens_model_;
  meta.focal_length_mm_   = params.raw_lens_focal_mm_;
  meta.aperture_f_number_ = params.raw_lens_aperture_f_;
  meta.distance_m_        = params.raw_lens_focus_distance_m_;
  meta.focal_35mm_mm_     = params.raw_lens_focal_35mm_;
  meta.crop_factor_hint_  = params.raw_lens_crop_factor_hint_;

  if (!input_meta_.cam_maker_.empty()) {
    meta.cam_maker_ = input_meta_.cam_maker_;
  }
  if (!input_meta_.cam_model_.empty()) {
    meta.cam_model_ = input_meta_.cam_model_;
  }
  if (!input_meta_.lens_maker_.empty()) {
    meta.lens_maker_ = input_meta_.lens_maker_;
  }
  if (!input_meta_.lens_model_.empty()) {
    meta.lens_model_ = input_meta_.lens_model_;
  }
  if (IsFinitePositive(input_meta_.focal_length_mm_)) {
    meta.focal_length_mm_ = input_meta_.focal_length_mm_;
  }
  if (IsFinitePositive(input_meta_.aperture_f_number_)) {
    meta.aperture_f_number_ = input_meta_.aperture_f_number_;
  }
  if (IsFinitePositive(input_meta_.distance_m_)) {
    meta.distance_m_ = input_meta_.distance_m_;
  }
  if (IsFinitePositive(input_meta_.focal_35mm_mm_)) {
    meta.focal_35mm_mm_ = input_meta_.focal_35mm_mm_;
  }
  if (IsFinitePositive(input_meta_.crop_factor_hint_)) {
    meta.crop_factor_hint_ = input_meta_.crop_factor_hint_;
  }
  // Keep input_meta_ as user overrides only. Do not persist auto-resolved metadata back into
  // operator params; otherwise UI-side param patching can churn every frame.

  if (meta.lens_model_.empty() || !IsFinitePositive(meta.focal_length_mm_)) {
    params.lens_calib_runtime_valid_  = false;
    params.lens_calib_runtime_failed_ = true;
    params.lens_calib_runtime_dirty_  = false;
    has_resolved_params_              = false;
    return;
  }

  const auto  db_root = ResolveDbRootPath(lens_profile_db_path_);
  lfDatabase* db      = GetLensfunDb(db_root);
  if (!db) {
    params.lens_calib_runtime_valid_  = false;
    params.lens_calib_runtime_failed_ = true;
    params.lens_calib_runtime_dirty_  = false;
    has_resolved_params_              = false;
    return;
  }

  const lfCamera* camera = FindBestCamera(db, meta.cam_maker_, meta.cam_model_);
  const lfLens*   lens   = FindBestLens(db, camera, meta);
  if (!lens) {
    params.lens_calib_runtime_valid_  = false;
    params.lens_calib_runtime_failed_ = true;
    params.lens_calib_runtime_dirty_  = false;
    has_resolved_params_              = false;
    return;
  }

  float crop_factor = (camera && IsFinitePositive(camera->CropFactor)) ? camera->CropFactor : 0.0f;
  if (!IsFinitePositive(crop_factor)) {
    crop_factor = meta.crop_factor_hint_;
  }
  if (!IsFinitePositive(crop_factor)) {
    crop_factor = 1.0f;
  }

  lfLensCalibDistortion distortion{};
#if ALCEDO_LENSFUN_MODERN_C_API
  const bool distortion_ok =
      lf_lens_interpolate_distortion(lens, meta.focal_length_mm_, &distortion) != 0;
  float real_focal_mm = meta.focal_length_mm_;
  lfLensCalibRealFocal real_focal{};
  if (lf_lens_interpolate_real_focal(lens, meta.focal_length_mm_, &real_focal) != 0 &&
      IsFinitePositive(real_focal.RealFocal)) {
    real_focal_mm = real_focal.RealFocal;
  }
  if (distortion_ok) {
    RescaleDistortionTerms(&distortion, real_focal_mm, lens);
  }

  lfLensCalibTCA tca{};
  const bool tca_ok = lf_lens_interpolate_tca(lens, meta.focal_length_mm_, &tca) != 0;
  if (tca_ok) {
    RescaleTcaTerms(&tca, real_focal_mm, lens);
  }
#else
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
#endif

  lfLensCalibVignetting vignette{};
  const float           safe_aperture =
      IsFinitePositive(meta.aperture_f_number_) ? meta.aperture_f_number_ : 0.0f;
  const float safe_distance =
      IsFinitePositive(meta.distance_m_) ? meta.distance_m_ : kDefaultFarDistanceM;
#if ALCEDO_LENSFUN_MODERN_C_API
  const bool vignette_ok =
      IsFinitePositive(safe_aperture) &&
      lf_lens_interpolate_vignetting(lens, meta.focal_length_mm_, safe_aperture, safe_distance,
                                      &vignette) != 0;
#else
  const bool vignette_ok =
      IsFinitePositive(safe_aperture) &&
      lf_lens_interpolate_vignetting(lens, crop_factor, meta.focal_length_mm_, safe_aperture,
                                     safe_distance, &vignette) != 0;
#endif
  if (vignette_ok) {
    RescalePaVignettingTerms(&vignette, real_focal_mm, crop_factor);
  }

  lfLensCalibCrop crop{};
#if ALCEDO_LENSFUN_MODERN_C_API
  const bool crop_ok = lf_lens_interpolate_crop(lens, meta.focal_length_mm_, &crop) != 0;
#else
  const bool      crop_ok =
      lf_lens_interpolate_crop(lens, crop_factor, meta.focal_length_mm_, &crop) != 0;
#endif
  const bool         dng_geometry_wins = params.raw_dng_warp_rectilinear_present_;

  LensCalibGpuParams runtime{};
  resolved_input_meta_          = meta;
  runtime.src_width             = 0;
  runtime.src_height            = 0;
  runtime.nominal_focal_mm      = meta.focal_length_mm_;
  runtime.real_focal_mm         = real_focal_mm;
  runtime.camera_crop_factor    = crop_factor;
  runtime.user_scale            = user_scale_;
  runtime.use_user_scale        = use_user_scale_ ? 1 : 0;
  runtime.use_auto_scale        = (auto_scale_ && !use_user_scale_) ? 1 : 0;
  runtime.low_precision_preview = low_precision_preview_ ? 1 : 0;
  runtime.lens_center_x         = lens->CenterX;
  runtime.lens_center_y         = lens->CenterY;

  runtime.source_projection     = static_cast<std::int32_t>(LensTypeFromLensfun(lens->Type));
  const auto target_projection  = projection_enabled_ ? ProjectionFromString(target_projection_)
                                                      : LensCalibProjectionType::UNKNOWN;
  runtime.target_projection     = static_cast<std::int32_t>(target_projection);

  runtime.apply_distortion =
      (!dng_geometry_wins && apply_distortion_ && distortion_ok &&
       (distortion.Model == LF_DIST_MODEL_POLY3 || distortion.Model == LF_DIST_MODEL_POLY5 ||
        distortion.Model == LF_DIST_MODEL_PTLENS))
          ? 1
          : 0;
  runtime.apply_tca = (!dng_geometry_wins && apply_tca_ && tca_ok &&
                       (tca.Model == LF_TCA_MODEL_LINEAR || tca.Model == LF_TCA_MODEL_POLY3))
                          ? 1
                          : 0;
  runtime.apply_vignetting =
      (apply_vignetting_ && vignette_ok && vignette.Model == LF_VIGNETTING_MODEL_PA) ? 1 : 0;

  const bool projection_valid = target_projection != LensCalibProjectionType::UNKNOWN &&
                                target_projection != LensTypeFromLensfun(lens->Type);
  runtime.apply_projection =
      (!dng_geometry_wins && projection_enabled_ && projection_valid) ? 1 : 0;

  const bool crop_profile_valid =
      crop_ok && (crop.CropMode == LF_CROP_RECTANGLE || crop.CropMode == LF_CROP_CIRCLE);
  runtime.apply_crop        = (!dng_geometry_wins && apply_crop_ && crop_profile_valid) ? 1 : 0;
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
  if (use_user_scale_ && IsFinitePositive(user_scale_)) {
    runtime.resolved_scale = user_scale_;
  } else if (auto_scale_) {
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

  std::cout << "LensCalibOp runtime: lens=\"" << meta.lens_model_ << "\""
            << " focal=" << meta.focal_length_mm_ << " crop_factor=" << crop_factor
            << " apply_distortion=" << runtime.apply_distortion
            << " apply_tca=" << runtime.apply_tca
            << " apply_projection=" << runtime.apply_projection
            << " apply_crop=" << runtime.apply_crop << " dng_geometry_wins=" << dng_geometry_wins
            << " crop_mode=" << runtime.crop_mode << " resolved_scale=" << runtime.resolved_scale
            << " crop_bounds=[" << runtime.crop_bounds[0] << ", " << runtime.crop_bounds[1] << ", "
            << runtime.crop_bounds[2] << ", " << runtime.crop_bounds[3] << "]" << std::endl;

  if (runtime.apply_vignetting == 0 && runtime.apply_distortion == 0 && runtime.apply_tca == 0 &&
      runtime.apply_projection == 0 && runtime.apply_crop == 0) {
    params.lens_calib_runtime_valid_  = false;
    params.lens_calib_runtime_failed_ = false;
    params.lens_calib_runtime_dirty_  = false;
    has_resolved_params_              = false;
    params.lens_calib_runtime_params_ = runtime;
    return;
  }

  params.lens_calib_runtime_valid_  = true;
  params.lens_calib_runtime_failed_ = false;
  params.lens_calib_runtime_dirty_  = false;
  params.lens_calib_runtime_params_ = runtime;
  resolved_params_                  = runtime;
  has_resolved_params_              = true;
}

void LensCalibOp::SetGlobalParams(OperatorParams& params) const {
  params.lens_calib_enabled_ = enabled_;
  if (!enabled_) {
    params.lens_calib_runtime_valid_  = false;
    params.lens_calib_runtime_failed_ = false;
    params.lens_calib_runtime_dirty_  = false;
    has_resolved_params_              = false;
    return;
  }
  ResolveRuntime(params);
}

void LensCalibOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  if (params.lens_calib_enabled_ == enable) {
    return;  // No state change; avoid unnecessary dirty marking.
  }
  params.lens_calib_enabled_       = enable;
  params.lens_calib_runtime_dirty_ = true;
  has_resolved_params_             = false;
}

}  // namespace alcedo
