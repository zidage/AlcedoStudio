//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "decoders/dng_warp_rectilinear.hpp"
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
  /// True when @c lens_maker_ and @c lens_model_ are a catalog selection, which names a Lensfun
  /// profile exactly. The search then accepts a profile of any mount: the selection names the lens
  /// on the camera, for example an adapted lens. False for the RAW lens name, which matches only
  /// profiles whose mount fits the camera.
  bool        lens_selected_     = false;
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
  /// No positive focal length, or no lens model on a camera without a built-in lens.
  MissingLensIdentity,
  DatabaseUnavailable,  ///< No Lensfun database directory could be found or loaded.
  LensNotInDatabase,    ///< The database has no profile for the lens.
};

/**
 * @brief Size-independent result of @ref LensCalibrationResolver::ResolveProfile.
 *
 * @c params_, @c matched_meta_, and the matched profile names are set for @c Resolved and
 * @c NoCorrectionApplies. The image size fields of @c params_ stay zero until
 * @ref LensCalibrationResolver::BindImageExtent runs.
 */
struct LensProfileResolution {
  LensProfileStatus  status_             = LensProfileStatus::MissingLensIdentity;
  LensCalibGpuParams params_             = {};
  LensInputMeta      matched_meta_       = {};
  /// Default maker and model strings of the matched Lensfun lens profile.
  std::string        matched_lens_maker_ = {};
  std::string        matched_lens_model_ = {};
};

/// Maker and model strings of one Lensfun lens profile.
struct LensProfileName {
  std::string maker_ = {};
  std::string model_ = {};
};

/**
 * @brief Resolves Lensfun coefficients into @ref LensCalibGpuParams for the lens kernels.
 *
 * The lens name selects the profile. A fixed-lens camera (a Lensfun camera whose mount name starts
 * with a lower-case letter) whose RAW names no lens, or a lens that no profile matches, uses the
 * profile of its built-in lens.
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
   * RAW lens name; a selected profile of another mount than the camera applies too
   * (@ref LensInputMeta::lens_selected_). The result is sized for @p extent.
   *
   * A disabled setting, missing lens identity, missing or unloadable Lensfun database, unknown
   * lens, or profile with no requested correction returns @c std::nullopt. Lens correction is
   * then skipped for that image; no other backend or algorithm runs.
   *
   * @param dng_warp What the DNG OpcodeList3 warp of the image corrects
   *        (@ref dng::CorrectionsOf). A warp that corrects distortion turns off distortion, TCA,
   *        projection, and crop; a warp that corrects only lateral chromatic aberration turns off
   *        TCA. Vignetting can apply in both cases.
   * @throws std::runtime_error when the setting is enabled and @p extent is empty.
   */
  [[nodiscard]] static auto Resolve(const DevelopPayload&         develop,
                                    const RawRuntimeColorContext& context, Extent2D extent,
                                    dng::WarpCorrections dng_warp)
      -> std::optional<LensCalibGpuParams>;

  /**
   * @brief Match a Lensfun profile and interpolate its coefficients for @p meta.
   *
   * Loads the database named by @p settings on first use or when the directory changes. Prints
   * one line to standard output for each matched profile. The image size fields of the result
   * are zero; call @ref BindImageExtent before a kernel uses the result. @p dng_warp turns off
   * corrections as in @ref Resolve.
   */
  [[nodiscard]] static auto ResolveProfile(const LensInputMeta&          meta,
                                           const LensCorrectionSettings& settings,
                                           dng::WarpCorrections dng_warp) -> LensProfileResolution;

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

  /// Load the database named by @p database_path into the cache, so that later calls find it
  /// loaded. Blocks while it loads; a failed load leaves the cache empty.
  static void               PreloadDatabase(const std::filesystem::path& database_path);

  /**
   * @brief Profile that @ref ResolveProfile matches for the lens and camera in @p meta.
   *
   * This is the profile of Lens Calibration Auto: the lens name decides, and a fixed-lens camera
   * without a matching lens name gets the profile of its built-in lens. The focal length and
   * aperture order profiles with equal match scores. Returns @c std::nullopt when no profile
   * matches or the database does not load. Loads the database named by @p database_path on first
   * use, as @ref ResolveProfile does.
   */
  [[nodiscard]] static auto ShotLensProfile(const LensInputMeta&         meta,
                                            const std::filesystem::path& database_path)
      -> std::optional<LensProfileName>;

  /// First existing Lensfun database directory of the default search, or an empty path.
  [[nodiscard]] static auto DefaultDatabasePath() -> std::filesystem::path;
};

}  // namespace alcedo
