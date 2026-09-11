//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mask_creation_controller.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "app/pipeline_document_history.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/i_node_model.hpp"
#include "edit/mask/brush_placement.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_list_selection.hpp"

namespace alcedo {
namespace {

[[nodiscard]] auto FiniteSample(const MaskCreationSample& sample) -> bool {
  return std::isfinite(sample.normalized.x) && std::isfinite(sample.normalized.y) &&
         std::isfinite(sample.reference_pixels.x) && std::isfinite(sample.reference_pixels.y);
}

[[nodiscard]] auto HandleMatchesKind(AnalyticMaskHandle handle, MaskSourceKind kind) -> bool {
  switch (handle) {
    case AnalyticMaskHandle::RadialCenter:
    case AnalyticMaskHandle::RadialMajor:
    case AnalyticMaskHandle::RadialMinor:
    case AnalyticMaskHandle::RadialRotate:
    case AnalyticMaskHandle::RadialInnerFeather:
    case AnalyticMaskHandle::RadialOuterFeather:
      return kind == MaskSourceKind::Radial;
    case AnalyticMaskHandle::LinearOrigin:
    case AnalyticMaskHandle::LinearDirection:
    case AnalyticMaskHandle::LinearStartBoundary:
    case AnalyticMaskHandle::LinearEndBoundary:
      return kind == MaskSourceKind::LinearGradient;
    case AnalyticMaskHandle::BrushMove:
      return kind == MaskSourceKind::Brush;
    case AnalyticMaskHandle::None:
      return false;
  }
  return false;
}

[[nodiscard]] auto CreationDisplayName(MaskSourceKind kind) -> std::string {
  switch (kind) {
    case MaskSourceKind::Brush:
      return "Brush";
    case MaskSourceKind::Radial:
      return "Radial";
    case MaskSourceKind::LinearGradient:
      return "Linear Gradient";
  }
  return "Mask";
}

[[nodiscard]] auto CollectBrushMaskIds(const ColorGradeNodeModel& grade) -> std::vector<MaskId> {
  std::vector<MaskId> ids;
  ids.reserve(grade.MaskCount());
  for (std::size_t i = 0; i < grade.MaskCount(); ++i) {
    if (GetMaskSourceKind(grade.MaskAt(i).source) == MaskSourceKind::Brush) {
      ids.push_back(grade.MaskAt(i).id);
    }
  }
  return ids;
}

[[nodiscard]] auto IsBrushPaintTool(EditorBrushTool tool) -> bool {
  return tool == EditorBrushTool::Paint || tool == EditorBrushTool::Erase;
}

[[nodiscard]] auto SourceFromJson(const nlohmann::json& source, const MaskId& mask_id)
    -> MaskSource {
  auto mask = MaskModelFromJson({{"id", std::string{mask_id.Value()}},
                                 {"display_name", ""},
                                 {"enabled", true},
                                 {"opacity", 1.0},
                                 {"invert", false},
                                 {"source", source},
                                 {"color_range", nullptr},
                                 {"luminance_range", nullptr}});
  return std::move(mask.source);
}

[[nodiscard]] auto CreationSourceValid(const MaskSource& source) -> bool {
  if (const auto* radial = std::get_if<RadialMaskSource>(&source)) {
    return RadialCreationIsValid(*radial);
  }
  if (const auto* linear = std::get_if<LinearGradientMaskSource>(&source)) {
    return LinearCreationIsValid(*linear);
  }
  return false;
}

/// Field-edit keys: Mask fields settle through SetMaskField; the Brush feather
/// is a source field and settles through ReplaceMaskSource.
[[nodiscard]] auto MaskFieldEditKeyIsValid(std::string_view field_key, MaskSourceKind kind)
    -> bool {
  if (field_key == kMaskFieldBrushFeather) {
    return kind == MaskSourceKind::Brush;
  }
  return field_key == kMaskFieldEnabled || field_key == kMaskFieldInvert ||
         field_key == kMaskFieldOpacity || field_key == kMaskFieldDisplayName;
}

[[nodiscard]] auto MaskFieldAffectsPixels(std::string_view field_key) -> bool {
  return field_key != kMaskFieldDisplayName;
}

[[nodiscard]] auto MaskFieldValueIsValid(std::string_view field_key, const nlohmann::json& value)
    -> bool {
  if (field_key == kMaskFieldEnabled || field_key == kMaskFieldInvert) {
    return value.is_boolean();
  }
  if (field_key == kMaskFieldOpacity) {
    return value.is_number() && std::isfinite(value.get<double>()) && value.get<double>() >= 0.0 &&
           value.get<double>() <= 1.0;
  }
  if (field_key == kMaskFieldDisplayName) {
    return value.is_string();
  }
  if (field_key == kMaskFieldBrushFeather) {
    return value.is_number() && std::isfinite(value.get<double>()) && value.get<double>() >= 0.0;
  }
  return false;
}

[[nodiscard]] auto MaskFieldValueJson(const MaskModel& mask, std::string_view field_key)
    -> nlohmann::json {
  if (field_key == kMaskFieldEnabled) {
    return mask.enabled;
  }
  if (field_key == kMaskFieldInvert) {
    return mask.invert;
  }
  if (field_key == kMaskFieldOpacity) {
    return mask.opacity;
  }
  if (field_key == kMaskFieldDisplayName) {
    return mask.display_name;
  }
  if (field_key == kMaskFieldBrushFeather) {
    if (const auto* brush = std::get_if<BrushMaskSource>(&mask.source)) {
      return brush->feather_radius;
    }
  }
  return nullptr;
}

}  // namespace

void EditorMaskCreationController::Bind(PipelineDocument&      document,
                                        MiniGitWorkingHistory& history) {
  if (document_ == &document && history_ == &history) {
    return;
  }
  if (open_) {
    throw std::runtime_error("cannot rebind the Mask creation controller during an open operation");
  }
  document_ = &document;
  history_  = &history;
}

void EditorMaskCreationController::SetInteractivePreview(std::function<void()> preview) {
  interactive_preview_ = std::move(preview);
}

void EditorMaskCreationController::SetSettlePublisher(
    std::function<bool(const PipelineEditBatch&, std::string*)> publish) {
  settle_publisher_ = std::move(publish);
}

void EditorMaskCreationController::DetachClosedDocument() {
  interactive_preview_ = {};
  settle_publisher_    = {};
  document_            = nullptr;
  history_             = nullptr;
  ResetMode();
}

auto EditorMaskCreationController::Reject(std::string error) const -> EditorMaskCreationResult {
  EditorMaskCreationResult result;
  result.error   = std::move(error);
  result.mask_id = mask_id_;
  return result;
}

auto EditorMaskCreationController::Ok() const -> EditorMaskCreationResult {
  EditorMaskCreationResult result;
  result.accepted = true;
  result.mask_id  = mask_id_;
  return result;
}

auto EditorMaskCreationController::Grade() -> ColorGradeNodeModel* {
  if (document_ == nullptr || node_id_.Empty()) {
    return nullptr;
  }
  return dynamic_cast<ColorGradeNodeModel*>(document_->Graph().FindNode(node_id_));
}

auto EditorMaskCreationController::Grade() const -> const ColorGradeNodeModel* {
  if (document_ == nullptr || node_id_.Empty()) {
    return nullptr;
  }
  return dynamic_cast<const ColorGradeNodeModel*>(document_->Graph().FindNode(node_id_));
}

auto EditorMaskCreationController::IdentityMatches(const MaskPointerIdentity& identity) const
    -> bool {
  return identity.device_id == pointer_.device_id && identity.point_id == pointer_.point_id &&
         identity.sequence_id == pointer_.sequence_id;
}

auto EditorMaskCreationController::AllocateMaskId() const -> MaskId {
  const auto* grade  = Grade();
  const char* prefix = "mask.radial.";
  if (kind_ == MaskSourceKind::LinearGradient) {
    prefix = "mask.linear.";
  } else if (kind_ == MaskSourceKind::Brush) {
    prefix = "mask.brush.";
  }
  for (std::uint32_t i = 1; i < 1000000; ++i) {
    MaskId id{std::string{prefix} + std::to_string(i)};
    if (grade != nullptr && grade->FindMask(id) == nullptr) {
      return id;
    }
  }
  return MaskId{};
}

auto EditorMaskCreationController::MakeCreationMask(const MaskSource& source) const -> MaskModel {
  MaskModel mask;
  mask.id           = mask_id_;
  mask.display_name = CreationDisplayName(kind_);
  mask.source       = source;
  return mask;
}

auto EditorMaskCreationController::LiveSourceJson() const -> nlohmann::json {
  const auto* grade = Grade();
  if (grade == nullptr) {
    return nullptr;
  }
  const auto* mask = grade->FindMask(mask_id_);
  if (mask == nullptr) {
    return nullptr;
  }
  return MaskModelToJson(*mask).at("source");
}

auto EditorMaskCreationController::SourcesEqual(const nlohmann::json& a,
                                                const nlohmann::json& b) const -> bool {
  return a == b;
}

auto EditorMaskCreationController::ApplyLiveSource(const MaskSource& source) -> bool {
  auto* grade = Grade();
  if (grade == nullptr) {
    return false;
  }
  try {
    grade->ReplaceMaskSource(mask_id_, source);
    draft_source_ = source;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

auto EditorMaskCreationController::InsertProvisional(const MaskSource& source) -> bool {
  auto* grade = Grade();
  if (grade == nullptr) {
    return false;
  }
  mask_id_ = AllocateMaskId();
  if (mask_id_.Empty()) {
    return false;
  }
  try {
    auto mask      = MakeCreationMask(source);
    display_index_ = static_cast<std::uint32_t>(grade->MaskCount());
    grade->AddMask(std::move(mask), display_index_);
    inserted_     = true;
    draft_source_ = source;
    return true;
  } catch (const std::exception&) {
    mask_id_ = MaskId{};
    return false;
  }
}

void EditorMaskCreationController::RestoreLive() {
  auto* grade = Grade();
  if (grade == nullptr) {
    return;
  }
  try {
    if (inserted_ && !mask_id_.Empty() && grade->FindMask(mask_id_) != nullptr) {
      grade->RemoveMask(mask_id_);
    } else if (!creating_ && !mask_id_.Empty() && grade->FindMask(mask_id_) != nullptr) {
      if (kind_ == MaskSourceKind::Brush) {
        (void)ApplyLiveSource(committed_brush_);
      } else if (!before_source_.is_null()) {
        grade->ReplaceMaskSource(mask_id_, SourceFromJson(before_source_, mask_id_));
      }
      if (!field_edit_key_.empty() && field_edit_key_ != kMaskFieldBrushFeather) {
        (void)ApplyLiveMaskField(field_edit_key_, before_field_value_);
      }
    }
  } catch (const std::exception&) {
  }
}

void EditorMaskCreationController::ClearOpenOperation() {
  brush_input_.Cancel();
  open_                    = false;
  terminated_              = true;
  handle_                  = AnalyticMaskHandle::None;
  pointer_                 = {};
  unwrapped_rotation_      = 0.0f;
  field_edit_key_.clear();
  before_field_value_      = nullptr;
  draft_source_.reset();
  published_draft_samples_ = 0;
}

void EditorMaskCreationController::ResetMode() {
  ClearOpenOperation();
  state_              = EditorMaskCreationState::Inactive;
  creating_           = false;
  inserted_           = false;
  mask_id_            = MaskId{};
  node_id_            = NodeId{};
  kind_               = MaskSourceKind::Radial;
  before_source_      = nullptr;
  session_            = {};
  terminated_         = false;
  brush_tool_         = EditorBrushTool::Idle;
  committed_brush_    = {};
  before_translation_ = {};
}

void EditorMaskCreationController::RequestInteractive(EditorMaskCreationResult& result) {
  if (interactive_preview_) {
    interactive_preview_();
  }
  result.interactive_preview = true;
}

auto EditorMaskCreationController::OverlayIsCreating() const -> bool {
  return (creating_ && open_) || brush_input_.IsOpen();
}

auto EditorMaskCreationController::CurrentSource() const -> std::optional<MaskSource> {
  if (draft_source_.has_value()) {
    return draft_source_;
  }
  const auto* grade = Grade();
  if (grade == nullptr || mask_id_.Empty()) {
    return std::nullopt;
  }
  const auto* mask = grade->FindMask(mask_id_);
  if (mask == nullptr) {
    return std::nullopt;
  }
  return mask->source;
}

auto EditorMaskCreationController::BeginCreation(MaskSourceKind kind, const NodeId& grade_id,
                                                 EditorSessionIdentity session, MaskId resume_mask)
    -> EditorMaskCreationResult {
  if (open_) {
    return Reject("finish or cancel the open Mask operation before changing tools");
  }
  if (kind != MaskSourceKind::Radial && kind != MaskSourceKind::LinearGradient &&
      kind != MaskSourceKind::Brush) {
    return Reject("creation supports Brush, Radial, and Linear Gradient");
  }
  if (document_ == nullptr || history_ == nullptr) {
    return Reject("Mask creation controller is not bound to a document");
  }
  node_id_    = grade_id;
  auto* grade = Grade();
  if (grade == nullptr) {
    node_id_ = NodeId{};
    return Reject("creation target is not a Color Grade");
  }
  if (kind == MaskSourceKind::Brush) {
    const auto brushes = CollectBrushMaskIds(*grade);
    MaskId     target  = resume_mask;
    if (brushes.empty()) {
      target = MaskId{};
    } else if (brushes.size() == 1 && target.Empty()) {
      target = brushes.front();
    } else if (brushes.size() > 1) {
      bool named = false;
      for (const auto& id : brushes) {
        if (id == target) {
          named = true;
          break;
        }
      }
      if (!named) {
        return Reject("select an existing Brush before painting");
      }
    }
    kind_    = MaskSourceKind::Brush;
    session_ = session;
    handle_  = AnalyticMaskHandle::None;
    draft_source_.reset();
    terminated_      = false;
    brush_tool_      = EditorBrushTool::Paint;
    committed_brush_ = {};
    if (target.Empty()) {
      mask_id_       = MaskId{};
      inserted_      = false;
      creating_      = true;
      before_source_ = nullptr;
      state_         = EditorMaskCreationState::Creating;
      return Ok();
    }
    const auto loaded = SelectMask(grade_id, target, session);
    if (!loaded.accepted) {
      return loaded;
    }
    brush_tool_ = EditorBrushTool::Paint;
    return loaded;
  }
  kind_          = kind;
  session_       = session;
  mask_id_       = MaskId{};
  inserted_      = false;
  creating_      = true;
  handle_        = AnalyticMaskHandle::None;
  before_source_ = nullptr;
  draft_source_.reset();
  terminated_ = false;
  brush_tool_ = EditorBrushTool::Idle;
  state_      = EditorMaskCreationState::Creating;
  return Ok();
}

auto EditorMaskCreationController::SelectMask(const NodeId& grade_id, const MaskId& mask_id,
                                              EditorSessionIdentity session)
    -> EditorMaskCreationResult {
  if (open_) {
    return Reject("finish or cancel the open Mask operation before changing selection");
  }
  if (document_ == nullptr || history_ == nullptr) {
    return Reject("Mask creation controller is not bound to a document");
  }
  node_id_    = grade_id;
  auto* grade = Grade();
  if (grade == nullptr) {
    node_id_ = NodeId{};
    return Reject("selection target is not a Color Grade");
  }
  const auto* mask = grade->FindMask(mask_id);
  if (mask == nullptr) {
    return Reject("Mask is missing");
  }
  const auto kind = GetMaskSourceKind(mask->source);
  if (kind != MaskSourceKind::Brush && kind != MaskSourceKind::Radial &&
      kind != MaskSourceKind::LinearGradient) {
    return Reject("Mask source kind is not selectable");
  }
  kind_            = kind;
  session_         = session;
  mask_id_         = mask_id;
  creating_        = false;
  inserted_        = false;
  handle_          = AnalyticMaskHandle::None;
  before_source_   = MaskModelToJson(*mask).at("source");
  draft_source_    = mask->source;
  terminated_      = false;
  brush_tool_      = EditorBrushTool::Idle;
  committed_brush_ = {};
  if (const auto* brush = std::get_if<BrushMaskSource>(&mask->source)) {
    committed_brush_ = *brush;
  }
  state_         = EditorMaskCreationState::Selected;
  auto result    = Ok();
  result.mask_id = mask_id_;
  return result;
}

auto EditorMaskCreationController::RemoveMask(const NodeId& grade_id, const MaskId& mask_id)
    -> EditorMaskCreationResult {
  if (document_ == nullptr || history_ == nullptr) {
    return Reject("Mask creation controller is not bound to a document");
  }
  if (grade_id.Empty() || mask_id.Empty()) {
    return Reject("RemoveMask requires a Color Grade and Mask identity");
  }
  if (open_ && mask_id_ == mask_id) {
    if (inserted_ && creating_) {
      return CancelMaskInput();
    }
    const auto cancelled = CancelMaskInput();
    if (!cancelled.accepted) {
      return cancelled;
    }
  }
  node_id_    = grade_id;
  auto* grade = Grade();
  if (grade == nullptr) {
    node_id_ = NodeId{};
    return Reject("removal target is not a Color Grade");
  }
  std::vector<MaskId> ordered;
  ordered.reserve(grade->MaskCount());
  std::optional<std::uint32_t> index;
  for (std::size_t i = 0; i < grade->MaskCount(); ++i) {
    const auto& mask = grade->MaskAt(i);
    ordered.push_back(mask.id);
    if (mask.id == mask_id) {
      index = static_cast<std::uint32_t>(i);
    }
  }
  if (!index.has_value()) {
    return Reject("Mask is missing");
  }
  const auto next_selection = MaskIdAfterDeletion(ordered, mask_id, mask_id_);
  const auto stored         = MaskModelToJson(grade->MaskAt(*index));
  const auto batch          = MakeRemoveMaskBatch(grade_id, mask_id, stored, *index);
  try {
    grade->RemoveMask(mask_id);
  } catch (const std::exception& ex) {
    return Reject(ex.what());
  }
  std::string settle_error;
  if (!PublishSettledBatch(batch, &settle_error)) {
    try {
      grade->AddMask(MaskModelFromJson(stored), *index);
    } catch (const std::exception&) {
    }
    return Reject(settle_error.empty() ? std::string{"RemoveMask history publish failed"}
                                       : settle_error);
  }
  last_removed_mask_id_ = mask_id;
  if (next_selection.Empty()) {
    mask_id_ = MaskId{};
    draft_source_.reset();
    before_source_ = nullptr;
    creating_      = false;
    handle_        = AnalyticMaskHandle::None;
    state_         = EditorMaskCreationState::Inactive;
  } else if (next_selection != mask_id_) {
    const auto loaded = SelectMask(grade_id, next_selection, session_);
    if (!loaded.accepted) {
      mask_id_ = MaskId{};
      draft_source_.reset();
      before_source_ = nullptr;
      state_         = EditorMaskCreationState::Inactive;
    }
  }
  auto result              = Ok();
  result.mask_id           = mask_id_;
  result.committed         = true;
  result.quality_requested = true;
  RequestInteractive(result);
  return result;
}

auto EditorMaskCreationController::BeginMaskFieldEdit(std::string field_key)
    -> EditorMaskCreationResult {
  if (open_) {
    return Reject("finish or cancel the open Mask operation before editing Mask values");
  }
  if (state_ == EditorMaskCreationState::Settling) {
    return Reject("Mask tool is unavailable until settle is acknowledged");
  }
  if (state_ != EditorMaskCreationState::Selected || mask_id_.Empty()) {
    return Reject("Mask value edits require a selected existing Mask");
  }
  if (!MaskFieldEditKeyIsValid(field_key, kind_)) {
    return Reject("Mask value key is not editable: " + field_key);
  }
  auto* grade = Grade();
  if (grade == nullptr) {
    return Reject("selection target is not a Color Grade");
  }
  const auto* mask = grade->FindMask(mask_id_);
  if (mask == nullptr) {
    return Reject("Mask is missing");
  }
  field_edit_key_     = std::move(field_key);
  before_field_value_ = MaskFieldValueJson(*mask, field_edit_key_);
  if (field_edit_key_ == kMaskFieldBrushFeather) {
    before_source_   = MaskModelToJson(*mask).at("source");
    committed_brush_ = *std::get_if<BrushMaskSource>(&mask->source);
    draft_source_    = mask->source;
  }
  handle_        = AnalyticMaskHandle::None;
  open_          = true;
  terminated_    = false;
  state_         = EditorMaskCreationState::Editing;
  auto result    = Ok();
  result.mask_id = mask_id_;
  return result;
}

auto EditorMaskCreationController::ApplyLiveMaskField(const std::string&    field_key,
                                                      const nlohmann::json& value) -> bool {
  auto* grade = Grade();
  if (grade == nullptr) {
    return false;
  }
  auto* mask = grade->FindMask(mask_id_);
  if (mask == nullptr) {
    return false;
  }
  try {
    if (field_key == kMaskFieldEnabled) {
      grade->SetMaskEnabled(mask_id_, value.get<bool>());
    } else if (field_key == kMaskFieldInvert) {
      grade->SetMaskInvert(mask_id_, value.get<bool>());
    } else if (field_key == kMaskFieldOpacity) {
      grade->SetMaskOpacity(mask_id_, value.get<float>());
    } else if (field_key == kMaskFieldDisplayName) {
      mask->display_name = value.get<std::string>();
    } else if (field_key == kMaskFieldBrushFeather) {
      if (const auto* brush = std::get_if<BrushMaskSource>(&mask->source)) {
        auto next           = *brush;
        next.feather_radius = value.get<float>();
        grade->ReplaceMaskSource(mask_id_, std::move(next));
        draft_source_ = grade->FindMask(mask_id_)->source;
      } else {
        return false;
      }
    } else {
      return false;
    }
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

auto EditorMaskCreationController::ApplyMaskFieldValue(std::string    field_key,
                                                       nlohmann::json after_value)
    -> EditorMaskCreationResult {
  if (open_) {
    if (field_edit_key_.empty() || field_edit_key_ != field_key) {
      return Reject("Mask value does not match the open Mask edit");
    }
    if (!MaskFieldValueIsValid(field_key, after_value)) {
      return Reject("Mask value is invalid for '" + field_key + "'");
    }
    if (!ApplyLiveMaskField(field_key, after_value)) {
      return Reject("Mask value apply was rejected");
    }
    auto result    = Ok();
    result.mask_id = mask_id_;
    if (MaskFieldAffectsPixels(field_key)) {
      RequestInteractive(result);
    }
    return result;
  }
  if (state_ == EditorMaskCreationState::Settling) {
    return Reject("Mask tool is unavailable until settle is acknowledged");
  }
  if (state_ != EditorMaskCreationState::Selected || mask_id_.Empty()) {
    return Reject("Mask value edits require a selected existing Mask");
  }
  if (!MaskFieldEditKeyIsValid(field_key, kind_)) {
    return Reject("Mask value key is not editable: " + field_key);
  }
  if (!MaskFieldValueIsValid(field_key, after_value)) {
    return Reject("Mask value is invalid for '" + field_key + "'");
  }
  auto* grade = Grade();
  if (grade == nullptr) {
    return Reject("selection target is not a Color Grade");
  }
  const auto* mask = grade->FindMask(mask_id_);
  if (mask == nullptr) {
    return Reject("Mask is missing");
  }
  const auto before = MaskFieldValueJson(*mask, field_key);
  if (before == after_value) {
    auto result    = Ok();
    result.mask_id = mask_id_;
    return result;
  }
  const auto before_source = MaskModelToJson(*mask).at("source");
  if (!ApplyLiveMaskField(field_key, after_value)) {
    return Reject("Mask value apply was rejected");
  }
  PipelineEditBatch batch;
  if (field_key == kMaskFieldBrushFeather) {
    const auto after_source = LiveSourceJson();
    if (after_source.is_null()) {
      state_ = EditorMaskCreationState::Failed;
      return Reject("Mask source is missing at settle");
    }
    batch = MakeReplaceMaskSourceBatch(node_id_, mask_id_, before_source, after_source);
  } else {
    batch = MakeSetMaskFieldBatch(node_id_, mask_id_, field_key, before, after_value);
  }
  std::string settle_error;
  if (!PublishSettledBatch(batch, &settle_error)) {
    (void)ApplyLiveMaskField(field_key, before);
    state_ = EditorMaskCreationState::Failed;
    return Reject(settle_error.empty() ? std::string{"Mask value history publish failed"}
                                       : settle_error);
  }
  if (const auto* updated = grade->FindMask(mask_id_)) {
    if (const auto* brush = std::get_if<BrushMaskSource>(&updated->source)) {
      committed_brush_ = *brush;
    }
    before_source_ = MaskModelToJson(*updated).at("source");
    draft_source_  = updated->source;
  }
  auto result              = Ok();
  result.mask_id           = mask_id_;
  result.committed         = true;
  result.quality_requested = MaskFieldAffectsPixels(field_key);
  return result;
}

auto EditorMaskCreationController::PublishMaskFieldEdit() -> EditorMaskCreationResult {
  const auto field_key = field_edit_key_;
  auto*      grade     = Grade();
  if (grade == nullptr) {
    state_ = EditorMaskCreationState::Failed;
    return Reject("Mask value settle is missing the Grade");
  }
  const auto* mask = grade->FindMask(mask_id_);
  if (mask == nullptr) {
    RestoreLive();
    state_ = EditorMaskCreationState::Failed;
    return Reject("Mask is missing at value settle");
  }
  const auto after = MaskFieldValueJson(*mask, field_key);
  if (after == before_field_value_) {
    ClearOpenOperation();
    state_ = EditorMaskCreationState::Selected;
    return Ok();
  }
  PipelineEditBatch batch;
  if (field_key == kMaskFieldBrushFeather) {
    const auto after_source = LiveSourceJson();
    if (after_source.is_null()) {
      RestoreLive();
      state_ = EditorMaskCreationState::Failed;
      return Reject("Mask source is missing at value settle");
    }
    batch = MakeReplaceMaskSourceBatch(node_id_, mask_id_, before_source_, after_source);
  } else {
    batch = MakeSetMaskFieldBatch(node_id_, mask_id_, field_key, before_field_value_, after);
  }
  std::string settle_error;
  if (!PublishSettledBatch(batch, &settle_error)) {
    RestoreLive();
    state_ = EditorMaskCreationState::Failed;
    return Reject(settle_error.empty() ? std::string{"Mask value history publish failed"}
                                       : settle_error);
  }
  ClearOpenOperation();
  if (const auto* updated = grade->FindMask(mask_id_)) {
    if (const auto* brush = std::get_if<BrushMaskSource>(&updated->source)) {
      committed_brush_ = *brush;
    }
    before_source_ = MaskModelToJson(*updated).at("source");
    draft_source_  = updated->source;
  }
  state_                   = EditorMaskCreationState::Settling;
  auto result              = Ok();
  result.mask_id           = mask_id_;
  result.committed         = true;
  result.quality_requested = MaskFieldAffectsPixels(field_key);
  return result;
}

auto EditorMaskCreationController::BeginMaskInput(MaskCreationSample  sample,
                                                  MaskPointerIdentity identity,
                                                  MaskSourceKind      expected_source,
                                                  float default_feather_reference_px)
    -> EditorMaskCreationResult {
  if (state_ == EditorMaskCreationState::Settling) {
    return Reject("Mask tool is unavailable until settle is acknowledged");
  }
  if (expected_source != kind_) {
    return Reject("stale Mask tool dispatch does not match the armed source kind");
  }
  if (kind_ == MaskSourceKind::Brush && IsBrushPaintTool(brush_tool_)) {
    const bool armed = (state_ == EditorMaskCreationState::Creating && !open_) ||
                       (state_ == EditorMaskCreationState::Selected && !open_ && !mask_id_.Empty());
    if (!armed) {
      return Reject("BeginMaskInput requires an armed Brush paint or erase tool");
    }
    if (brush_tool_ == EditorBrushTool::Erase && mask_id_.Empty()) {
      return Reject("Brush erase requires an existing Brush");
    }
    if (!FiniteSample(sample) || !sample.inside_photograph) {
      return Reject("Mask press must lie inside the photograph");
    }
    pointer_                = identity;
    press_normalized_       = sample.normalized;
    press_reference_pixels_ = sample.reference_pixels;
    return BeginBrushStroke(sample, default_feather_reference_px);
  }
  if (state_ != EditorMaskCreationState::Creating || open_) {
    return Reject("BeginMaskInput requires an armed creation tool");
  }
  if (!FiniteSample(sample) || !sample.inside_photograph) {
    return Reject("Mask press must lie inside the photograph");
  }
  pointer_            = identity;
  press_normalized_   = sample.normalized;
  handle_             = AnalyticMaskHandle::None;
  inserted_           = false;
  mask_id_            = MaskId{};
  before_source_      = nullptr;
  unwrapped_rotation_ = 0.0f;
  draft_source_       = kind_ == MaskSourceKind::Radial
                            ? MaskSource{RadialFromCenterOut(sample.normalized, sample.normalized)}
                            : MaskSource{LinearFromEndpoints(sample.normalized, sample.normalized)};
  open_               = true;
  terminated_         = false;
  state_              = EditorMaskCreationState::Editing;
  return Ok();
}

auto EditorMaskCreationController::BeginMaskMove(AnalyticMaskHandle  handle,
                                                 MaskCreationSample  sample,
                                                 MaskPointerIdentity identity,
                                                 MaskSourceKind      expected_source)
    -> EditorMaskCreationResult {
  if (state_ == EditorMaskCreationState::Settling) {
    return Reject("Mask tool is unavailable until settle is acknowledged");
  }
  if (expected_source != kind_) {
    return Reject("stale Mask tool dispatch does not match the selected source kind");
  }
  if (state_ != EditorMaskCreationState::Selected || open_) {
    return Reject("BeginMaskMove requires a selected existing Mask");
  }
  if (!HandleMatchesKind(handle, kind_)) {
    return Reject("handle does not match the selected Mask kind");
  }
  if (kind_ == MaskSourceKind::Brush) {
    if (brush_tool_ != EditorBrushTool::Move || handle != AnalyticMaskHandle::BrushMove) {
      return Reject("Brush move requires Move mode");
    }
  }
  if (!FiniteSample(sample) || !sample.inside_photograph) {
    return Reject("Mask press must lie inside the photograph");
  }
  auto* grade = Grade();
  if (grade == nullptr) {
    return Reject("selection target is not a Color Grade");
  }
  const auto* mask = grade->FindMask(mask_id_);
  if (mask == nullptr) {
    return Reject("Mask is missing");
  }
  pointer_          = identity;
  press_normalized_ = sample.normalized;
  handle_           = handle;
  creating_         = false;
  inserted_         = false;
  before_source_    = MaskModelToJson(*mask).at("source");
  draft_source_     = mask->source;
  if (const auto* radial = std::get_if<RadialMaskSource>(&mask->source)) {
    unwrapped_rotation_ = radial->rotation;
  } else {
    unwrapped_rotation_ = 0.0f;
  }
  if (const auto* brush = std::get_if<BrushMaskSource>(&mask->source)) {
    committed_brush_        = *brush;
    before_translation_     = brush->placement_translation;
    press_reference_pixels_ = sample.reference_pixels;
  }
  open_          = true;
  terminated_    = false;
  state_         = EditorMaskCreationState::Editing;
  auto result    = Ok();
  result.mask_id = mask_id_;
  return result;
}

auto EditorMaskCreationController::UpdateCreation(const MaskCreationSample& sample)
    -> EditorMaskCreationResult {
  MaskSource next;
  switch (kind_) {
    case MaskSourceKind::Radial:
      next = RadialFromCenterOut(press_normalized_, sample.normalized);
      break;
    case MaskSourceKind::LinearGradient:
      next = LinearFromEndpoints(press_normalized_, sample.normalized);
      break;
    case MaskSourceKind::Brush:
      return Reject("Brush input must route through the Brush stroke path");
  }
  draft_source_ = next;
  if (!CreationSourceValid(next)) {
    return Ok();
  }
  if (!inserted_) {
    if (!InsertProvisional(next)) {
      return Reject("provisional Mask insertion failed");
    }
  } else if (!ApplyLiveSource(next)) {
    return Reject("provisional Mask source was rejected");
  }
  auto result    = Ok();
  result.mask_id = mask_id_;
  RequestInteractive(result);
  return result;
}

auto EditorMaskCreationController::UpdateExisting(const MaskCreationSample& sample)
    -> EditorMaskCreationResult {
  auto* grade = Grade();
  if (grade == nullptr) {
    return Reject("selection target is not a Color Grade");
  }
  const auto* mask = grade->FindMask(mask_id_);
  if (mask == nullptr) {
    return Reject("Mask is missing");
  }
  auto next =
      ApplyAnalyticMaskHandle(handle_, mask->source, sample.normalized, unwrapped_rotation_);
  if (!next) {
    return Reject("analytic handle update is not valid");
  }
  if (*next == mask->source) {
    auto result    = Ok();
    result.mask_id = mask_id_;
    return result;
  }
  if (!ApplyLiveSource(*next)) {
    return Reject("analytic source update was rejected");
  }
  auto result    = Ok();
  result.mask_id = mask_id_;
  RequestInteractive(result);
  return result;
}

auto EditorMaskCreationController::AppendMaskInput(MaskCreationSample  sample,
                                                   MaskPointerIdentity identity)
    -> EditorMaskCreationResult {
  if (!open_ || terminated_) {
    return Reject("no open Mask input sequence");
  }
  if (!IdentityMatches(identity)) {
    return Reject("pointer identity does not match the captured sequence");
  }
  if (!FiniteSample(sample)) {
    return Reject("Mask sample is not finite");
  }
  // Brush strokes keep `creating_` armed until PublishAddMask, so the open
  // Brush input must win over the analytic creation path; otherwise the first
  // stroke's appends would replace the live source with a Linear draft.
  if (kind_ == MaskSourceKind::Brush && brush_input_.IsOpen()) {
    return UpdateBrushPaint(sample);
  }
  if (kind_ == MaskSourceKind::Brush && handle_ == AnalyticMaskHandle::BrushMove) {
    return UpdateBrushMove(sample);
  }
  if (creating_) {
    return UpdateCreation(sample);
  }
  return UpdateExisting(sample);
}

auto EditorMaskCreationController::PublishSettledBatch(const PipelineEditBatch& batch,
                                                       std::string*             error) -> bool {
  if (settle_publisher_) {
    return settle_publisher_(batch, error);
  }
  if (history_ == nullptr) {
    if (error != nullptr) {
      *error = "Mask creation controller is not bound to history";
    }
    return false;
  }
  const auto append = history_->AppendEdit(batch);
  if (append.committed) {
    return true;
  }
  if (error != nullptr) {
    *error = append.error;
  }
  return false;
}

auto EditorMaskCreationController::PublishAddMask() -> EditorMaskCreationResult {
  auto* grade = Grade();
  if (grade == nullptr || history_ == nullptr) {
    RestoreLive();
    state_ = EditorMaskCreationState::Failed;
    return Reject("AddMask settle is missing the Grade or history");
  }
  const auto* mask = grade->FindMask(mask_id_);
  if (mask == nullptr) {
    RestoreLive();
    state_ = EditorMaskCreationState::Failed;
    return Reject("provisional Mask is missing at settle");
  }
  const auto  batch = MakeAddMaskBatch(node_id_, mask_id_, MaskModelToJson(*mask), display_index_);
  std::string settle_error;
  if (!PublishSettledBatch(batch, &settle_error)) {
    RestoreLive();
    inserted_ = false;
    state_    = EditorMaskCreationState::Failed;
    return Reject(settle_error.empty() ? std::string{"AddMask history publish failed"}
                                       : settle_error);
  }
  ClearOpenOperation();
  creating_                = false;
  inserted_                = false;
  before_source_           = MaskModelToJson(*mask).at("source");
  draft_source_            = mask->source;
  state_                   = EditorMaskCreationState::Settling;
  auto result              = Ok();
  result.mask_id           = mask_id_;
  result.committed         = true;
  result.quality_requested = true;
  return result;
}

auto EditorMaskCreationController::PublishReplaceSource() -> EditorMaskCreationResult {
  const auto after = LiveSourceJson();
  if (after.is_null()) {
    state_ = EditorMaskCreationState::Failed;
    return Reject("Mask source is missing at settle");
  }
  if (SourcesEqual(before_source_, after)) {
    ClearOpenOperation();
    state_ = EditorMaskCreationState::Selected;
    return Ok();
  }
  if (history_ == nullptr) {
    RestoreLive();
    state_ = EditorMaskCreationState::Failed;
    return Reject("ReplaceMaskSource settle is missing history");
  }
  const auto  batch = MakeReplaceMaskSourceBatch(node_id_, mask_id_, before_source_, after);
  std::string settle_error;
  if (!PublishSettledBatch(batch, &settle_error)) {
    RestoreLive();
    state_ = EditorMaskCreationState::Failed;
    return Reject(settle_error.empty() ? std::string{"ReplaceMaskSource history publish failed"}
                                       : settle_error);
  }
  ClearOpenOperation();
  before_source_ = after;
  if (Grade() != nullptr) {
    if (const auto* mask = Grade()->FindMask(mask_id_)) {
      draft_source_ = mask->source;
    }
  }
  state_                   = EditorMaskCreationState::Settling;
  auto result              = Ok();
  result.mask_id           = mask_id_;
  result.committed         = true;
  result.quality_requested = true;
  return result;
}

auto EditorMaskCreationController::FinishMaskInput() -> EditorMaskCreationResult {
  if (!open_) {
    auto result    = Ok();
    result.mask_id = mask_id_;
    return result;
  }
  if (!field_edit_key_.empty()) {
    return PublishMaskFieldEdit();
  }
  if (creating_ && !inserted_) {
    ClearOpenOperation();
    draft_source_.reset();
    state_ = EditorMaskCreationState::Creating;
    return Ok();
  }
  if (kind_ == MaskSourceKind::Brush && brush_input_.IsOpen()) {
    return FinishBrushStroke();
  }
  if (kind_ == MaskSourceKind::Brush && handle_ == AnalyticMaskHandle::BrushMove) {
    return PublishSetTranslation();
  }
  if (creating_ && inserted_) {
    return PublishAddMask();
  }
  return PublishReplaceSource();
}

auto EditorMaskCreationController::CancelMaskInput() -> EditorMaskCreationResult {
  if (open_) {
    RestoreLive();
  }
  const auto id = mask_id_;
  ClearOpenOperation();
  if (creating_) {
    inserted_ = false;
    mask_id_  = MaskId{};
    draft_source_.reset();
    before_source_ = nullptr;
    state_         = EditorMaskCreationState::Inactive;
  } else {
    state_ = EditorMaskCreationState::Selected;
    if (Grade() != nullptr && Grade()->FindMask(id) != nullptr) {
      mask_id_       = id;
      before_source_ = MaskModelToJson(*Grade()->FindMask(id)).at("source");
      draft_source_  = Grade()->FindMask(id)->source;
    }
  }
  auto result    = Ok();
  result.mask_id = mask_id_;
  return result;
}

auto EditorMaskCreationController::FinishCreationMode() -> EditorMaskCreationResult {
  auto result = open_ ? FinishMaskInput() : Ok();
  if (!result.accepted && open_) {
    return result;
  }
  const bool committed         = result.committed;
  const bool quality_requested = result.quality_requested;
  const auto mask_id           = result.mask_id;
  ResetMode();
  result                   = Ok();
  result.committed         = committed;
  result.quality_requested = quality_requested;
  result.mask_id           = mask_id;
  return result;
}

auto EditorMaskCreationController::CancelCreationMode() -> EditorMaskCreationResult {
  auto result = CancelMaskInput();
  ResetMode();
  result.mask_id = MaskId{};
  return result;
}

auto EditorMaskCreationController::SetBrushTool(EditorBrushTool tool) -> EditorMaskCreationResult {
  if (open_) {
    return Reject("finish or cancel the open Mask operation before changing the Brush tool");
  }
  if (tool != EditorBrushTool::Idle && kind_ != MaskSourceKind::Brush) {
    return Reject("Brush tools require a Brush Mask");
  }
  if (tool == EditorBrushTool::Move && (creating_ || mask_id_.Empty())) {
    return Reject("Brush move requires an existing Brush");
  }
  if (tool == EditorBrushTool::Erase && mask_id_.Empty()) {
    return Reject("Brush erase requires an existing Brush");
  }
  brush_tool_ = tool;
  if (IsBrushPaintTool(tool)) {
    brush_input_.SetStrokeMode(BrushStrokeModeFromTool());
  }
  auto result    = Ok();
  result.mask_id = mask_id_;
  return result;
}

auto EditorMaskCreationController::SetBrushStrokeParameters(float radius, float strength,
                                                            float hardness)
    -> EditorMaskCreationResult {
  if (radius == brush_input_.radius() && strength == brush_input_.strength() &&
      hardness == brush_input_.hardness()) {
    // Queued appends re-send the current parameters; an unchanged set must not
    // republish the draft or schedule a render.
    auto result    = Ok();
    result.mask_id = mask_id_;
    return result;
  }
  try {
    brush_input_.SetStrokeParameters(radius, strength, hardness);
  } catch (const std::exception& ex) {
    return Reject(ex.what());
  }
  if (!brush_input_.IsOpen()) {
    auto result    = Ok();
    result.mask_id = mask_id_;
    return result;
  }
  if (!ApplyLiveBrushDraft()) {
    return Reject("Brush parameter update was rejected");
  }
  published_draft_samples_ = brush_input_.DraftSamples().size();
  auto result    = Ok();
  result.mask_id = mask_id_;
  RequestInteractive(result);
  return result;
}

auto EditorMaskCreationController::BrushStrokeModeFromTool() const -> BrushStrokeMode {
  return brush_tool_ == EditorBrushTool::Erase ? BrushStrokeMode::Erase : BrushStrokeMode::Paint;
}

auto EditorMaskCreationController::AllocateStrokeId() const -> StrokeId {
  for (std::uint32_t i = 1; i < 1000000; ++i) {
    StrokeId id{std::string{"stroke."} + std::to_string(i)};
    if (FindBrushStrokeIndex(committed_brush_.strokes, id) == committed_brush_.strokes.size()) {
      return id;
    }
  }
  return StrokeId{};
}

auto EditorMaskCreationController::ComposeDraftBrush() const -> BrushMaskSource {
  auto brush = committed_brush_;
  if (auto draft = brush_input_.DraftStroke()) {
    const auto index = FindBrushStrokeIndex(brush.strokes, draft->id);
    if (index != brush.strokes.size()) {
      brush.strokes.erase(brush.strokes.begin() + static_cast<std::ptrdiff_t>(index));
    }
    brush.strokes.push_back(std::move(*draft));
  }
  return brush;
}

auto EditorMaskCreationController::ApplyLiveBrushDraft() -> bool {
  const MaskSource source{ComposeDraftBrush()};
  if (creating_ && !inserted_) {
    return InsertProvisional(source);
  }
  return ApplyLiveSource(source);
}

auto EditorMaskCreationController::BeginBrushStroke(const MaskCreationSample& sample,
                                                    float default_feather_reference_px)
    -> EditorMaskCreationResult {
  auto* grade = Grade();
  if (grade == nullptr) {
    return Reject("creation target is not a Color Grade");
  }
  Vector2 translation{};
  if (!mask_id_.Empty()) {
    const auto* mask = grade->FindMask(mask_id_);
    if (mask == nullptr) {
      return Reject("Mask is missing");
    }
    const auto* brush = std::get_if<BrushMaskSource>(&mask->source);
    if (brush == nullptr) {
      return Reject("selected Mask is not a Brush");
    }
    committed_brush_ = *brush;
    before_source_   = MaskModelToJson(*mask).at("source");
    translation      = brush->placement_translation;
  } else {
    committed_brush_ = {};
    if (std::isfinite(default_feather_reference_px) && default_feather_reference_px > 0.0f) {
      committed_brush_.feather_radius = default_feather_reference_px;
    }
    before_source_ = nullptr;
  }
  const auto stroke_id = AllocateStrokeId();
  if (stroke_id.Empty()) {
    return Reject("Brush StrokeId allocation failed");
  }
  try {
    brush_input_.SetStrokeMode(BrushStrokeModeFromTool());
    brush_input_.Begin(stroke_id, sample.reference_pixels, translation);
  } catch (const std::exception& ex) {
    return Reject(ex.what());
  }
  handle_     = AnalyticMaskHandle::None;
  open_       = true;
  terminated_ = false;
  state_      = EditorMaskCreationState::Painting;
  if (!ApplyLiveBrushDraft()) {
    brush_input_.Cancel();
    open_ = false;
    return Reject("provisional Brush stroke was rejected");
  }
  published_draft_samples_ = brush_input_.DraftSamples().size();
  auto result    = Ok();
  result.mask_id = mask_id_;
  RequestInteractive(result);
  return result;
}

auto EditorMaskCreationController::UpdateBrushPaint(const MaskCreationSample& sample)
    -> EditorMaskCreationResult {
  try {
    brush_input_.Append(sample.reference_pixels);
  } catch (const std::exception& ex) {
    return Reject(ex.what());
  }
  const auto draft_count = brush_input_.DraftSamples().size();
  if (draft_count == published_draft_samples_) {
    // The append emitted no new canonical dab: the live source is unchanged, so
    // neither a republish nor an Interactive frame is needed.
    auto result    = Ok();
    result.mask_id = mask_id_;
    return result;
  }
  if (!ApplyLiveBrushDraft()) {
    return Reject("provisional Brush source was rejected");
  }
  published_draft_samples_ = draft_count;
  auto result    = Ok();
  result.mask_id = mask_id_;
  RequestInteractive(result);
  return result;
}

auto EditorMaskCreationController::UpdateBrushMove(const MaskCreationSample& sample)
    -> EditorMaskCreationResult {
  auto* grade = Grade();
  if (grade == nullptr) {
    return Reject("selection target is not a Color Grade");
  }
  if (grade->FindMask(mask_id_) == nullptr) {
    return Reject("Mask is missing");
  }
  const auto after   = BrushPlacementForReferenceDrag(before_translation_, press_reference_pixels_,
                                                      sample.reference_pixels);
  const auto current = grade->BrushPlacementTranslation(mask_id_);
  if (current == after) {
    auto result    = Ok();
    result.mask_id = mask_id_;
    return result;
  }
  try {
    grade->SetBrushTranslation(MakeBrushTranslationCommand(node_id_, mask_id_, current, after,
                                                           grade->MaskContentRevision(mask_id_)));
  } catch (const std::exception& ex) {
    return Reject(ex.what());
  }
  draft_source_  = grade->FindMask(mask_id_)->source;
  auto result    = Ok();
  result.mask_id = mask_id_;
  RequestInteractive(result);
  return result;
}

auto EditorMaskCreationController::FinishBrushStroke() -> EditorMaskCreationResult {
  BrushStroke stroke;
  try {
    stroke = brush_input_.Finish();
  } catch (const std::exception& ex) {
    RestoreLive();
    state_ = EditorMaskCreationState::Failed;
    return Reject(ex.what());
  }
  const bool unchanged =
      stroke.mode == BrushStrokeMode::Paint
          ? !BrushStrokeHasCoverageEffect(stroke)
          : !BrushEraseOverlapsPaint(stroke, committed_brush_);
  if (unchanged) {
    // Confirmed no-change stroke: restore the live draft and stay armed without
    // producing a history commit.
    const auto id = mask_id_;
    RestoreLive();
    ClearOpenOperation();
    if (creating_) {
      inserted_      = false;
      mask_id_       = MaskId{};
      before_source_ = nullptr;
      state_         = EditorMaskCreationState::Creating;
    } else {
      state_ = EditorMaskCreationState::Selected;
      if (Grade() != nullptr && Grade()->FindMask(id) != nullptr) {
        mask_id_       = id;
        before_source_ = MaskModelToJson(*Grade()->FindMask(id)).at("source");
        draft_source_  = Grade()->FindMask(id)->source;
      }
    }
    return Ok();
  }
  auto       brush = committed_brush_;
  const auto index = FindBrushStrokeIndex(brush.strokes, stroke.id);
  if (index == brush.strokes.size()) {
    brush.strokes.push_back(stroke);
  } else {
    brush.strokes[index] = stroke;
  }
  if (!ApplyLiveSource(brush)) {
    RestoreLive();
    inserted_ = false;
    state_    = EditorMaskCreationState::Failed;
    return Reject("Brush stroke source was rejected at settle");
  }
  if (creating_ && inserted_) {
    return PublishAddMask();
  }
  return PublishAppendStroke(std::move(stroke));
}

auto EditorMaskCreationController::PublishAppendStroke(BrushStroke stroke)
    -> EditorMaskCreationResult {
  if (history_ == nullptr) {
    RestoreLive();
    state_ = EditorMaskCreationState::Failed;
    return Reject("AppendBrushStroke settle is missing history");
  }
  const auto  batch = MakeAppendBrushStrokeBatch(node_id_, mask_id_, std::move(stroke));
  std::string settle_error;
  if (!PublishSettledBatch(batch, &settle_error)) {
    RestoreLive();
    state_ = EditorMaskCreationState::Failed;
    return Reject(settle_error.empty() ? std::string{"AppendBrushStroke history publish failed"}
                                       : settle_error);
  }
  ClearOpenOperation();
  creating_ = false;
  inserted_ = false;
  if (Grade() != nullptr) {
    if (const auto* mask = Grade()->FindMask(mask_id_)) {
      if (const auto* brush = std::get_if<BrushMaskSource>(&mask->source)) {
        committed_brush_ = *brush;
      }
      before_source_ = MaskModelToJson(*mask).at("source");
      draft_source_  = mask->source;
    }
  }
  state_                   = EditorMaskCreationState::Settling;
  auto result              = Ok();
  result.mask_id           = mask_id_;
  result.committed         = true;
  result.quality_requested = true;
  return result;
}

auto EditorMaskCreationController::PublishSetTranslation() -> EditorMaskCreationResult {
  auto* grade = Grade();
  if (grade == nullptr) {
    RestoreLive();
    state_ = EditorMaskCreationState::Failed;
    return Reject("SetBrushTranslation settle is missing the Grade");
  }
  Vector2 after{};
  try {
    after = grade->BrushPlacementTranslation(mask_id_);
  } catch (const std::exception& ex) {
    RestoreLive();
    state_ = EditorMaskCreationState::Failed;
    return Reject(ex.what());
  }
  if (after == before_translation_) {
    ClearOpenOperation();
    state_ = EditorMaskCreationState::Selected;
    return Ok();
  }
  const auto  batch = MakeSetBrushTranslationBatch(node_id_, mask_id_, before_translation_, after);
  std::string settle_error;
  if (!PublishSettledBatch(batch, &settle_error)) {
    RestoreLive();
    state_ = EditorMaskCreationState::Failed;
    return Reject(settle_error.empty() ? std::string{"SetBrushTranslation history publish failed"}
                                       : settle_error);
  }
  ClearOpenOperation();
  if (const auto* mask = grade->FindMask(mask_id_)) {
    if (const auto* brush = std::get_if<BrushMaskSource>(&mask->source)) {
      committed_brush_ = *brush;
    }
    before_source_ = MaskModelToJson(*mask).at("source");
    draft_source_  = mask->source;
  }
  state_                   = EditorMaskCreationState::Settling;
  auto result              = Ok();
  result.mask_id           = mask_id_;
  result.committed         = true;
  result.quality_requested = true;
  return result;
}

}  // namespace alcedo
