//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_parameter_write.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include "app/editor_adjustment_types.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/image_geometry_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"

namespace alcedo {
namespace {

auto SetError(std::string* error, std::string message) -> bool {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

auto ColorGradeOf(PipelineDocument& document, const NodeId& id, std::string* error)
    -> ColorGradeNodeModel* {
  auto* grade = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(id));
  if (grade == nullptr) {
    SetError(error, "Color Grade node is missing: " + std::string(id.Value()));
    return nullptr;
  }
  return grade;
}

auto IsDrtPostAdjustmentField(std::string_view field) -> bool {
  return field == "clarity" || field == "sharpen" || field == "halation" || field == "film_grain";
}

auto DrtPostModel(DrtNodeModel& drt, const AdjustmentInstanceId& id, std::string* error)
    -> IOperatorModel* {
  auto* model = drt.FindAdjustment(id);
  if (model == nullptr) {
    SetError(error, "Adjustment instance is missing: " + std::string(id.Value()));
  }
  return model;
}

template <typename Alternative>
auto RequireWrite(const EditorParameterWrite& write, std::string_view field) -> const Alternative& {
  const auto* value = std::get_if<Alternative>(&write);
  if (value == nullptr) {
    throw std::invalid_argument("Field " + std::string{field} +
                                " received an incompatible write payload");
  }
  return *value;
}

template <typename Model>
void ApplyScalarValue(IOperatorModel& base, std::string_view field, float value) {
  auto* model = dynamic_cast<Model*>(&base);
  if (model == nullptr) {
    throw std::invalid_argument("Adjustment Model type does not match field " + std::string{field});
  }
  model->SetValue(value);
}

void ApplyColorGradeWrite(IOperatorModel& model, std::string_view field,
                          const EditorParameterWrite& write) {
  if (field == "exposure") {
    ApplyScalarValue<ExposureModel>(model, field, RequireWrite<EditorScalarWrite>(write, field).value);
    return;
  }
  if (field == "contrast") {
    ApplyScalarValue<ContrastModel>(model, field, RequireWrite<EditorScalarWrite>(write, field).value);
    return;
  }
  if (field == "white" || field == "whites") {
    ApplyScalarValue<WhiteModel>(model, field, RequireWrite<EditorScalarWrite>(write, field).value);
    return;
  }
  if (field == "black" || field == "blacks") {
    ApplyScalarValue<BlackModel>(model, field, RequireWrite<EditorScalarWrite>(write, field).value);
    return;
  }
  if (field == "shadows") {
    ApplyScalarValue<ShadowsModel>(model, field, RequireWrite<EditorScalarWrite>(write, field).value);
    return;
  }
  if (field == "highlights") {
    ApplyScalarValue<HighlightsModel>(model, field,
                                      RequireWrite<EditorScalarWrite>(write, field).value);
    return;
  }
  if (field == "saturation") {
    ApplyScalarValue<SaturationModel>(model, field,
                                      RequireWrite<EditorScalarWrite>(write, field).value);
    return;
  }
  if (field == "vibrance") {
    ApplyScalarValue<VibranceModel>(model, field, RequireWrite<EditorScalarWrite>(write, field).value);
    return;
  }
  if (field == "curve") {
    auto* typed = dynamic_cast<CurveModel*>(&model);
    if (typed == nullptr) {
      throw std::invalid_argument("Adjustment Model type does not match field curve");
    }
    typed->SetPoints(RequireWrite<EditorCurveWrite>(write, field).points);
    return;
  }
  if (field == "tint") {
    auto* typed = dynamic_cast<Cat02WhiteBalanceModel*>(&model);
    if (typed == nullptr) {
      throw std::invalid_argument("Adjustment Model type does not match field tint");
    }
    typed->ApplyUpdate(RequireWrite<Cat02WhiteBalanceUpdate>(write, field));
    return;
  }
  if (field == "hls" || field == "HLS") {
    auto* typed = dynamic_cast<HlsModel*>(&model);
    if (typed == nullptr) {
      throw std::invalid_argument("Adjustment Model type does not match field HLS");
    }
    typed->ApplyUpdate(RequireWrite<HlsUpdate>(write, field));
    return;
  }
  if (field == "color_wheel") {
    auto* typed = dynamic_cast<ColorWheelModel*>(&model);
    if (typed == nullptr) {
      throw std::invalid_argument("Adjustment Model type does not match field color_wheel");
    }
    typed->ApplyUpdate(RequireWrite<ColorWheelUpdate>(write, field));
    return;
  }
  if (field == "lut" || field == "ocio_lmt") {
    auto* typed = dynamic_cast<LmtModel*>(&model);
    if (typed == nullptr) {
      throw std::invalid_argument("Adjustment Model type does not match field lut");
    }
    typed->SetCubePath(RequireWrite<EditorLutWrite>(write, field).cube_path);
    return;
  }
  throw std::invalid_argument("Unsupported Color Grade parameter field: " + std::string{field});
}

void ApplyDrtPostWrite(IOperatorModel& model, std::string_view field,
                       const EditorParameterWrite& write) {
  if (field == "clarity") {
    ApplyScalarValue<ClarityModel>(model, field, RequireWrite<EditorScalarWrite>(write, field).value);
    return;
  }
  if (field == "halation") {
    ApplyScalarValue<HalationModel>(model, field, RequireWrite<EditorScalarWrite>(write, field).value);
    return;
  }
  if (field == "film_grain") {
    ApplyScalarValue<FilmGrainModel>(model, field,
                                     RequireWrite<EditorScalarWrite>(write, field).value);
    return;
  }
  if (field == "sharpen") {
    auto* typed = dynamic_cast<SharpenModel*>(&model);
    if (typed == nullptr) {
      throw std::invalid_argument("Adjustment Model type does not match field sharpen");
    }
    typed->ApplyUpdate(RequireWrite<SharpenUpdate>(write, field));
    return;
  }
  throw std::invalid_argument("Unsupported DRT/Post parameter field: " + std::string{field});
}

}  // namespace

auto ApplyEditorParameterWrite(PipelineDocument& document, const EditorParameterTarget& target,
                               const EditorParameterWrite& write, std::string* error) -> bool {
  const auto target_error = DescribeEditorParameterTargetError(target, target.field_key);
  if (!target_error.empty()) {
    return SetError(error, target_error);
  }
  try {
    switch (target.owner_kind) {
      case EditorParameterOwnerKind::ColorGrade: {
        auto* grade = ColorGradeOf(document, target.node_id, error);
        if (grade == nullptr) {
          return false;
        }
        auto* model = grade->FindAdjustment(target.adjustment_instance_id);
        if (model == nullptr) {
          return SetError(error, "Adjustment instance is missing: " +
                                     std::string(target.adjustment_instance_id.Value()));
        }
        ApplyColorGradeWrite(*model, target.field_key, write);
        return true;
      }
      case EditorParameterOwnerKind::Document: {
        document.Geometry().ApplyUpdate(RequireWrite<ImageGeometryUpdate>(write, target.field_key));
        return true;
      }
      case EditorParameterOwnerKind::Develop: {
        auto* develop = document.Develop();
        if (develop == nullptr || develop->Id() != target.node_id) {
          return SetError(error, "Develop node is missing: " + std::string(target.node_id.Value()));
        }
        if (target.field_key == "raw_decode") {
          develop->Params().ApplyRawDecodeUpdate(
              RequireWrite<DevelopRawDecodeUpdate>(write, target.field_key));
        } else if (target.field_key == "color_temp") {
          develop->Params().ApplyColorTemperatureUpdate(
              RequireWrite<DevelopColorTemperatureUpdate>(write, target.field_key));
        } else if (target.field_key == "lens_calib") {
          develop->Params().ApplyLensCalibrationUpdate(
              RequireWrite<DevelopLensCalibrationUpdate>(write, target.field_key));
        } else {
          return SetError(error, "Unsupported Develop parameter field: " + target.field_key);
        }
        return true;
      }
      case EditorParameterOwnerKind::DrtPost: {
        auto* drt = document.Drt();
        if (drt == nullptr || drt->Id() != target.node_id) {
          return SetError(error, "DRT node is missing: " + std::string(target.node_id.Value()));
        }
        if (IsDrtPostAdjustmentField(target.field_key)) {
          auto* model = DrtPostModel(*drt, target.adjustment_instance_id, error);
          if (model == nullptr) {
            return false;
          }
          ApplyDrtPostWrite(*model, target.field_key, write);
          return true;
        }
        if (target.field_key != "odt") {
          return SetError(error, "Unsupported DRT parameter field: " + target.field_key);
        }
        drt->Params().ApplyUpdate(RequireWrite<DrtParameterUpdate>(write, target.field_key));
        return true;
      }
      default:
        return SetError(error, "Editor parameter target owner_kind is not supported");
    }
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }
}

}  // namespace alcedo
