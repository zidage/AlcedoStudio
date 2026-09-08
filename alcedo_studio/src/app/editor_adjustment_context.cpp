//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_adjustment_context.hpp"

#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "image/image.hpp"

namespace alcedo {
namespace {

constexpr std::array<std::string_view, 2> kDevelopPanels{kAdjustmentPanelRaw,
                                                         kAdjustmentPanelGeometry};
constexpr std::array<std::string_view, 4> kColorGradePanels{
    kAdjustmentPanelTone, kAdjustmentPanelLook, kAdjustmentPanelLut, kAdjustmentPanelMasks};
constexpr std::array<std::string_view, 2> kDrtPanels{kAdjustmentPanelDisplay,
                                                     kAdjustmentPanelDetail};

auto SetError(std::string* error, std::string message) -> bool {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

auto KindOf(const INodeModel& node) -> std::optional<EditorNodeKind> {
  if (node.Type() == type_ids::DevelopNode()) {
    return EditorNodeKind::Develop;
  }
  if (node.Type() == type_ids::ColorGradeNode()) {
    return EditorNodeKind::ColorGrade;
  }
  if (node.Type() == type_ids::DrtNode()) {
    return EditorNodeKind::Drt;
  }
  return std::nullopt;
}

auto IsDrtPostField(std::string_view field) -> bool {
  return field == "clarity" || field == "sharpen" || field == "halation" || field == "film_grain";
}

auto IsDevelopField(std::string_view field) -> bool {
  return field == "raw_decode" || field == "lens_calib" || field == "color_temp" ||
         field == "crop_rotate";
}

auto IsColorGradeField(std::string_view field) -> bool {
  return field == "exposure" || field == "contrast" || field == "white" || field == "whites" ||
         field == "black" || field == "blacks" || field == "shadows" || field == "highlights" ||
         field == "curve" || field == "saturation" || field == "vibrance" || field == "tint" ||
         field == "hls" || field == "HLS" || field == "color_wheel" || field == "lut" ||
         field == "ocio_lmt";
}

auto OperatorTypeForField(std::string_view field) -> const OperatorTypeId* {
  if (field == "exposure") return &type_ids::Exposure();
  if (field == "contrast") return &type_ids::Contrast();
  if (field == "white" || field == "whites") return &type_ids::White();
  if (field == "black" || field == "blacks") return &type_ids::Black();
  if (field == "shadows") return &type_ids::Shadows();
  if (field == "highlights") return &type_ids::Highlights();
  if (field == "curve") return &type_ids::Curve();
  if (field == "saturation") return &type_ids::Saturation();
  if (field == "vibrance") return &type_ids::Vibrance();
  if (field == "tint") return &type_ids::Cat02WhiteBalance();
  if (field == "hls" || field == "HLS") return &type_ids::Hls();
  if (field == "color_wheel") return &type_ids::ColorWheel();
  if (field == "lut" || field == "ocio_lmt") return &type_ids::Lmt();
  if (field == "clarity") return &type_ids::Clarity();
  if (field == "sharpen") return &type_ids::Sharpen();
  if (field == "halation") return &type_ids::Halation();
  if (field == "film_grain") return &type_ids::FilmGrain();
  return nullptr;
}

}  // namespace

auto ReadEditorImageExifDisplay(const ExifDisplayMetaData& metadata) -> EditorImageExifDisplay {
  EditorImageExifDisplay display;
  if (metadata.shutter_speed_.first > 0 && metadata.shutter_speed_.second > 0) {
    display.shutter_speed = metadata.shutter_speed_;
  }
  if (metadata.iso_ > 0) {
    display.iso = metadata.iso_;
  }
  if (metadata.aperture_ > 0.0f) {
    display.aperture = metadata.aperture_;
  }
  if (metadata.focal_ > 0.0f) {
    display.focal_mm = metadata.focal_;
  }
  return display;
}

auto ReadEditorImageExifDisplay(const Image& image) -> EditorImageExifDisplay {
  if (!image.has_exif_display_.load()) {
    return {};
  }
  return ReadEditorImageExifDisplay(image.exif_display_);
}

namespace {

auto FormatPositiveNumber(float value) -> std::string {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(1) << value;
  std::string out = stream.str();
  while (out.size() > 1 && out.back() == '0') {
    out.pop_back();
  }
  if (!out.empty() && out.back() == '.') {
    out.pop_back();
  }
  return out;
}

auto AppendExifToken(std::string* line, const std::string& token) -> void {
  if (line == nullptr || token.empty() || token == kMissingExifDisplay) {
    return;
  }
  if (!line->empty()) {
    *line += ' ';
  }
  *line += token;
}

}  // namespace

auto FormatEditorImageExifDisplay(const EditorImageExifDisplay& display) -> EditorExifRowText {
  EditorExifRowText text;
  if (display.shutter_speed.has_value()) {
    const auto [numerator, denominator] = *display.shutter_speed;
    if (denominator == 1) {
      text.shutter = std::to_string(numerator) + "s";
    } else {
      text.shutter = std::to_string(numerator) + "/" + std::to_string(denominator) + "s";
    }
  } else {
    text.shutter = std::string(kMissingExifDisplay);
  }
  if (display.iso.has_value()) {
    text.iso = "ISO " + std::to_string(*display.iso);
  } else {
    text.iso = std::string(kMissingExifDisplay);
  }
  if (display.aperture.has_value() && std::isfinite(*display.aperture)) {
    text.aperture = "f" + FormatPositiveNumber(*display.aperture);
  } else {
    text.aperture = std::string(kMissingExifDisplay);
  }
  if (display.focal_mm.has_value() && std::isfinite(*display.focal_mm)) {
    text.focal = FormatPositiveNumber(*display.focal_mm) + "mm";
  } else {
    text.focal = std::string(kMissingExifDisplay);
  }
  return text;
}

auto FormatEditorImageExifLine(const EditorExifRowText& text) -> std::string {
  std::string line;
  AppendExifToken(&line, text.focal);
  AppendExifToken(&line, text.aperture);
  AppendExifToken(&line, text.shutter);
  AppendExifToken(&line, text.iso);
  if (line.empty()) {
    return std::string(kMissingExifDisplay);
  }
  return line;
}

auto FormatEditorImageExifLine(const EditorImageExifDisplay& display) -> std::string {
  return FormatEditorImageExifLine(FormatEditorImageExifDisplay(display));
}

auto SupportedAdjustmentPanels(EditorNodeKind kind) -> std::span<const std::string_view> {
  switch (kind) {
    case EditorNodeKind::Develop:
      return kDevelopPanels;
    case EditorNodeKind::ColorGrade:
      return kColorGradePanels;
    case EditorNodeKind::Drt:
      return kDrtPanels;
  }
  return {};
}

auto DefaultAdjustmentPanel(EditorNodeKind kind) -> std::string_view {
  switch (kind) {
    case EditorNodeKind::Develop:
      return kAdjustmentPanelRaw;
    case EditorNodeKind::ColorGrade:
      return kAdjustmentPanelTone;
    case EditorNodeKind::Drt:
      return kAdjustmentPanelDisplay;
  }
  return kAdjustmentPanelTone;
}

auto AdjustmentPanelIsSupported(EditorNodeKind kind, std::string_view panel) -> bool {
  for (const auto supported : SupportedAdjustmentPanels(kind)) {
    if (supported == panel) {
      return true;
    }
  }
  return false;
}

auto AdjustmentFieldIsSupported(EditorNodeKind kind, std::string_view field_key) -> bool {
  switch (kind) {
    case EditorNodeKind::Develop:
      return IsDevelopField(field_key);
    case EditorNodeKind::ColorGrade:
      return IsColorGradeField(field_key);
    case EditorNodeKind::Drt:
      return field_key == "odt" || IsDrtPostField(field_key);
  }
  return false;
}

auto CompleteSelectedNodeParameterTarget(const PipelineDocument& document,
                                         const NodeId& selected_node_id, std::string field_key,
                                         std::string* error)
    -> std::optional<EditorParameterTarget> {
  const auto* node = document.Graph().FindNode(selected_node_id);
  if (node == nullptr) {
    SetError(error, "Selected node is missing: " + std::string(selected_node_id.Value()));
    return std::nullopt;
  }
  const auto kind = KindOf(*node);
  if (!kind.has_value()) {
    SetError(error, "Selected node kind is not supported: " + std::string(selected_node_id.Value()));
    return std::nullopt;
  }
  if (!AdjustmentFieldIsSupported(*kind, field_key)) {
    SetError(error, "Field " + field_key + " is not owned by the selected node");
    return std::nullopt;
  }

  EditorParameterTarget target;
  target.field_key = std::move(field_key);
  if (target.field_key == "crop_rotate") {
    target.owner_kind = EditorParameterOwnerKind::Document;
    return target;
  }
  if (target.field_key == "raw_decode" || target.field_key == "lens_calib" ||
      target.field_key == "color_temp") {
    const auto* develop = document.Develop();
    if (develop == nullptr || develop->Id() != selected_node_id) {
      SetError(error, "Develop node is missing: " + std::string(selected_node_id.Value()));
      return std::nullopt;
    }
    target.owner_kind = EditorParameterOwnerKind::Develop;
    target.node_id    = selected_node_id;
    return target;
  }
  if (target.field_key == "odt") {
    const auto* drt = document.Drt();
    if (drt == nullptr || drt->Id() != selected_node_id) {
      SetError(error, "DRT node is missing: " + std::string(selected_node_id.Value()));
      return std::nullopt;
    }
    target.owner_kind = EditorParameterOwnerKind::DrtPost;
    target.node_id    = selected_node_id;
    return target;
  }
  const auto* type = OperatorTypeForField(target.field_key);
  if (type == nullptr) {
    SetError(error, "Unknown editor adjustment field: " + target.field_key);
    return std::nullopt;
  }
  if (IsDrtPostField(target.field_key)) {
    const auto* drt = dynamic_cast<const DrtNodeModel*>(node);
    if (drt == nullptr) {
      SetError(error, "DRT node is missing: " + std::string(selected_node_id.Value()));
      return std::nullopt;
    }
    const auto* instance = drt->FindAdjustmentIdByType(*type);
    if (instance == nullptr) {
      SetError(error, "DRT/Post adjustment is missing: " + std::string{type->Text()});
      return std::nullopt;
    }
    target.owner_kind             = EditorParameterOwnerKind::DrtPost;
    target.node_id                = selected_node_id;
    target.adjustment_instance_id = *instance;
    return target;
  }
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(node);
  if (grade == nullptr) {
    SetError(error, "Color Grade node is missing: " + std::string(selected_node_id.Value()));
    return std::nullopt;
  }
  const auto* instance = grade->FindAdjustmentIdByType(*type);
  if (instance == nullptr) {
    SetError(error, "Color Grade adjustment is missing: " + std::string{type->Text()});
    return std::nullopt;
  }
  target.owner_kind             = EditorParameterOwnerKind::ColorGrade;
  target.node_id                = selected_node_id;
  target.adjustment_instance_id = *instance;
  return target;
}

auto SelectedNodeProjectionTargets(const PipelineDocument& document, const NodeId& selected_node_id,
                                   std::string* error)
    -> std::optional<std::vector<EditorParameterTarget>> {
  const auto* node = document.Graph().FindNode(selected_node_id);
  if (node == nullptr) {
    SetError(error, "Selected node is missing: " + std::string(selected_node_id.Value()));
    return std::nullopt;
  }
  const auto kind = KindOf(*node);
  if (!kind.has_value()) {
    SetError(error, "Selected node kind is not supported: " + std::string(selected_node_id.Value()));
    return std::nullopt;
  }
  std::vector<EditorParameterTarget> targets;
  const auto                         table = EditorPanelAdapterTable::Production();
  targets.reserve(table.Adapters().size());
  for (const auto& adapter : table.Adapters()) {
    if (!AdjustmentFieldIsSupported(*kind, adapter.field_key)) {
      continue;
    }
    std::string field_error;
    auto        target = CompleteSelectedNodeParameterTarget(
        document, selected_node_id, std::string{adapter.field_key}, &field_error);
    if (!target.has_value()) {
      SetError(error, field_error);
      return std::nullopt;
    }
    targets.push_back(*target);
  }
  return targets;
}

auto ProjectSelectedNodePanelFields(const PipelineDocument& document, const NodeId& selected_node_id,
                                    std::uint64_t session_generation, EditorPanelProjection* out,
                                    std::string* error) -> bool {
  const auto targets = SelectedNodeProjectionTargets(document, selected_node_id, error);
  if (!targets.has_value()) {
    return false;
  }
  return ProjectEditorPanelFields(document, *targets, session_generation, out, error);
}

auto MakeEditorAdjustmentContext(const PipelineDocument& document, const NodeId& selected_node_id,
                                 EditorImageExifDisplay exif)
    -> std::optional<EditorAdjustmentContext> {
  const auto* node = document.Graph().FindNode(selected_node_id);
  if (node == nullptr) {
    return std::nullopt;
  }
  const auto kind = KindOf(*node);
  if (!kind.has_value()) {
    return std::nullopt;
  }
  EditorAdjustmentContext context;
  context.selected_node_id = selected_node_id;
  context.node_kind        = *kind;
  context.display_name     = std::string{node->DisplayName()};
  context.supported_panels = SupportedAdjustmentPanels(*kind);
  context.default_panel    = DefaultAdjustmentPanel(*kind);
  context.exif             = std::move(exif);
  return context;
}

}  // namespace alcedo
