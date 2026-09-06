//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_pending_input.hpp"

namespace alcedo {
namespace {

[[nodiscard]] auto RejectAdmit(std::string error) -> EditorPendingInputAdmitResult {
  EditorPendingInputAdmitResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] auto AcceptAdmit(std::uint64_t sequence_id) -> EditorPendingInputAdmitResult {
  EditorPendingInputAdmitResult result;
  result.accepted    = true;
  result.sequence_id = sequence_id;
  return result;
}

[[nodiscard]] auto SameSessionImage(const EditorSessionIdentity& left,
                                    const EditorSessionIdentity& right) -> bool {
  return left.element_id == right.element_id && left.image_id == right.image_id;
}

/// Node / instance / owner identity only. Field keys are not part of sequence
/// targeting; parameter values are never compared.
[[nodiscard]] auto SameWriteTargetIdentity(const EditorParameterTarget& left,
                                           const EditorParameterTarget& right) -> bool {
  return left.owner_kind == right.owner_kind && left.node_id == right.node_id &&
         left.adjustment_instance_id == right.adjustment_instance_id && left.mask_id == right.mask_id;
}

[[nodiscard]] auto SequenceTargetFrom(const EditorParameterTarget& target)
    -> EditorParameterTarget {
  EditorParameterTarget captured = target;
  captured.field_key.clear();
  return captured;
}

[[nodiscard]] auto HasCompleteWriteIdentity(const EditorParameterTarget& target) -> bool {
  return target.owner_kind != EditorParameterOwnerKind::Unspecified;
}

}  // namespace

auto EditorPendingInputQueue::AdmitFieldChange(EditorSessionIdentity          identity,
                                               const EditorAdjustmentPatch&   patch)
    -> EditorPendingInputAdmitResult {
  std::scoped_lock lock(mutex_);
  return AdmitFieldChangeLocked(identity, patch);
}

auto EditorPendingInputQueue::AdmitBoundary(EditorSessionIdentity          identity,
                                            EditorPendingInputBoundaryKind kind)
    -> EditorPendingInputAdmitResult {
  std::scoped_lock lock(mutex_);
  return AdmitBoundaryLocked(identity, kind);
}

auto EditorPendingInputQueue::Peek() const -> EditorPendingInputView {
  std::scoped_lock lock(mutex_);
  return PeekLocked();
}

auto EditorPendingInputQueue::empty() const -> bool {
  std::scoped_lock lock(mutex_);
  return sealed_.empty() && !open_.has_value();
}

auto EditorPendingInputQueue::AdmitFieldChangeLocked(EditorSessionIdentity        identity,
                                                     const EditorAdjustmentPatch& patch)
    -> EditorPendingInputAdmitResult {
  if (patch.field_key.empty()) {
    return RejectAdmit("Adjustment input requires a field key");
  }
  if (!patch.target.field_key.empty() && patch.target.field_key != patch.field_key) {
    return RejectAdmit("Editor parameter target field_key must match the patch field_key");
  }
  if (identity.element_id == 0 || identity.image_id == 0) {
    return RejectAdmit("Adjustment input requires session image identity");
  }

  if (open_.has_value()) {
    if (!SameSessionImage(open_->identity, identity)) {
      return RejectAdmit("Queued adjustment input belongs to a different image");
    }
    if (HasCompleteWriteIdentity(open_->captured_target) &&
        HasCompleteWriteIdentity(patch.target) &&
        !SameWriteTargetIdentity(open_->captured_target, patch.target)) {
      return RejectAdmit(
          "Queued adjustment input cannot retarget a different node in the same sequence");
    }
  } else {
    StartSequenceLocked(identity, patch.target);
  }

  if (!HasCompleteWriteIdentity(open_->captured_target) &&
      HasCompleteWriteIdentity(patch.target)) {
    open_->captured_target = SequenceTargetFrom(patch.target);
  }

  EditorPendingFieldChange change;
  change.identity    = identity;
  change.target      = patch.target;
  change.target.field_key = patch.field_key;
  if (HasCompleteWriteIdentity(open_->captured_target)) {
    change.target.owner_kind             = open_->captured_target.owner_kind;
    change.target.node_id                = open_->captured_target.node_id;
    change.target.adjustment_instance_id = open_->captured_target.adjustment_instance_id;
    change.target.mask_id                = open_->captured_target.mask_id;
  }
  change.params_json = patch.params_json;
  change.enabled     = patch.enabled;

  const auto existing = open_field_index_.find(patch.field_key);
  if (existing != open_field_index_.end()) {
    open_->fields[existing->second] = std::move(change);
  } else {
    open_field_index_.emplace(patch.field_key, open_->fields.size());
    open_->fields.push_back(std::move(change));
  }

  const auto sequence_id = open_->sequence_id;
  if (patch.settled) {
    SealOpenLocked(EditorPendingInputBoundaryKind::Release);
  }
  return AcceptAdmit(sequence_id);
}

auto EditorPendingInputQueue::AdmitBoundaryLocked(EditorSessionIdentity          identity,
                                                  EditorPendingInputBoundaryKind kind)
    -> EditorPendingInputAdmitResult {
  if (kind == EditorPendingInputBoundaryKind::None) {
    return RejectAdmit("Pending input boundary requires Release, Cancel, or NodeSwitch");
  }
  if (identity.element_id == 0 || identity.image_id == 0) {
    return RejectAdmit("Adjustment input requires session image identity");
  }
  if (open_.has_value() && !SameSessionImage(open_->identity, identity)) {
    return RejectAdmit("Queued adjustment input belongs to a different image");
  }
  if (!open_.has_value()) {
    StartSequenceLocked(identity, EditorParameterTarget{});
  }
  const auto sequence_id = open_->sequence_id;
  if (kind == EditorPendingInputBoundaryKind::Cancel) {
    open_->fields.clear();
    open_field_index_.clear();
  }
  SealOpenLocked(kind);
  return AcceptAdmit(sequence_id);
}

auto EditorPendingInputQueue::StartSequenceLocked(EditorSessionIdentity        identity,
                                                  const EditorParameterTarget& target)
    -> EditorPendingSequence& {
  EditorPendingSequence sequence;
  sequence.sequence_id     = next_sequence_id_++;
  sequence.identity        = identity;
  sequence.captured_target = SequenceTargetFrom(target);
  open_                    = std::move(sequence);
  open_field_index_.clear();
  return *open_;
}

void EditorPendingInputQueue::SealOpenLocked(EditorPendingInputBoundaryKind kind) {
  if (!open_.has_value()) {
    return;
  }
  open_->seal = kind;
  sealed_.push_back(std::move(*open_));
  open_.reset();
  open_field_index_.clear();
}

auto EditorPendingInputQueue::PeekLocked() const -> EditorPendingInputView {
  EditorPendingInputView view;
  view.sequences = sealed_;
  if (open_.has_value()) {
    view.sequences.push_back(*open_);
  }
  return view;
}

}  // namespace alcedo
