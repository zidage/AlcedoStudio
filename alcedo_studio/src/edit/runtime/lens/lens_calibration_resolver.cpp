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
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
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

auto ScoreLensCandidate(const lfLens* lens, const LensInputMeta& input) -> int {
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

auto FindBestLens(const lfDatabase* db, const lfCamera* camera, const LensInputMeta& input)
    -> const lfLens* {
  if (!db || input.lens_model_.empty()) {
    return nullptr;
  }

  const char*    lens_maker = input.lens_maker_.empty() ? nullptr : input.lens_maker_.c_str();
  constexpr int  flags      = LF_SEARCH_SORT_AND_UNIQUIFY | LF_SEARCH_LOOSE;
  const lfLens** lenses =
      lf_db_find_lenses(db, camera, lens_maker, input.lens_model_.c_str(), flags);
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

}  // namespace

auto LensCalibrationResolver::Resolve(const DevelopPayload&         develop,
                                      const RawRuntimeColorContext& context, Extent2D extent,
                                      bool dng_geometry_applied)
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

  const auto profile           = ResolveProfile(meta, settings, dng_geometry_applied);
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
                                             bool dng_geometry_applied) -> LensProfileResolution {
  LensProfileResolution result;
  if (meta.lens_model_.empty() || !IsFinitePositive(meta.focal_length_mm_)) {
    result.status_ = LensProfileStatus::MissingLensIdentity;
    return result;
  }

  const auto  db_root = ResolveDbRootPath(settings.database_path_);
  lfDatabase* db      = GetLensfunDb(db_root);
  if (!db) {
    result.status_ = LensProfileStatus::DatabaseUnavailable;
    return result;
  }

  const lfCamera* camera = FindBestCamera(db, meta.cam_maker_, meta.cam_model_);
  const lfLens*   lens   = FindBestLens(db, camera, meta);
  if (!lens) {
    result.status_ = LensProfileStatus::LensNotInDatabase;
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
  const bool         dng_geometry_wins = dng_geometry_applied;

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
  runtime.apply_tca = (!dng_geometry_wins && settings.apply_tca_ && tca_ok &&
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

  std::cout << "LensCalibrationResolver: lens=\"" << meta.lens_model_ << "\""
            << " focal=" << meta.focal_length_mm_ << " crop_factor=" << crop_factor
            << " apply_distortion=" << runtime.apply_distortion
            << " apply_tca=" << runtime.apply_tca
            << " apply_projection=" << runtime.apply_projection
            << " apply_crop=" << runtime.apply_crop << " dng_geometry_wins=" << dng_geometry_wins
            << " crop_mode=" << runtime.crop_mode << " resolved_scale=" << runtime.resolved_scale
            << " crop_bounds=[" << runtime.crop_bounds[0] << ", " << runtime.crop_bounds[1] << ", "
            << runtime.crop_bounds[2] << ", " << runtime.crop_bounds[3] << "]" << std::endl;

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

auto LensCalibrationResolver::DefaultDatabasePath() -> std::filesystem::path {
  return ResolveDefaultDbPath();
}

}  // namespace alcedo
