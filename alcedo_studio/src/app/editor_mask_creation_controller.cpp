//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mask_creation_controller.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "app/pipeline_document_history.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/i_node_model.hpp"

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
  return kind == MaskSourceKind::Radial ? std::string{"Radial"} : std::string{"Linear Gradient"};
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

}  // namespace

void EditorMaskCreationController::Bind(PipelineDocument& document,
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
  const auto* grade = Grade();
  const char* prefix =
      kind_ == MaskSourceKind::Radial ? "mask.radial." : "mask.linear.";
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
    auto mask         = MakeCreationMask(source);
    display_index_    = static_cast<std::uint32_t>(grade->MaskCount());
    grade->AddMask(std::move(mask), display_index_);
    inserted_         = true;
    draft_source_     = source;
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
    } else if (!creating_ && !mask_id_.Empty() && !before_source_.is_null() &&
               grade->FindMask(mask_id_) != nullptr) {
      grade->ReplaceMaskSource(mask_id_, SourceFromJson(before_source_, mask_id_));
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
  draft_source_.reset();
}

void EditorMaskCreationController::ResetMode() {
  ClearOpenOperation();
  state_         = EditorMaskCreationState::Inactive;
  creating_      = false;
  inserted_      = false;
  mask_id_       = MaskId{};
  node_id_       = NodeId{};
  kind_          = MaskSourceKind::Radial;
  before_source_ = nullptr;
  session_       = {};
  terminated_    = false;
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
    return Reject("analytic creation supports Radial and Linear Gradient");
  }
  if (document_ == nullptr || history_ == nullptr) {
    return Reject("Mask creation controller is not bound to a document");
  }
  node_id_ = grade_id;
  if (Grade() == nullptr) {
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
  terminated_    = false;
  state_         = EditorMaskCreationState::Creating;
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
  node_id_ = grade_id;
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
    return Reject("analytic movement supports Radial and Linear Gradient");
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

auto EditorMaskCreationController::BeginMaskInput(MaskCreationSample sample,
                                                  MaskPointerIdentity identity)
    -> EditorMaskCreationResult {
  if (state_ == EditorMaskCreationState::Settling) {
    return Reject("Mask tool is unavailable until settle is acknowledged");
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

auto EditorMaskCreationController::BeginMaskMove(AnalyticMaskHandle handle,
                                                 MaskCreationSample sample,
                                                 MaskPointerIdentity identity)
    -> EditorMaskCreationResult {
  if (state_ == EditorMaskCreationState::Settling) {
    return Reject("Mask tool is unavailable until settle is acknowledged");
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
  open_        = true;
  terminated_  = false;
  state_       = EditorMaskCreationState::Editing;
  auto result  = Ok();
  result.mask_id = mask_id_;
  return result;
}

auto EditorMaskCreationController::UpdateCreation(const MaskCreationSample& sample)
    -> EditorMaskCreationResult {
  MaskSource next;
  if (kind_ == MaskSourceKind::Radial) {
    next = RadialFromCenterOut(press_normalized_, sample.normalized);
  } else {
    next = LinearFromEndpoints(press_normalized_, sample.normalized);
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
  auto next = ApplyAnalyticMaskHandle(handle_, mask->source, sample.normalized, unwrapped_rotation_);
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

auto EditorMaskCreationController::AppendMaskInput(MaskCreationSample sample,
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
                                                       std::string* error) -> bool {
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
  const auto batch =
      MakeAddMaskBatch(node_id_, mask_id_, MaskModelToJson(*mask), display_index_);
  std::string settle_error;
  if (!PublishSettledBatch(batch, &settle_error)) {
    RestoreLive();
    inserted_ = false;
    state_    = EditorMaskCreationState::Failed;
    return Reject(settle_error.empty() ? std::string{"AddMask history publish failed"}
                                       : settle_error);
  }
  ClearOpenOperation();
  creating_      = false;
  inserted_      = false;
  before_source_ = MaskModelToJson(*mask).at("source");
  draft_source_  = mask->source;
  state_         = EditorMaskCreationState::Settling;
  auto result    = Ok();
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
  const auto batch = MakeReplaceMaskSourceBatch(node_id_, mask_id_, before_source_, after);
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
  state_      = EditorMaskCreationState::Settling;
  auto result = Ok();
  result.mask_id           = mask_id_;
  result.committed         = true;
  result.quality_requested = true;
  return result;
}

auto EditorMaskCreationController::FinishMaskInput() -> EditorMaskCreationResult {
  if (!open_) {
    auto result = Ok();
    result.mask_id = mask_id_;
    return result;
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
    inserted_      = false;
    mask_id_       = MaskId{};
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
