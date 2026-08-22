//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <memory>
#include <span>

#include "edit/graph/i_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"

namespace alcedo {

enum class AnalyticMaskKind {
  Radial,
  GraduatedNd,
};

struct RadialMaskParams {
  float center_x      = 0.5f;
  float center_y      = 0.5f;
  float major_radius  = 0.5f;
  float minor_radius  = 0.5f;
  float rotation      = 0.0f;
  float inner_feather = 0.0f;
  float outer_feather = 0.0f;
  bool  invert        = false;
};

struct GraduatedNdMaskParams {
  float origin_x             = 0.5f;
  float origin_y             = 0.5f;
  float normal_x             = 0.0f;
  float normal_y             = 1.0f;
  float transition_distance  = 0.2f;
  float start_value          = 1.0f;
  float end_value            = 0.0f;
  bool  invert               = false;
};

/**
 * @brief Analytic mask node. Produces a Mask port; not created in the default graph.
 */
class AnalyticMaskNodeModel final : public INodeModel {
 public:
  explicit AnalyticMaskNodeModel(NodeId id, AnalyticMaskKind kind = AnalyticMaskKind::Radial);

  [[nodiscard]] auto Id() const -> const NodeId& override { return id_; }
  [[nodiscard]] auto Type() const -> const OperatorTypeId& override {
    return type_ids::AnalyticMaskNode();
  }
  [[nodiscard]] auto InputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto OutputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto ToJson() const -> nlohmann::json override;

  [[nodiscard]] auto Kind() const -> AnalyticMaskKind { return kind_; }
  [[nodiscard]] auto Radial() const -> const RadialMaskParams& { return radial_; }
  [[nodiscard]] auto GraduatedNd() const -> const GraduatedNdMaskParams& { return graduated_; }

  void SetKind(AnalyticMaskKind kind) { kind_ = kind; }
  void SetRadial(RadialMaskParams params) { radial_ = params; }
  void SetGraduatedNd(GraduatedNdMaskParams params) { graduated_ = params; }

  static auto FromJson(const nlohmann::json& json) -> std::unique_ptr<AnalyticMaskNodeModel>;

 private:
  NodeId                 id_;
  AnalyticMaskKind       kind_ = AnalyticMaskKind::Radial;
  RadialMaskParams       radial_{};
  GraduatedNdMaskParams  graduated_{};
  std::array<PortDescriptor, 1> outputs_;
};

}  // namespace alcedo
