//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "edit/graph/i_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

enum class DrtMethod : int {
  Aces20  = 0,
  OpenDrt = 1,
};

enum class DrtColorSpace : int {
  Rec709  = 0,
  Rec2020 = 1,
  P3D65   = 2,
};

enum class DrtEotf : int {
  Linear   = 0,
  St2084   = 1,
  Hlg      = 2,
  Gamma26  = 3,
  Bt1886   = 4,
  Gamma22  = 5,
  Gamma18  = 6,
};

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
  DrtMethod     method          = DrtMethod::OpenDrt;
  DrtColorSpace encoding_space  = DrtColorSpace::Rec709;
  DrtEotf       encoding_eotf   = DrtEotf::Gamma22;
  DrtColorSpace limiting_space  = DrtColorSpace::Rec709;
  float         peak_luminance  = 100.0f;
  std::string   look_preset     = "standard";
  std::string   tonescale_preset = "use_look_preset";
  std::string   creative_white  = "use_look_preset";
  float         creative_white_limit     = 0.25f;
  float         display_grey_luminance   = 10.0f;
  float         hdr_grey_boost           = 0.13f;
  float         hdr_purity               = 0.5f;
  OpenDrtDetailedParams parameters{};
};

enum class DrtDirty : std::uint32_t {
  None       = 0,
  Method     = 1U << 0,
  Encoding   = 1U << 1,
  OpenDrt    = 1U << 2,
  All        = Method | Encoding | OpenDrt,
};

class DrtParamsModel final : public OperatorModelBase<DrtParamsModel, DrtPayload, DrtDirty> {
 public:
  static auto TypeId() -> const OperatorTypeId& { return type_ids::DrtNode(); }

  [[nodiscard]] auto IsDefault() const -> bool override;
  [[nodiscard]] auto ToJson() const -> nlohmann::json override;
  void               LoadJson(const nlohmann::json& json) override;

  [[nodiscard]] auto Params() const -> DrtPayload { return PayloadCopy(); }
  void               ReplaceParams(DrtPayload payload);
};

/**
 * @brief Display-referred endpoint. One scene-image input; display output is not
 * a scene graph port.
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

  [[nodiscard]] auto Params() -> DrtParamsModel& { return params_; }
  [[nodiscard]] auto Params() const -> const DrtParamsModel& { return params_; }

  static auto FromJson(const nlohmann::json& json) -> std::unique_ptr<DrtNodeModel>;

 private:
  NodeId         id_;
  DrtParamsModel params_;
  std::array<PortDescriptor, 1> inputs_;
  std::array<PortDescriptor, 1> outputs_;
};

}  // namespace alcedo
