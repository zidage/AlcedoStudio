//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/adjustment_catalog.hpp"

#include <stdexcept>

#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"

namespace alcedo {

namespace {

template <class Model>
auto MakeFactory() -> OperatorModelFactory {
  return []() -> std::unique_ptr<IOperatorModel> { return std::make_unique<Model>(); };
}

}  // namespace

auto BuiltinAdjustmentCatalog::Instance() -> const BuiltinAdjustmentCatalog& {
  static const BuiltinAdjustmentCatalog catalog;
  return catalog;
}

BuiltinAdjustmentCatalog::BuiltinAdjustmentCatalog() {
  Register({type_ids::Cat02WhiteBalance(), "CAT02 White Balance", MakeFactory<Cat02WhiteBalanceModel>()});
  Register({type_ids::Exposure(), "Exposure", MakeFactory<ExposureModel>()});
  Register({type_ids::Contrast(), "Contrast", MakeFactory<ContrastModel>()});
  Register({type_ids::White(), "White", MakeFactory<WhiteModel>()});
  Register({type_ids::Black(), "Black", MakeFactory<BlackModel>()});
  Register({type_ids::Shadows(), "Shadows", MakeFactory<ShadowsModel>()});
  Register({type_ids::Highlights(), "Highlights", MakeFactory<HighlightsModel>()});
  Register({type_ids::Curve(), "Curve", MakeFactory<CurveModel>()});
  Register({type_ids::Hls(), "HLS", MakeFactory<HlsModel>()});
  Register({type_ids::Saturation(), "Saturation", MakeFactory<SaturationModel>()});
  Register({type_ids::Vibrance(), "Vibrance", MakeFactory<VibranceModel>()});
  Register({type_ids::ColorWheel(), "Color Wheel", MakeFactory<ColorWheelModel>()});
  Register({type_ids::Lmt(), "LMT", MakeFactory<LmtModel>()});
  Register({type_ids::Clarity(), "Clarity", MakeFactory<ClarityModel>()});
  Register({type_ids::Sharpen(), "Sharpen", MakeFactory<SharpenModel>()});
  Register({type_ids::Halation(), "Halation", MakeFactory<HalationModel>()});
  Register({type_ids::FilmGrain(), "Film Grain", MakeFactory<FilmGrainModel>()});
  Register({type_ids::Tint(), "Tint", MakeFactory<TintModel>()});
}

void BuiltinAdjustmentCatalog::Register(AdjustmentDefinition definition) {
  for (const auto& existing : definitions_) {
    if (existing.type.Text() == definition.type.Text()) {
      throw std::logic_error("Duplicate adjustment type text: " + std::string{definition.type.Text()});
    }
    if (existing.type.Hash() == definition.type.Hash()) {
      throw std::logic_error("Duplicate adjustment type hash: " + std::string{definition.type.Text()});
    }
  }
  definitions_.push_back(std::move(definition));
}

auto BuiltinAdjustmentCatalog::Find(const OperatorTypeId& type) const
    -> const AdjustmentDefinition* {
  return Find(type.Text());
}

auto BuiltinAdjustmentCatalog::Find(std::string_view text) const -> const AdjustmentDefinition* {
  for (const auto& definition : definitions_) {
    if (definition.type.Text() == text) {
      return &definition;
    }
  }
  return nullptr;
}

auto BuiltinAdjustmentCatalog::Definitions() const -> const std::vector<AdjustmentDefinition>& {
  return definitions_;
}

auto BuiltinAdjustmentCatalog::CreateDefault(const OperatorTypeId& type) const
    -> std::unique_ptr<IOperatorModel> {
  const auto* definition = Find(type);
  if (definition == nullptr) {
    return nullptr;
  }
  return definition->create_default_model();
}

}  // namespace alcedo
