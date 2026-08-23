//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "edit/graph/i_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

/**
 * @brief Import-time camera colour matrices stored on the Develop node.
 *
 * ColorMatrix/ForwardMatrix/AsShotNeutral/illuminant CCT are copied from the
 * image at import. CameraColor interpolates these fields; it does not parse RAW
 * or look up the CameraMatrices database at render time. cam_mul is only used
 * to derive as-shot neutral when AsShotNeutral is absent.
 */
struct DevelopCameraProfile {
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
  return a.color_matrices_valid == b.color_matrices_valid && a.color_matrix_1 == b.color_matrix_1 &&
         a.color_matrix_2 == b.color_matrix_2 &&
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

struct DevelopPayload {
  std::string demosaic_method          = "default";
  bool        highlights_reconstruct   = true;
  bool        use_camera_wb            = true;
  float       user_wb                  = 7600.0f;
  std::string wb_mode                  = "as_shot";
  float       custom_cct               = 6500.0f;
  float       custom_tint              = 0.0f;
  float       as_shot_cct              = 6500.0f;
  float       as_shot_tint             = 0.0f;
  DevelopCameraProfile camera_profile{};
  bool        lens_enabled             = false;
  bool        apply_vignetting         = true;
  bool        apply_distortion         = true;
  bool        apply_tca                = true;
  bool        apply_crop               = true;
  bool        auto_scale               = true;
  bool        use_user_scale           = false;
  float       user_scale               = 1.0f;
  bool        projection_enabled       = false;
  std::string target_projection        = "unknown";
  std::string lens_profile_db_path     = "src/config/lens_calib";
};

enum class DevelopDirty : std::uint32_t {
  None        = 0,
  Demosaic    = 1U << 0,
  Highlights  = 1U << 1,
  WhiteBalance = 1U << 2,
  Lens        = 1U << 3,
  All         = Demosaic | Highlights | WhiteBalance | Lens,
};

/**
 * @brief Develop endpoint parameters. LibRaw unpack stays outside the graph.
 */
class DevelopParamsModel final
    : public OperatorModelBase<DevelopParamsModel, DevelopPayload, DevelopDirty> {
 public:
  static auto TypeId() -> const OperatorTypeId& { return type_ids::DevelopNode(); }

  [[nodiscard]] auto IsDefault() const -> bool override;
  [[nodiscard]] auto ToJson() const -> nlohmann::json override;
  void               LoadJson(const nlohmann::json& json) override;

  [[nodiscard]] auto Params() const -> DevelopPayload { return PayloadCopy(); }
  void               ReplaceParams(DevelopPayload payload);
};

/**
 * @brief Develop root node. No image input; one scene-image output.
 */
class DevelopNodeModel final : public INodeModel {
 public:
  explicit DevelopNodeModel(NodeId id);

  [[nodiscard]] auto Id() const -> const NodeId& override { return id_; }
  [[nodiscard]] auto Type() const -> const OperatorTypeId& override { return type_ids::DevelopNode(); }
  [[nodiscard]] auto InputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto OutputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto ToJson() const -> nlohmann::json override;

  [[nodiscard]] auto Params() -> DevelopParamsModel& { return params_; }
  [[nodiscard]] auto Params() const -> const DevelopParamsModel& { return params_; }

  static auto FromJson(const nlohmann::json& json) -> std::unique_ptr<DevelopNodeModel>;

 private:
  NodeId             id_;
  DevelopParamsModel params_;
  std::array<PortDescriptor, 1> outputs_;
};

}  // namespace alcedo
