//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "edit/graph/develop_raster_input.hpp"
#include "edit/graph/i_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"
#include "image/dcp_camera_profile.hpp"
#include "image/dng_color_profile.hpp"

namespace alcedo {

/**
 * @brief Import-time camera colour matrices stored on the Develop node.
 *
 * ColorMatrix/ForwardMatrix/AsShotNeutral/illuminant CCT are copied from the
 * image at import. CameraColor interpolates these fields; it does not parse RAW
 * or look up the CameraMatrices database at render time. cam_mul is only used
 * to derive as-shot neutral when AsShotNeutral is absent.
 *
 * The DNG profile is import-bound runtime data. JSON stores only its fingerprint
 * (`dng_profile_fingerprint`); the pipeline service binds the profile tables from
 * the source file before the document renders.
 */
struct DevelopCameraProfile {
  DngColorProfileRef    dng_profile;
  bool                  color_matrices_valid = false;
  std::array<double, 9> color_matrix_1{};
  std::array<double, 9> color_matrix_2{};
  bool                  forward_matrices_valid = false;
  std::array<double, 9> forward_matrix_1{};
  std::array<double, 9> forward_matrix_2{};
  bool                  as_shot_neutral_valid = false;
  std::array<double, 3> as_shot_neutral{};
  bool                  calibration_illuminants_valid = false;
  double                color_matrix_1_cct            = 2856.0;
  double                color_matrix_2_cct            = 6504.0;
  std::array<float, 3>  cam_mul{1.0f, 1.0f, 1.0f};
};

inline auto operator==(const DevelopCameraProfile& a, const DevelopCameraProfile& b) -> bool {
  return a.dng_profile == b.dng_profile && a.color_matrices_valid == b.color_matrices_valid &&
         a.color_matrix_1 == b.color_matrix_1 && a.color_matrix_2 == b.color_matrix_2 &&
         a.forward_matrices_valid == b.forward_matrices_valid &&
         a.forward_matrix_1 == b.forward_matrix_1 && a.forward_matrix_2 == b.forward_matrix_2 &&
         a.as_shot_neutral_valid == b.as_shot_neutral_valid &&
         a.as_shot_neutral == b.as_shot_neutral &&
         a.calibration_illuminants_valid == b.calibration_illuminants_valid &&
         a.color_matrix_1_cct == b.color_matrix_1_cct &&
         a.color_matrix_2_cct == b.color_matrix_2_cct && a.cam_mul == b.cam_mul;
}

inline auto operator!=(const DevelopCameraProfile& a, const DevelopCameraProfile& b) -> bool {
  return !(a == b);
}

/**
 * @brief Reference from the Develop data to a DCP file of the user DCP folder.
 *
 * Project data stores only @ref camera_model, @ref profile_name, @ref fingerprint, and
 * @ref profile_tone (Develop JSON key `camera_profile_file`). @ref profile is runtime data: the
 * owner of a loaded document binds it from the DCP library before the document renders. A reference
 * is bound when @ref profile is set, and unbound when it was read from project data and no binding
 * step resolved it. Rendering code must fail for an unbound reference; it never renders it as the
 * import profile.
 *
 * Equality compares the four stored values only, so a bound and an unbound reference to the same
 * file content are equal. @ref SameProfile compares the profile identity without
 * @ref profile_tone.
 */
struct DevelopCameraProfileFile {
  std::string         camera_model;      ///< UniqueCameraModel of the DCP file.
  std::string         profile_name;      ///< ProfileName of the DCP file.
  std::uint64_t       fingerprint  = 0;  ///< DcpCameraProfile::fingerprint of the file content.
  /// The profile tone stage of the display transform applies the `ProfileToneCurve` of the
  /// profile (dcp_profile_tone_curve_study.md, section 6). Key `profile_tone`, written only as
  /// `false`, so a reference made before the key keeps its bytes. The camera pass does not read it.
  bool                profile_tone = true;
  DcpCameraProfilePtr profile;  ///< Bound profile; null for an unbound reference.
  /// For a DNG source: the tables of @ref profile merged with the source values (AnalogBalance,
  /// BaselineExposure, CameraCalibration; plan phase C6). Runtime data like @ref profile; null
  /// for a source without an embedded DNG profile, which uses the tables of @ref profile.
  DngColorProfilePtr  source_tables;
  /// Fingerprint of the DNG profile of the source that @ref source_tables was merged with.
  std::uint64_t       source_fingerprint = 0;

  [[nodiscard]] auto  IsBound() const -> bool { return profile != nullptr; }
  /// True when the runtime values of the two references are the same objects.
  [[nodiscard]] auto  SameBinding(const DevelopCameraProfileFile& other) const -> bool {
    return profile == other.profile && source_tables == other.source_tables &&
           source_fingerprint == other.source_fingerprint;
  }
  /// True when both references store the same profile: identity and fingerprint, without
  /// @ref profile_tone.
  [[nodiscard]] auto SameProfile(const DevelopCameraProfileFile& other) const -> bool {
    return camera_model == other.camera_model && profile_name == other.profile_name &&
           fingerprint == other.fingerprint;
  }
};

inline auto operator==(const DevelopCameraProfileFile& a, const DevelopCameraProfileFile& b)
    -> bool {
  return a.SameProfile(b) && a.profile_tone == b.profile_tone;
}

/// True when both are absent, or both store the same profile (@ref DevelopCameraProfileFile::
/// SameProfile). The camera pass reads the profile and not `profile_tone`.
[[nodiscard]] inline auto SameCameraProfile(const std::optional<DevelopCameraProfileFile>& a,
                                            const std::optional<DevelopCameraProfileFile>& b)
    -> bool {
  if (a.has_value() != b.has_value()) return false;
  return !a.has_value() || a->SameProfile(*b);
}

/// The `profile_tone` value of @p reference: false without a reference.
[[nodiscard]] inline auto ProfileToneSelected(
    const std::optional<DevelopCameraProfileFile>& reference) -> bool {
  return reference.has_value() && reference->profile_tone;
}

/**
 * @brief True when the source tables of @p reference belong to the DNG profile @p source of the
 *        image (plan phase C6).
 *
 * A referenced @p source needs source tables merged with a profile of its fingerprint; an empty
 * @p source needs none, so the tables of the DCP profile apply. An unbound @p source is compared
 * by its fingerprint: the merged tables already hold the source values.
 */
[[nodiscard]] inline auto DcpSourceTablesMatchSource(const DevelopCameraProfileFile& reference,
                                                     const DngColorProfileRef& source) -> bool {
  if (!source.IsReferenced()) return reference.source_tables == nullptr;
  return reference.source_tables != nullptr &&
         reference.source_fingerprint == *source.Fingerprint();
}

/**
 * @brief Bound reference to @p profile: identity and fingerprint come from the profile itself.
 * @throws std::invalid_argument when @p profile is null.
 */
[[nodiscard]] auto MakeDevelopCameraProfileFile(DcpCameraProfilePtr profile)
    -> DevelopCameraProfileFile;

/// Persisted form: `{"camera_model", "profile_name", "fingerprint"}`, and `"profile_tone": false`
/// when the stage is off. The profile is not written.
[[nodiscard]] auto DevelopCameraProfileFileToJson(const DevelopCameraProfileFile& reference)
    -> nlohmann::json;

/**
 * @brief Read a persisted reference. The result is unbound.
 * @throws std::runtime_error when @p json is not an object with two non-empty strings and a
 *         fingerprint text of 16 lowercase hexadecimal digits, or when `profile_tone` is present
 *         and not a boolean.
 */
[[nodiscard]] auto DevelopCameraProfileFileFromJson(const nlohmann::json& json)
    -> DevelopCameraProfileFile;

struct DevelopPayload {
  std::string          demosaic_method        = "default";
  bool                 highlights_reconstruct = true;
  bool                 use_camera_wb          = true;
  float                user_wb                = 7600.0f;
  std::string          wb_mode                = "as_shot";
  float                custom_cct             = 6500.0f;
  float                custom_tint            = 0.0f;
  float                as_shot_cct            = 6500.0f;
  float                as_shot_tint           = 0.0f;
  DevelopCameraProfile camera_profile{};
  /// DCP profile that replaces the matrices and tables of @ref camera_profile in the render.
  /// Absent means the import profile applies; the JSON key is then absent too, so the JSON and root
  /// id of a document without a selection stay as before. @ref camera_profile keeps the import
  /// values while a reference is set.
  std::optional<DevelopCameraProfileFile> camera_profile_file;
  bool                 lens_enabled         = false;
  bool                 apply_vignetting     = true;
  bool                 apply_distortion     = true;
  bool                 apply_tca            = true;
  bool                 apply_crop           = true;
  bool                 auto_scale           = true;
  bool                 use_user_scale       = false;
  float                user_scale           = 1.0f;
  bool                 projection_enabled   = false;
  std::string          target_projection    = "unknown";
  std::string          lens_profile_db_path = "src/config/lens_calib";
  std::string          lens_maker;
  std::string          lens_model;
  /// Present only for a raster (JPEG, PNG, TIFF, OpenEXR) document; absent means RAW. A RAW
  /// document never serializes the `input` key, so its JSON and root id stay as before.
  std::optional<DevelopRasterInput> input;
};

inline auto operator==(const DevelopPayload& a, const DevelopPayload& b) -> bool {
  return a.demosaic_method == b.demosaic_method &&
         a.highlights_reconstruct == b.highlights_reconstruct &&
         a.use_camera_wb == b.use_camera_wb && a.user_wb == b.user_wb && a.wb_mode == b.wb_mode &&
         a.custom_cct == b.custom_cct && a.custom_tint == b.custom_tint &&
         a.as_shot_cct == b.as_shot_cct && a.as_shot_tint == b.as_shot_tint &&
         a.camera_profile == b.camera_profile && a.camera_profile_file == b.camera_profile_file &&
         a.lens_enabled == b.lens_enabled && a.apply_vignetting == b.apply_vignetting &&
         a.apply_distortion == b.apply_distortion && a.apply_tca == b.apply_tca &&
         a.apply_crop == b.apply_crop && a.auto_scale == b.auto_scale &&
         a.use_user_scale == b.use_user_scale && a.user_scale == b.user_scale &&
         a.projection_enabled == b.projection_enabled &&
         a.target_projection == b.target_projection &&
         a.lens_profile_db_path == b.lens_profile_db_path && a.lens_maker == b.lens_maker &&
         a.lens_model == b.lens_model && a.input == b.input;
}

inline auto operator!=(const DevelopPayload& a, const DevelopPayload& b) -> bool {
  return !(a == b);
}

enum class DevelopDirty : std::uint32_t {
  None         = 0,
  Demosaic     = 1U << 0,
  Highlights   = 1U << 1,
  WhiteBalance = 1U << 2,
  Lens         = 1U << 3,
  /// Raster input description or profile override. Changes the raster linearize step, so it
  /// invalidates from the sensor result.
  Input        = 1U << 4,
  /// `camera_profile_file.profile_tone` alone. The camera pass does not read it, so it invalidates
  /// the display result only (dcp_profile_tone_curve_study.md, section 6.3).
  ProfileTone  = 1U << 5,
  All          = Demosaic | Highlights | WhiteBalance | Lens | Input | ProfileTone,
};

/**
 * @brief Focused RAW decode update. Omitted fields retain their values.
 */
struct DevelopRawDecodeUpdate {
  std::optional<std::string> demosaic_method;
  std::optional<bool>        highlights_reconstruct;
  std::optional<bool>        use_camera_wb;
  std::optional<float>       user_wb;
};

/**
 * @brief Focused color-temperature update. Omitted fields retain their values.
 */
struct DevelopColorTemperatureUpdate {
  std::optional<std::string> wb_mode;
  std::optional<float>       custom_cct;
  std::optional<float>       custom_tint;
  std::optional<float>       as_shot_cct;
  std::optional<float>       as_shot_tint;
};

/**
 * @brief Focused lens and projection update. Omitted fields retain their values.
 *
 * Empty @p lens_maker / @p lens_model mean Auto: Develop uses the prepared RAW
 * lens identity. A non-empty pair is a user catalog selection.
 */
struct DevelopLensCalibrationUpdate {
  std::optional<bool>        lens_enabled;
  std::optional<bool>        apply_vignetting;
  std::optional<bool>        apply_distortion;
  std::optional<bool>        apply_tca;
  std::optional<bool>        apply_crop;
  std::optional<bool>        auto_scale;
  std::optional<bool>        use_user_scale;
  std::optional<float>       user_scale;
  std::optional<bool>        projection_enabled;
  std::optional<std::string> target_projection;
  std::optional<std::string> lens_profile_db_path;
  std::optional<std::string> lens_maker;
  std::optional<std::string> lens_model;
};

/**
 * @brief Raster input profile override (`input_profile` field, decision D5).
 */
struct DevelopInputProfileUpdate {
  std::string profile_override;
};

/**
 * @brief DCP profile selection (`camera_profile_file` field).
 *
 * - @ref profile set: a profile of the DCP library. It gives a bound reference with its own
 *   identity and fingerprint.
 * - @ref unresolved set: a stored reference that the DCP library cannot resolve (history replay,
 *   project load). It gives an unbound reference with the stored values, so the render fails with
 *   `UnboundCameraProfileFile` and no other profile applies.
 * - Both empty: clears the reference, so the import profile applies again.
 *
 * Both set is an invalid update. With @ref profile, a document with an embedded DNG profile also
 * needs @ref source_tables: the tables of @ref profile merged with that DNG profile, whose
 * fingerprint is @ref source_fingerprint (DcpProfileLibrary::SourceTables, plan phase C6).
 * @ref profile_tone is the `profile_tone` value of the reference that @ref profile gives; an
 * @ref unresolved reference keeps its own value.
 *
 * A change of `profile_tone` alone marks DevelopDirty::ProfileTone and not
 * DevelopDirty::WhiteBalance, so the camera pass result stays valid.
 */
struct DevelopCameraProfileFileUpdate {
  DcpCameraProfilePtr                     profile;
  std::optional<DevelopCameraProfileFile> unresolved;
  DngColorProfilePtr                      source_tables;
  std::uint64_t                           source_fingerprint = 0;
  bool                                    profile_tone       = true;
};

/**
 * @brief Develop endpoint parameters. LibRaw unpack stays outside the graph.
 */
class DevelopParamsModel final
    : public OperatorModelBase<DevelopParamsModel, DevelopPayload, DevelopDirty> {
 public:
  static auto        TypeId() -> const OperatorTypeId& { return type_ids::DevelopNode(); }

  [[nodiscard]] auto IsDefault() const -> bool override;

  /**
   * @brief Read individual Develop fields through the owning Model lock.
   */
  [[nodiscard]] auto DemosaicMethod() const -> std::string;
  [[nodiscard]] auto HighlightsReconstruct() const -> bool;
  [[nodiscard]] auto UseCameraWhiteBalance() const -> bool;
  [[nodiscard]] auto UserWhiteBalance() const -> float;
  [[nodiscard]] auto WhiteBalanceMode() const -> std::string;
  [[nodiscard]] auto CustomCct() const -> float;
  [[nodiscard]] auto CustomTint() const -> float;
  [[nodiscard]] auto AsShotCct() const -> float;
  [[nodiscard]] auto AsShotTint() const -> float;
  [[nodiscard]] auto LensEnabled() const -> bool;
  [[nodiscard]] auto ApplyVignetting() const -> bool;
  [[nodiscard]] auto ApplyDistortion() const -> bool;
  [[nodiscard]] auto ApplyTca() const -> bool;
  [[nodiscard]] auto ApplyCrop() const -> bool;
  [[nodiscard]] auto AutoScale() const -> bool;
  [[nodiscard]] auto UseUserScale() const -> bool;
  [[nodiscard]] auto UserScale() const -> float;
  [[nodiscard]] auto ProjectionEnabled() const -> bool;
  [[nodiscard]] auto TargetProjection() const -> std::string;
  [[nodiscard]] auto LensProfileDbPath() const -> std::string;
  [[nodiscard]] auto LensMaker() const -> std::string;
  [[nodiscard]] auto LensModel() const -> std::string;

  /// Raster input object, or std::nullopt for a RAW document.
  [[nodiscard]] auto RasterInput() const -> std::optional<DevelopRasterInput>;

  /**
   * @brief Apply RAW decode fields atomically and mark only changed field groups dirty.
   * @throws std::logic_error for a raster document, which has no RAW decode; the payload is
   *         unchanged.
   */
  void               ApplyRawDecodeUpdate(DevelopRawDecodeUpdate update);

  /**
   * @brief Apply color-temperature fields atomically and mark only changed fields dirty.
   * @throws std::logic_error for a raster document, which has no RAW white balance (use the
   *         Color Grade CAT02 white balance); the payload is unchanged.
   */
  void               ApplyColorTemperatureUpdate(DevelopColorTemperatureUpdate update);

  /**
   * @brief Set the raster input profile override and mark Input dirty when it changes.
   * @throws std::logic_error for a RAW document; std::invalid_argument for an unknown value.
   */
  void               ApplyInputProfileUpdate(DevelopInputProfileUpdate update);

  /**
   * @brief Apply lens/projection fields atomically and avoid dirtying equal values.
   */
  void               ApplyLensCalibrationUpdate(DevelopLensCalibrationUpdate update);

  /**
   * @brief Take every field except white balance, with its stamp, from @p current, and keep the
   *        white balance of this Model.
   *
   * Afterwards demosaic, highlight reconstruction, lens/projection, the camera profile, and the
   * DCP profile reference with its bound profile equal @p current with the same field stamps, so a
   * renderer that applied @p current reuses its sensor result. The white-balance fields
   * (`use_camera_wb`, `user_wb`, `wb_mode`, custom and as-shot CCT/tint) keep this Model's values.
   * When they differ from @p current they get one new stamp; when they are equal they keep the
   * stamp of @p current. Runs under both Model locks.
   *
   * Used for documents of the editor comparison, which render a selected state with the current
   * sensor settings. Never call it on a Model of the working document.
   * @throws std::invalid_argument when @p current is this Model; this Model is unchanged.
   */
  void               UseSensorSettingsFrom(const DevelopParamsModel& current);

  /**
   * @brief Take every field, with its stamp, from @p current, including the white balance.
   *
   * Afterwards this Model equals @p current, so a renderer that applied @p current reuses its
   * sensor result. Runs under both Model locks.
   *
   * Used for the AI Mask analysis document, which renders the import root look with the current
   * Develop settings. Never call it on a Model of the working document.
   * @throws std::invalid_argument when @p current is this Model; this Model is unchanged.
   */
  void               UseAllSettingsFrom(const DevelopParamsModel& current);

  /**
   * @brief Set or clear the DCP profile reference atomically.
   *
   * Solves `as_shot_cct` and `as_shot_tint` again from the effective matrices (see
   * EffectiveCameraProfile); `custom_cct` and `custom_tint` keep their numbers. Marks WhiteBalance
   * dirty when the reference, its bound profile, or a solved value changes. When the as-shot solve
   * fails (no usable import neutral), the as-shot values keep their numbers, as at import.
   * @throws std::logic_error for a raster document, which has no camera profile;
   *         std::invalid_argument when the update sets both members, when its unresolved
   *         reference has a bound profile, when it has source tables without a profile, or when
   *         its source tables do not belong to the DNG profile of the document (absent for a
   *         document with one, present for a document without one, or another fingerprint).
   *         The payload is unchanged in all cases.
   */
  void               ApplyCameraProfileFileUpdate(DevelopCameraProfileFileUpdate update);

  /// Current DCP profile reference, or std::nullopt when the import profile applies.
  [[nodiscard]] auto CameraProfileFile() const -> std::optional<DevelopCameraProfileFile>;

  /// Current DNG profile reference, read under the Model lock.
  [[nodiscard]] auto DngProfile() const -> DngColorProfileRef;

  /**
   * @brief Bind the DNG profile tables loaded from the source file.
   *
   * The reference becomes a bound reference to @p profile. When @p profile has another
   * fingerprint than the stored reference (the source file changed), the new profile wins and
   * the stored fingerprint changes with it. Marks WhiteBalance dirty only when the bound profile
   * changes.
   *
   * @param profile Profile from DngColorProfileCache; must not be null.
   * @throws std::invalid_argument when @p profile is null.
   */
  void               BindDngColorProfile(DngColorProfilePtr profile);

  /// JSON writes the DNG profile fingerprint and the DCP reference only. LoadJson keeps a bound
  /// profile whose stored values match the JSON; any other value gives an unbound reference.
  [[nodiscard]] auto ToJson() const -> nlohmann::json override;
  void               LoadJson(const nlohmann::json& json) override;

  [[nodiscard]] auto Params() const -> DevelopPayload { return PayloadCopy(); }
  /// Replace the payload and mark the changed field groups dirty. A payload equal to the current
  /// one (same DNG profile fingerprint, same DCP reference) still installs its bound profiles.
  void               ReplaceParams(DevelopPayload payload);
};

/**
 * @brief Develop root node. No image input; one scene-image output.
 */
class DevelopNodeModel final : public INodeModel {
 public:
  explicit DevelopNodeModel(NodeId id);

  [[nodiscard]] auto Id() const -> const NodeId& override { return id_; }
  [[nodiscard]] auto Type() const -> const OperatorTypeId& override {
    return type_ids::DevelopNode();
  }
  [[nodiscard]] auto DisplayName() const -> std::string_view override { return "Develop"; }
  [[nodiscard]] auto InputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto OutputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto ToJson() const -> nlohmann::json override;
  /// Copies the Develop parameters; the bound DNG and DCP profiles stay shared (@ref
  /// INodeModel::Clone).
  [[nodiscard]] auto Clone() const -> std::shared_ptr<INodeModel> override;

  [[nodiscard]] auto Params() -> DevelopParamsModel& { return params_; }
  [[nodiscard]] auto Params() const -> const DevelopParamsModel& { return params_; }

  static auto        FromJson(const nlohmann::json& json) -> std::unique_ptr<DevelopNodeModel>;

 private:
  /// Used only by @ref Clone. Copy assignment is deleted.
  DevelopNodeModel(const DevelopNodeModel& other)            = default;
  DevelopNodeModel& operator=(const DevelopNodeModel& other) = delete;

  NodeId                        id_;
  DevelopParamsModel            params_;
  std::array<PortDescriptor, 1> outputs_;
};

}  // namespace alcedo
