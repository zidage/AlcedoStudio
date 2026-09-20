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
    case AnalyticMaskHandle::None:
      return false;
  }
  return false;
}

[[nodiscard]] auto CreationDisplayName(MaskSourceKind kind) -> std::string {
  switch (kind) {
    case MaskSourceKind::Radial:
      return "Radial";
    case MaskSourceKind::LinearGradient:
      return "Linear Gradient";
  }
  return "Mask";
}

[[nodiscard]] auto SourceFromJson(const nlohmann::json& source, const MaskId& mask_id)
    -> MaskSource {
  auto mask = MaskModelFromJson({{"id", std::string{mask_id.Value()}},
                                 {"display_name", ""},
                                 {"enabled", true},
                                 {"opacity", 1.0},
                                 {"deletion_protected", false},
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

/// Field-edit keys settle through SetMaskField.
[[nodiscard]] auto MaskFieldEditKeyIsValid(std::string_view field_key, MaskSourceKind /*kind*/)
    -> bool {
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
      if (!before_source_.is_null()) {
        grade->ReplaceMaskSource(mask_id_, SourceFromJson(before_source_, mask_id_));
      }
      if (!field_edit_key_.empty()) {
        (void)ApplyLiveMaskField(field_edit_key_, before_field_value_);
      }
    }
  } catch (const std::exception&) {
  }
}

void EditorMaskCreationController::ClearOpenOperation() {
  open_               = false;
  terminated_         = true;
  handle_             = AnalyticMaskHandle::None;
  pointer_            = {};
  unwrapped_rotation_ = 0.0f;
  field_edit_key_.clear();
  before_field_value_ = nullptr;
  draft_source_.reset();
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
}

void EditorMaskCreationController::RequestInteractive(EditorMaskCreationResult& result) {
  if (interactive_preview_) {
    interactive_preview_();
  }
  result.interactive_preview = true;
}

auto EditorMaskCreationController::OverlayIsCreating() const -> bool {
  return creating_ && open_;
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
                                                 EditorSessionIdentity session)
    -> EditorMaskCreationResult {
  if (open_) {
    return Reject("finish or cancel the open Mask operation before changing tools");
  }
  if (kind != MaskSourceKind::Radial && kind != MaskSourceKind::LinearGradient) {
    return Reject("creation supports Radial and Linear Gradient");
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
  kind_          = kind;
  session_       = session;
  mask_id_       = MaskId{};
  inserted_      = false;
  creating_      = true;
  handle_        = AnalyticMaskHandle::None;
  before_source_ = nullptr;
  draft_source_.reset();
  terminated_ = false;
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
  if (kind != MaskSourceKind::Radial && kind != MaskSourceKind::LinearGradient) {
    return Reject("Mask source kind is not selectable");
  }
  kind_          = kind;
  session_       = session;
  mask_id_       = mask_id;
  creating_      = false;
  inserted_      = false;
  handle_        = AnalyticMaskHandle::None;
  before_source_ = MaskModelToJson(*mask).at("source");
  draft_source_  = mask->source;
  terminated_    = false;
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
  // Cancelling an uncommitted creation is rollback, not a user deletion.
  if (open_ && node_id_ == grade_id && mask_id_ == mask_id && inserted_ && creating_) {
    return CancelMaskInput();
  }
  const auto errors = document_->ValidateUserDeletion(grade_id, mask_id);
  if (!errors.empty()) {
    return Reject(errors.front().message);
  }
  if (open_ && node_id_ == grade_id && mask_id_ == mask_id) {
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
  return ApplyLiveMaskField(Grade(), mask_id_, field_key, value);
}

auto EditorMaskCreationController::ApplyLiveMaskField(ColorGradeNodeModel*  grade,
                                                      const MaskId&         mask_id,
                                                      const std::string&    field_key,
                                                      const nlohmann::json& value) -> bool {
  if (grade == nullptr) {
    return false;
  }
  auto* mask = grade->FindMask(mask_id);
  if (mask == nullptr) {
    return false;
  }
  try {
    if (field_key == kMaskFieldEnabled) {
      grade->SetMaskEnabled(mask_id, value.get<bool>());
    } else if (field_key == kMaskFieldInvert) {
      grade->SetMaskInvert(mask_id, value.get<bool>());
    } else if (field_key == kMaskFieldOpacity) {
      grade->SetMaskOpacity(mask_id, value.get<float>());
    } else if (field_key == kMaskFieldDisplayName) {
      mask->display_name = value.get<std::string>();
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
  return ApplyMaskFieldValue(node_id_, mask_id_, std::move(field_key), std::move(after_value));
}

auto EditorMaskCreationController::ApplyMaskFieldValue(const NodeId&  grade_id,
                                                       const MaskId&  mask_id,
                                                       std::string    field_key,
                                                       nlohmann::json after_value)
    -> EditorMaskCreationResult {
  if (open_) {
    if (field_edit_key_.empty() || field_edit_key_ != field_key || grade_id != node_id_ ||
        mask_id != mask_id_) {
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
  if (grade_id.Empty() || mask_id.Empty()) {
    return Reject("Mask value edits require a Mask identity");
  }
  if (state_ != EditorMaskCreationState::Selected &&
      state_ != EditorMaskCreationState::Inactive) {
    return Reject("Mask value edits require a settled Mask tool state");
  }
  if (!MaskFieldEditKeyIsValid(field_key, kind_)) {
    return Reject("Mask value key is not editable: " + field_key);
  }
  if (!MaskFieldValueIsValid(field_key, after_value)) {
    return Reject("Mask value is invalid for '" + field_key + "'");
  }
  if (document_ == nullptr) {
    return Reject("Mask creation controller is not bound to a document");
  }
  auto* grade = dynamic_cast<ColorGradeNodeModel*>(document_->Graph().FindNode(grade_id));
  if (grade == nullptr) {
    return Reject("Mask value target is not a Color Grade");
  }
  const auto* mask = grade->FindMask(mask_id);
  if (mask == nullptr) {
    return Reject("Mask is missing");
  }
  const auto before = MaskFieldValueJson(*mask, field_key);
  if (before == after_value) {
    auto result    = Ok();
    result.mask_id = mask_id;
    return result;
  }
  if (!ApplyLiveMaskField(grade, mask_id, field_key, after_value)) {
    return Reject("Mask value apply was rejected");
  }
  const auto  batch = MakeSetMaskFieldBatch(grade_id, mask_id, field_key, before, after_value);
  std::string settle_error;
  if (!PublishSettledBatch(batch, &settle_error)) {
    (void)ApplyLiveMaskField(grade, mask_id, field_key, before);
    state_ = EditorMaskCreationState::Failed;
    return Reject(settle_error.empty() ? std::string{"Mask value history publish failed"}
                                       : settle_error);
  }
  if (grade_id == node_id_ && mask_id == mask_id_) {
    if (const auto* updated = grade->FindMask(mask_id_)) {
      before_source_ = MaskModelToJson(*updated).at("source");
      draft_source_  = updated->source;
    }
  }
  auto result              = Ok();
  result.mask_id           = mask_id;
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
  const auto  batch =
      MakeSetMaskFieldBatch(node_id_, mask_id_, field_key, before_field_value_, after);
  std::string settle_error;
  if (!PublishSettledBatch(batch, &settle_error)) {
    RestoreLive();
    state_ = EditorMaskCreationState::Failed;
    return Reject(settle_error.empty() ? std::string{"Mask value history publish failed"}
                                       : settle_error);
  }
  ClearOpenOperation();
  if (const auto* updated = grade->FindMask(mask_id_)) {
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
                                                  MaskSourceKind      expected_source)
    -> EditorMaskCreationResult {
  if (state_ == EditorMaskCreationState::Settling) {
    return Reject("Mask tool is unavailable until settle is acknowledged");
  }
  if (expected_source != kind_) {
    return Reject("stale Mask tool dispatch does not match the armed source kind");
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
    default:
      return Reject("creation source kind is not drawable");
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

}  // namespace alcedo
