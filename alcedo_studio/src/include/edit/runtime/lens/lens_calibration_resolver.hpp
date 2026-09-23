//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "decoders/processor/raw_color_context.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/runtime/lens/lens_calib_runtime.hpp"

namespace alcedo {

/**
 * @brief Camera and lens identity plus shooting values that select and interpolate one Lensfun
 *        profile.
 */
struct LensInputMeta {
  std::string cam_maker_         = {};
  std::string cam_model_         = {};
  std::string lens_maker_        = {};
  std::string lens_model_        = {};
  float       focal_length_mm_   = 0.0f;
  float       aperture_f_number_ = 0.0f;
  float       distance_m_        = 0.0f;
  float       focal_35mm_mm_     = 0.0f;
  float       crop_factor_hint_  = 0.0f;
};

/**
 * @brief Correction choices that the resolver applies to a matched Lensfun profile.
 */
struct LensCorrectionSettings {
  /// Lensfun XML directory. Empty, or a directory that does not exist, selects the default search.
  std::filesystem::path database_path_         = {};
  bool                  apply_vignetting_      = true;
  bool                  apply_distortion_      = true;
  bool                  apply_tca_             = true;
  bool                  apply_crop_            = true;
  bool                  auto_scale_            = true;
  bool                  use_user_scale_        = false;
  float                 user_scale_            = 1.0f;
  bool                  projection_enabled_    = false;
  std::string           target_projection_     = "unknown";
  bool                  low_precision_preview_ = false;
};

/// Outcome of one profile resolution. Only @c Resolved produces a correction to apply.
enum class LensProfileStatus {
  Resolved,             ///< At least one correction applies.
  NoCorrectionApplies,  ///< The profile matched, but no requested correction is available.
  MissingLensIdentity,  ///< No lens model or no positive focal length.
  DatabaseUnavailable,  ///< No Lensfun database directory could be found or loaded.
  LensNotInDatabase,    ///< The database has no profile for the lens.
};

/**
 * @brief Size-independent result of @ref LensCalibrationResolver::ResolveProfile.
 *
 * @c params_ and @c matched_meta_ are set for @c Resolved and @c NoCorrectionApplies. The image
 * size fields of @c params_ stay zero until @ref LensCalibrationResolver::BindImageExtent runs.
 */
struct LensProfileResolution {
  LensProfileStatus  status_       = LensProfileStatus::MissingLensIdentity;
  LensCalibGpuParams params_       = {};
  LensInputMeta      matched_meta_ = {};
};

/**
 * @brief Resolves Lensfun coefficients into @ref LensCalibGpuParams for the lens kernels.
 *
 * The resolver owns the process-wide Lensfun database cache. The cache holds one loaded database
 * directory and reloads it when a call names a different directory; a mutex serializes the load.
 * The resolver changes no other state and has no per-image state.
 */
class LensCalibrationResolver {
 public:
  /**
   * @brief Resolve the lens correction for one Develop output.
   *
   * Uses the prepared RAW camera, lens, focal length, aperture, and focus distance. A non-empty
   * @c DevelopPayload::lens_maker or @c lens_model is a user catalog selection and replaces the
   * RAW lens name. The result is sized for @p extent.
   *
   * A disabled setting, missing lens identity, missing or unloadable Lensfun database, unknown
   * lens, or profile with no requested correction returns @c std::nullopt. Lens correction is
   * then skipped for that image; no other backend or algorithm runs.
   *
   * @param dng_geometry_applied True when the DNG OpcodeList3 warp already corrected geometry.
   *        Distortion, TCA, projection, and crop are then off; vignetting can still apply.
   * @throws std::runtime_error when the setting is enabled and @p extent is empty.
   */
  [[nodiscard]] static auto Resolve(const DevelopPayload&         develop,
                                    const RawRuntimeColorContext& context, Extent2D extent,
                                    bool dng_geometry_applied) -> std::optional<LensCalibGpuParams>;

  /**
   * @brief Match a Lensfun profile and interpolate its coefficients for @p meta.
   *
   * Loads the database named by @p settings on first use or when the directory changes. Prints
   * one line to standard output for each matched profile. The image size fields of the result
   * are zero; call @ref BindImageExtent before a kernel uses the result.
   */
  [[nodiscard]] static auto ResolveProfile(const LensInputMeta&          meta,
                                           const LensCorrectionSettings& settings,
                                           bool dng_geometry_applied) -> LensProfileResolution;

  /**
   * @brief Size a resolved profile for a @p width x @p height image.
   *
   * Sets the source and destination size, the Lensfun coordinate normalization, and the optical
   * center. When automatic scale is active, it also recomputes @c resolved_scale for this size
   * from the lens in @p matched_meta. If that lens is no longer found, @c resolved_scale keeps its
   * value.
   */
  static void               BindImageExtent(const LensInputMeta&         matched_meta,
                                            const std::filesystem::path& database_path,
                                            LensCalibGpuParams& params, int width, int height);

  /// First existing Lensfun database directory of the default search, or an empty path.
  [[nodiscard]] static auto DefaultDatabasePath() -> std::filesystem::path;
};

}  // namespace alcedo
