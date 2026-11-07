//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "color/color_encoding_math.h"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/diffusion_filter_model.hpp"
#include "edit/graph/film_format.hpp"
#include "edit/graph/film_print_model.hpp"
#include "edit/graph/i_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

enum class DrtMethod : int {
  Aces20    = 0,
  OpenDrt   = 1,
  FilmPrint = 2,
};

enum class DrtColorSpace : int {
  Rec709  = 0,
  Rec2020 = 1,
  P3D65   = 2,
};

enum class DrtEotf : int {
  Linear        = 0,
  St2084        = 1,
  Hlg           = 2,
  Gamma26       = 3,
  Bt1886        = 4,
  Gamma22       = 5,
  Gamma18       = 6,
  /// sRGB (IEC 61966-2-1) curve of the color encoding catalog (not gamma 2.2).
  SrgbPiecewise = 7,
};

// The serialized DrtEotf values are the catalog transfer function ids (color_encoding_math.h).
static_assert(static_cast<int>(DrtEotf::Linear) == CE_TF_LINEAR);
static_assert(static_cast<int>(DrtEotf::St2084) == CE_TF_ST2084);
static_assert(static_cast<int>(DrtEotf::Hlg) == CE_TF_HLG);
static_assert(static_cast<int>(DrtEotf::Gamma26) == CE_TF_GAMMA_2_6);
static_assert(static_cast<int>(DrtEotf::Bt1886) == CE_TF_BT1886);
static_assert(static_cast<int>(DrtEotf::Gamma22) == CE_TF_GAMMA_2_2);
static_assert(static_cast<int>(DrtEotf::Gamma18) == CE_TF_GAMMA_1_8);
static_assert(static_cast<int>(DrtEotf::SrgbPiecewise) == CE_TF_SRGB);

struct OpenDrtDetailedParams {
  float tn_con       = 1.66f;
  float tn_sh        = 0.5f;
  float tn_toe       = 0.003f;
  float tn_off       = 0.005f;
  float tn_hcon      = 0.0f;
  float tn_hcon_pv   = 1.0f;
  float tn_hcon_st   = 4.0f;
  float tn_lcon      = 0.0f;
  float tn_lcon_w    = 0.5f;
  float cwp_lm       = 0.25f;
  float rs_sa        = 0.35f;
  float rs_rw        = 0.25f;
  float rs_bw        = 0.55f;
  float pt_lml       = 0.25f;
  float pt_lml_r     = 0.5f;
  float pt_lml_g     = 0.0f;
  float pt_lml_b     = 0.1f;
  float pt_lmh       = 0.25f;
  float pt_lmh_r     = 0.5f;
  float pt_lmh_b     = 0.0f;
  float ptl_c        = 0.06f;
  float ptl_m        = 0.08f;
  float ptl_y        = 0.06f;
  float ptm_low      = 0.4f;
  float ptm_low_rng  = 0.25f;
  float ptm_low_st   = 0.5f;
  float ptm_high     = -0.8f;
  float ptm_high_rng = 0.35f;
  float ptm_high_st  = 0.4f;
  float brl          = 0.0f;
  float brl_r        = -2.5f;
  float brl_g        = -1.5f;
  float brl_b        = -1.5f;
  float brl_rng      = 0.5f;
  float brl_st       = 0.35f;
  float brlp         = -0.5f;
  float brlp_r       = -1.25f;
  float brlp_g       = -1.25f;
  float brlp_b       = -0.25f;
  float hc_r         = 1.0f;
  float hc_r_rng     = 0.3f;
  float hs_r         = 0.6f;
  float hs_r_rng     = 0.6f;
  float hs_g         = 0.35f;
  float hs_g_rng     = 1.0f;
  float hs_b         = 0.66f;
  float hs_b_rng     = 1.0f;
  float hs_c         = 0.25f;
  float hs_c_rng     = 1.0f;
  float hs_m         = 0.0f;
  float hs_m_rng     = 1.0f;
  float hs_y         = 0.0f;
  float hs_y_rng     = 1.0f;
};

struct DrtPayload {
  DrtMethod             method                 = DrtMethod::OpenDrt;
  DrtColorSpace         encoding_space         = DrtColorSpace::Rec709;
  DrtEotf               encoding_eotf          = DrtEotf::Gamma22;
  DrtColorSpace         limiting_space         = DrtColorSpace::Rec709;
  float                 peak_luminance         = 100.0f;
  std::string           look_preset            = "standard";
  std::string           tonescale_preset       = "use_look_preset";
  std::string           creative_white         = "use_look_preset";
  float                 creative_white_limit   = 0.25f;
  float                 display_grey_luminance = 10.0f;
  float                 hdr_grey_boost         = 0.13f;
  float                 hdr_purity             = 0.5f;
  OpenDrtDetailedParams parameters{};
  /// Stored diffusion filter strength in [0, 1]. See @ref ResolveDiffusionFilterShape.
  float                 diffusion_strength = 0.0f;
  /// Written only when @ref method is @ref DrtMethod::FilmPrint.
  FilmPrintParams       film_print{};
  /// The film format of the frame. Every method reads it. Key `film_format`, written only when the
  /// value is not @ref FilmFormat::Film35mm, so a document made before the field keeps its bytes.
  FilmFormat            film_format = FilmFormat::Film35mm;
};

enum class DrtDirty : std::uint32_t {
  None         = 0,
  Method       = 1U << 0,
  Encoding     = 1U << 1,
  OpenDrt      = 1U << 2,
  Diffusion    = 1U << 3,
  /// The fields that change the exposure of the film print negative: the negative, the print
  /// material, the scene light, Light spread, and the film format, which sets the frame of the
  /// image plane.
  FilmExposure = 1U << 4,
  FilmHalation = 1U << 5,  ///< Halation of Film Print.
  FilmFlare    = 1U << 6,  ///< Enlarger flare of Film Print.
  /// The rendering settings of Negative Scan (auto colour, contrast, exposure, keys, black). They
  /// act only in FP6, so no canonical film print result reads them.
  FilmScan     = 1U << 7,
  /// The scanner settings of Negative Scan (band centres, width, light). They choose the scanner
  /// table, so the prescan of rule 'frame_auto_colour' (`FilmScanPrescanResultId`) reads them.
  FilmScanner  = 1U << 8,
  /// The film format. It sets the size and the strength of the grain on the film. A format change
  /// also marks @ref FilmExposure.
  FilmFormat   = 1U << 9,
  All          = Method | Encoding | OpenDrt | Diffusion | FilmExposure | FilmHalation | FilmFlare |
        FilmScan | FilmScanner | FilmFormat,
};

/**
 * @brief Focused DRT/ODT update. Omitted fields retain their current values.
 *
 * A detailed OpenDRT parameter object is accepted as one complete update because the
 * serialized schema exposes that object as a single editor-owned parameter group.
 */
struct DrtParameterUpdate {
  std::optional<DrtMethod>             method;
  std::optional<DrtColorSpace>         encoding_space;
  std::optional<DrtEotf>               encoding_eotf;
  std::optional<DrtColorSpace>         limiting_space;
  std::optional<float>                 peak_luminance;
  std::optional<std::string>           look_preset;
  std::optional<std::string>           tonescale_preset;
  std::optional<std::string>           creative_white;
  std::optional<float>                 creative_white_limit;
  std::optional<float>                 display_grey_luminance;
  std::optional<float>                 hdr_grey_boost;
  std::optional<float>                 hdr_purity;
  std::optional<OpenDrtDetailedParams> parameters;
  std::optional<std::string>           film_negative;
  std::optional<std::string>           film_print_material;
  std::optional<std::string>           film_scene_light;
  std::optional<float>                 film_light_spread;
  std::optional<float>                 film_halation;
  std::optional<float>                 film_enlarger_flare;
  std::optional<std::string>           film_scan_auto;
  std::optional<float>                 film_scan_auto_strength;
  std::optional<float>                 film_scan_auto_density;
  std::optional<float>                 film_scan_contrast;
  std::optional<float>                 film_scan_exposure_stops;
  std::optional<float>                 film_scan_cyan;
  std::optional<float>                 film_scan_magenta;
  std::optional<float>                 film_scan_yellow;
  std::optional<float>                 film_scan_black;
  std::optional<float>                 film_scanner_red_nm;
  std::optional<float>                 film_scanner_green_nm;
  std::optional<float>                 film_scanner_blue_nm;
  std::optional<float>                 film_scanner_width_nm;
  std::optional<std::string>           film_scanner_light;
};

/// The film format write of the editor field `film_format` (@ref DrtParamsModel::ApplyFilmFormat).
struct DrtFilmFormatUpdate {
  FilmFormat format = FilmFormat::Film35mm;
};

class DrtParamsModel final : public OperatorModelBase<DrtParamsModel, DrtPayload, DrtDirty> {
 public:
  static auto        TypeId() -> const OperatorTypeId& { return type_ids::DrtNode(); }

  [[nodiscard]] auto IsDefault() const -> bool override;

  /**
   * @brief Read individual DRT/ODT fields through the owning Model lock.
   */
  [[nodiscard]] auto Method() const -> DrtMethod;
  [[nodiscard]] auto EncodingSpace() const -> DrtColorSpace;
  [[nodiscard]] auto EncodingEotf() const -> DrtEotf;
  [[nodiscard]] auto LimitingSpace() const -> DrtColorSpace;
  [[nodiscard]] auto PeakLuminance() const -> float;
  [[nodiscard]] auto LookPreset() const -> std::string;
  [[nodiscard]] auto TonescalePreset() const -> std::string;
  [[nodiscard]] auto CreativeWhite() const -> std::string;
  [[nodiscard]] auto CreativeWhiteLimit() const -> float;
  [[nodiscard]] auto DisplayGreyLuminance() const -> float;
  [[nodiscard]] auto HdrGreyBoost() const -> float;
  [[nodiscard]] auto HdrPurity() const -> float;
  [[nodiscard]] auto DiffusionStrength() const -> float;
  [[nodiscard]] auto FilmPrint() const -> FilmPrintParams;
  [[nodiscard]] auto FilmFormat() const -> alcedo::FilmFormat;

  /**
   * @brief Apply validated DRT/ODT fields atomically and mark changed groups dirty.
   *
   * @throws std::invalid_argument that names the field for a non-finite value, a value outside its
   *         range, or an unknown identifier of the film print. No field changes.
   */
  void               ApplyUpdate(DrtParameterUpdate update);

  /**
   * @brief Set the diffusion filter strength and mark @ref DrtDirty::Diffusion when it changes.
   *
   * The value is clamped to [@ref kDiffusionStrengthMin, @ref kDiffusionStrengthMax].
   * @throws std::invalid_argument for a non-finite value. No field changes.
   */
  void               ApplyDiffusionStrength(float strength);

  /**
   * @brief Set the film format. A change marks @ref DrtDirty::FilmFormat and
   *        @ref DrtDirty::FilmExposure, because the format sets the frame of the film print image
   *        plane, so the canonical film print results are made again.
   *
   * @throws std::invalid_argument that names the field `film_format` for a value outside the enum.
   *         No field changes.
   */
  void               ApplyFilmFormat(alcedo::FilmFormat format);

  /**
   * @brief Output-transform fields only (method, encoding, OpenDRT, and `film_print` when the
   *        method is the film print). Excludes the diffusion filter and the film format, which the
   *        editor writes through their own `diffusion` and `film_format` fields.
   */
  [[nodiscard]] auto OutputTransformJson() const -> nlohmann::json;

  /**
   * @brief @ref OutputTransformJson plus `diffusion.strength` when the strength is not 0, and
   *        `film_format` when the format is not `35mm`.
   *
   * A document without the filter and with the format `35mm` keeps its previous serialized bytes.
   */
  [[nodiscard]] auto ToJson() const -> nlohmann::json override;
  /**
   * @throws std::runtime_error that names the key for an invalid `diffusion`, `film_print`, or
   *         `film_format` value. No field changes. A missing object or key reads as its default.
   */
  void               LoadJson(const nlohmann::json& json) override;

  [[nodiscard]] auto Params() const -> DrtPayload { return PayloadCopy(); }
  void               ReplaceParams(DrtPayload payload);
};

/**
 * @brief Display-referred endpoint. One scene-image input; display output is not
 * a scene graph port.
 *
 * Owns output-transform parameters, the scene-linear diffusion filter that runs before the
 * display transform, and the four display-referred DRT/Post adjustments (Clarity, Sharpen,
 * Halation, Film Grain) that run after it.
 */
class DrtNodeModel final : public INodeModel {
 public:
  explicit DrtNodeModel(NodeId id);

  [[nodiscard]] auto Id() const -> const NodeId& override { return id_; }
  [[nodiscard]] auto Type() const -> const OperatorTypeId& override { return type_ids::DrtNode(); }
  [[nodiscard]] auto DisplayName() const -> std::string_view override { return "DRT"; }
  [[nodiscard]] auto InputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto OutputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto ToJson() const -> nlohmann::json override;
  /// Copies the endpoint parameters; shares adjustment Models (@ref INodeModel::Clone).
  [[nodiscard]] auto Clone() const -> std::shared_ptr<INodeModel> override;

  [[nodiscard]] auto Params() -> DrtParamsModel& { return params_; }
  [[nodiscard]] auto Params() const -> const DrtParamsModel& { return params_; }

  /**
   * @brief DRT endpoint with default ODT params and the four catalog post adjustments.
   *
   * Instance ids are `drt.clarity`, `drt.sharpen`, `drt.halation`, and `drt.film_grain`
   * when @p id is `drt`.
   */
  static auto        MakeDefault(NodeId id) -> std::unique_ptr<DrtNodeModel>;
  static auto        FromJson(const nlohmann::json& json) -> std::unique_ptr<DrtNodeModel>;

  [[nodiscard]] auto AdjustmentCount() const -> std::size_t { return adjustments_.size(); }
  [[nodiscard]] auto AdjustmentIdAt(std::size_t index) const -> const AdjustmentInstanceId&;
  /// Non-const Model lookups copy a Model shared with a frozen document first
  /// (@ref MutableAdjustmentModel). Use the const overloads to read.
  [[nodiscard]] auto AdjustmentAt(std::size_t index) -> IOperatorModel&;
  [[nodiscard]] auto AdjustmentAt(std::size_t index) const -> const IOperatorModel&;
  [[nodiscard]] auto FindAdjustment(const AdjustmentInstanceId& id) -> IOperatorModel*;
  [[nodiscard]] auto FindAdjustment(const AdjustmentInstanceId& id) const -> const IOperatorModel*;
  [[nodiscard]] auto FindAdjustmentByType(const OperatorTypeId& type) -> IOperatorModel*;
  [[nodiscard]] auto FindAdjustmentByType(const OperatorTypeId& type) const
      -> const IOperatorModel*;
  [[nodiscard]] auto FindAdjustmentIdByType(const OperatorTypeId& type) const
      -> const AdjustmentInstanceId*;

  /**
   * @brief Insert a DRT/Post-owned adjustment.
   * @throws std::runtime_error when @p model is Color Grade-owned or unsupported.
   */
  void InsertAdjustment(std::size_t index, AdjustmentInstanceId id,
                        std::unique_ptr<IOperatorModel> model);

 private:
  /// Used only by @ref Clone. Copy assignment is deleted.
  DrtNodeModel(const DrtNodeModel& other)            = default;
  DrtNodeModel& operator=(const DrtNodeModel& other) = delete;

  NodeId                            id_;
  DrtParamsModel                    params_;
  std::vector<AdjustmentModelEntry> adjustments_;
  std::array<PortDescriptor, 1>     inputs_;
  std::array<PortDescriptor, 1>     outputs_;
};

/**
 * @brief True when the DRT node encodes an HDR signal (ST 2084 or HLG).
 *
 * Reads only @p drt's encoding EOTF through its Model lock. Library HDR flags use this value;
 * no other parameter store is consulted.
 */
[[nodiscard]] auto IsHdrExportEncoding(const DrtNodeModel& drt) -> bool;

}  // namespace alcedo
