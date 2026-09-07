//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "app/editor_adjustment_types.hpp"
#include "app/editor_session_types.hpp"

namespace alcedo {

/**
 * @brief Ordered boundary that seals one pending input sequence.
 *
 * These are not copies of live parameter state. They mark when a sequence
 * stops accepting further field replacements.
 */
enum class EditorPendingInputBoundaryKind : std::uint8_t {
  None = 0,
  Release,
  Cancel,
  NodeSwitch,
};

/**
 * @brief One field write waiting for the session owner to apply.
 *
 * @p write is the caller's operation for this field only. It is not a copy of
 * the live operator, node, or document parameter collection.
 */
struct EditorPendingFieldChange {
  EditorSessionIdentity identity{};
  EditorParameterTarget target{};
  EditorParameterWrite  write{};
  bool                  enabled = true;
};

/**
 * @brief One input sequence: coalesced field writes plus an optional seal.
 *
 * @p captured_target stores NodeId / AdjustmentInstanceId / owner identity
 * taken at sequence start. Its @c field_key is empty. It does not store
 * parameter values.
 */
struct EditorPendingSequence {
  std::uint64_t                  sequence_id = 0;
  EditorSessionIdentity          identity{};
  EditorParameterTarget          captured_target{};
  std::vector<EditorPendingFieldChange> fields;
  EditorPendingInputBoundaryKind seal = EditorPendingInputBoundaryKind::None;
};

/**
 * @brief Inspectable copy of queued change descriptions.
 *
 * This is the pending-input queue's own contents, not a mirror of
 * PipelineDocument or an EditorRenderAdjustmentSnapshot.
 */
struct EditorPendingInputView {
  std::vector<EditorPendingSequence> sequences;
};

/**
 * @brief Result of admitting one field write or boundary.
 *
 * @p accepted means queued for later owner processing, not history-committed
 * and not applied to the live document.
 */
struct EditorPendingInputAdmitResult {
  bool          accepted    = false;
  std::string   error;
  std::uint64_t sequence_id = 0;
};

/**
 * @brief Bounded pending-input queue for typed adjustment writes.
 *
 * Absolute assignments to the same sequence, target identity, and field keep
 * only the newest write payload. Distinct fields merge. Release, cancel, and
 * node-switch seals are retained in order and start a new sequence.
 *
 * The GUI may admit work without reading live parameters or taking the render
 * lock. Application, history before-values, and rendering are owner consume
 * operations and do not run here.
 *
 * @thread_safety All public methods are serialized by an internal mutex.
 */
class EditorPendingInputQueue {
 public:
  EditorPendingInputQueue() = default;

  /**
   * @brief Queue one absolute field write.
   *
   * @pre @p identity identifies the session/image that produced the write.
   * @pre @p patch.field_key is non-empty. @p patch.write is the field operation,
   *      not a live parameter-body copy.
   * @param identity Session/image ids captured at enqueue. Not a document copy.
   * @param patch Field key, typed write, optional target ids, and whether
   *        this write also releases the sequence.
   * @return Accepted with the sequence id, or rejected with @p error. Never
   *         mutates live document or history.
   */
  auto AdmitFieldChange(EditorSessionIdentity identity, EditorAdjustmentPatch patch)
      -> EditorPendingInputAdmitResult;

  /**
   * @brief Queue a sequence seal without a new field write.
   *
   * @param identity Session/image ids for the boundary.
   * @param kind Release, Cancel, or NodeSwitch. None is rejected.
   * @return Accepted with the sealed sequence id, or rejected.
   *
   * Cancel discards unapplied field writes of the open sequence and still
   * records the Cancel seal so the boundary is not dropped.
   */
  auto AdmitBoundary(EditorSessionIdentity identity, EditorPendingInputBoundaryKind kind)
      -> EditorPendingInputAdmitResult;

  /**
   * @brief Copy queued sequences for tests and owner inspection.
   *
   * @return Sealed sequences in admit order, then the open sequence if any.
   */
  [[nodiscard]] auto Peek() const -> EditorPendingInputView;

  /**
   * @brief Take the next sequence the owner may apply.
   *
   * Sealed sequences are taken in admit order. When none are sealed, the open
   * sequence's current field writes are taken and the sequence stays open so
   * later pointer samples can merge. Empty open sequences are not taken.
   * Cancel seals are taken even when fields were discarded at admit time.
   *
   * @return The batch, or nullopt when nothing is consumable.
   */
  auto TakeReadyBatch() -> std::optional<EditorPendingSequence>;

  /// True when TakeReadyBatch would return a sequence.
  [[nodiscard]] auto HasConsumableWork() const -> bool;

  /// True when no sealed or open sequences remain.
  [[nodiscard]] auto empty() const -> bool;

 private:
  auto AdmitFieldChangeLocked(EditorSessionIdentity identity, EditorAdjustmentPatch patch)
      -> EditorPendingInputAdmitResult;
  auto AdmitBoundaryLocked(EditorSessionIdentity identity, EditorPendingInputBoundaryKind kind)
      -> EditorPendingInputAdmitResult;
  auto StartSequenceLocked(EditorSessionIdentity identity, const EditorParameterTarget& target)
      -> EditorPendingSequence&;
  void SealOpenLocked(EditorPendingInputBoundaryKind kind);
  [[nodiscard]] auto PeekLocked() const -> EditorPendingInputView;
  [[nodiscard]] auto HasConsumableWorkLocked() const -> bool;

  mutable std::mutex                                 mutex_;
  std::uint64_t                                      next_sequence_id_ = 1;
  std::optional<EditorPendingSequence>               open_;
  std::vector<EditorPendingSequence>                 sealed_;
  std::unordered_map<std::string, std::size_t>       open_field_index_;
};

/**
 * @brief Find the newest queued write for @p field_key.
 *
 * Searches from the last sequence so an open sequence wins over an earlier
 * sealed one.
 */
[[nodiscard]] inline auto FindPendingField(const EditorPendingInputView& view,
                                           std::string_view              field_key)
    -> const EditorPendingFieldChange* {
  for (auto sequence = view.sequences.rbegin(); sequence != view.sequences.rend(); ++sequence) {
    for (const auto& field : sequence->fields) {
      if (field.target.field_key == field_key) {
        return &field;
      }
    }
  }
  return nullptr;
}

/**
 * @brief Read a queued scalar assignment when @p field holds that operation.
 */
[[nodiscard]] inline auto PendingScalarValue(const EditorPendingFieldChange& field)
    -> std::optional<float> {
  const auto* scalar = std::get_if<EditorScalarWrite>(&field.write);
  if (scalar == nullptr) {
    return std::nullopt;
  }
  return scalar->value;
}

}  // namespace alcedo
