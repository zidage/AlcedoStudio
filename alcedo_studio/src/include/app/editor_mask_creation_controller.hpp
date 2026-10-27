//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "app/editor_session_types.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/analytic_mask_edit.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief Master Mask-creation states. One controller owns the current state.
 */
enum class EditorMaskCreationState : std::uint8_t {
  Inactive = 0,
  Selected = 1,
  Creating = 2,
  Editing  = 3,
  Settling = 5,
  Failed   = 6,
};

/**
 * @brief One pointer sample already mapped into Mask ReferenceSpace.
 *
 * Mapping is the UI/item boundary (@c MaskEditGeometry). This service does not
 * reapply crop, zoom, or DPR. Pointer samples have no extra half-pixel offset.
 * @p inside_photograph is the creation press precondition; captured drags and presses
 * on existing handles may set it false.
 */
struct MaskCreationSample {
  Vector2 normalized{};
  Vector2 reference_pixels{};
  bool    inside_photograph = false;
};

/**
 * @brief Captured pointer identity for one input sequence.
 *
 * Admission is not a history commit. Samples with a mismatched identity after
 * press are ignored.
 */
struct MaskPointerIdentity {
  std::uint64_t device_id   = 0;
  std::uint32_t point_id    = 0;
  std::uint64_t sequence_id = 0;
};

/**
 * @brief Result of one creation-controller admission.
 *
 * @p accepted means the call was consumed by the state machine. @p interactive_preview
 * is true when live Grade source fields changed and the Interactive callback ran.
 * @p committed / @p quality_requested are set only by a successful settle.
 */
struct EditorMaskCreationResult {
  bool        accepted            = false;
  bool        interactive_preview = false;
  bool        committed           = false;
  bool        quality_requested   = false;
  std::string error;
  MaskId      mask_id;
};

/**
 * @brief Queued Mask-creation operation for the session owner thread.
 *
 * GUI mapping produces these; the serial consumer applies them under the live
 * pipeline lock. Latest Append samples with the same pointer identity
 * coalesce.
 */
enum class EditorMaskCreationCommandKind : std::uint8_t {
  BeginCreation = 0,
  SelectMask,
  BeginInput,
  BeginMove,
  Append,
  Finish,
  Cancel,
  FinishMode,
  CancelMode,
  RemoveMask,
  BeginMaskField,
  SetMaskField,
  ToggleMaskInvert,
};

/// Mask value keys used by BeginMaskField / SetMaskField commands, committed
/// through SetMaskField.
inline constexpr std::string_view kMaskFieldEnabled      = "enabled";
inline constexpr std::string_view kMaskFieldInvert       = "invert";
inline constexpr std::string_view kMaskFieldOpacity      = "opacity";
inline constexpr std::string_view kMaskFieldDisplayName  = "display_name";
/// Hue Range: a @ref ColorRangeToJson value (`null` selects every hue).
inline constexpr std::string_view kMaskFieldColorRange     = "color_range";
/// Luminance Range: a @ref LuminanceRangeToJson value (`null` selects every luminance).
inline constexpr std::string_view kMaskFieldLuminanceRange = "luminance_range";

struct EditorMaskCreationCommand {
  EditorMaskCreationCommandKind kind        = EditorMaskCreationCommandKind::BeginCreation;
  MaskSourceKind                source_kind = MaskSourceKind::Radial;
  NodeId                        node_id;
  MaskId                        mask_id;
  MaskCreationSample            sample{};
  MaskPointerIdentity           identity{};
  AnalyticMaskHandle            handle         = AnalyticMaskHandle::None;
  bool                          ordered_append = false;
  /**
   * @brief Mask-level value edit target.
   *
   * `enabled`, `invert`, `opacity`, `display_name`, `color_range`, and `luminance_range`
   * settle as SetMaskField.
   */
  std::string                   field_key;
  nlohmann::json                field_value;
};

/// True for `luminance_range` and `color_range`.
[[nodiscard]] auto IsMaskRangeFieldKey(std::string_view field_key) -> bool;

/**
 * @brief True for a command of a Mask range edit (Mask ranges MR4): BeginMaskField or
 *        SetMaskField of a range key, and Finish.
 *
 * During the AI Mask edit of a stored Mask the session admits these (and Cancel) for that Mask
 * only; the session owner checks the target.
 */
[[nodiscard]] auto IsMaskRangeEditCommand(const EditorMaskCreationCommand& command) -> bool;

/**
 * @brief Application owner of Mask creation and existing-mask movement.
 *
 * Owns mode, active handle, and captured identities. Does not own the live
 * @ref PipelineDocument, Grade selection, or Mix raster. Provisional Grade
 * fields are written before release so Interactive Mix can update. Analytic
 * settle stays AddMask or ReplaceMaskSource. Escape restores the live Grade
 * and publishes no commit.
 *
 * Thread: document-owning thread. Does not take the pipeline lock, perform
 * cache I/O, or build QSG geometry.
 */
class EditorMaskCreationController {
 public:
  /**
   * @brief Bind the live document and history journal for subsequent calls.
   *
   * @p document and @p history are not owned. Callers keep them alive for every
   * operation. Rebinding while an operation is open is rejected.
   */
  void Bind(PipelineDocument& document, MiniGitWorkingHistory& history);

  /**
   * @brief Called after live Grade source fields become visible, before return.
   *
   * Used to evaluate Interactive Mix. Must not lock the pipeline, throw, or
   * commit history. Empty clears the callback.
   */
  void SetInteractivePreview(std::function<void()> preview);

  /**
   * @brief Optional settle publisher used instead of @c MiniGitWorkingHistory::AppendEdit.
   *
   * Production supplies a locked @c PublishAppliedTypedBatch with the live document
   * already at after-values. Empty keeps the test AppendEdit path.
   */
  void SetSettlePublisher(std::function<bool(const PipelineEditBatch&, std::string*)> publish);

  /**
   * @brief Drop document/history pointers after cancel or image close.
   *
   * Does not restore Grade fields. Callers must Cancel first when an operation
   * is still open and the document is still alive.
   */
  void DetachClosedDocument();

  /**
   * @brief Arm Radial or Linear creation on @p grade_id without a commit.
   *
   * An open operation must be finished or cancelled first.
   */
  auto BeginCreation(MaskSourceKind kind, const NodeId& grade_id, EditorSessionIdentity session)
      -> EditorMaskCreationResult;

  /**
   * @brief Select an existing Radial, Linear, or AI Mask. Load-only; no Mix or commit.
   *
   * Selection loads handles for later movement. An AI Mask has no handles: its selection allows
   * the Mask value and range edits, and selecting it starts no AI Mask edit (#564).
   */
  auto SelectMask(const NodeId& grade_id, const MaskId& mask_id, EditorSessionIdentity session)
      -> EditorMaskCreationResult;

  /**
   * @brief Remove @p mask_id from @p grade_id with one typed history operation.
   *
   * Checks the owning Grade's deletion protection before cancelling an existing edit or
   * changing selection. Cancelling a provisional creation is rollback, not deletion.
   * Other Masks' open edits are left alone. Failed publish leaves the committed Mask in place.
   */
  auto RemoveMask(const NodeId& grade_id, const MaskId& mask_id) -> EditorMaskCreationResult;

  /**
   * @brief Publish the result of an AI Mask edit session as one typed history operation.
   *
   * Without @p mask_id: a new Mask with a new `mask.ai.N` id, the display name "AI Mask N", and
   * @p source, appended to the Grade through AddMask. With @p mask_id: the Mask must be an AI
   * Segmentation Mask of the Grade; its source changes through ReplaceMaskSource, and an equal
   * source publishes nothing. Requires no open operation. Leaves the controller Inactive on every
   * return; a failed publish restores the live Grade.
   * @return accepted and committed when a batch was published; accepted and not committed for an
   *         equal source; not accepted with the error otherwise.
   */
  auto CommitAiMaskSource(const NodeId& grade_id, const std::optional<MaskId>& mask_id,
                          const AiSegmentationMaskSource& source) -> EditorMaskCreationResult;

  /**
   * @brief Open a Mask value edit on the selected Mask.
   *
   * Valid keys: `enabled`, `invert`, `opacity`, and `display_name`.
   * The live value is captured for Finish/Cancel. Requires a selected existing Mask with
   * no open operation.
   */
  auto BeginMaskFieldEdit(std::string field_key) -> EditorMaskCreationResult;

  /**
   * @brief Open a range edit (`luminance_range` or `color_range`) of the Mask (@p grade_id,
   *        @p mask_id), of any source kind (Mask ranges MR4).
   *
   * The edit does not change the selection or the mode: the controller can be Inactive (an AI
   * Mask has no selection) or have another Mask selected. While it is open, the matching explicit
   * @ref ApplyMaskFieldValue writes live values with Interactive, @ref FinishMaskInput publishes
   * one SetMaskField commit, @ref CancelMaskInput restores the before value, and every other
   * operation is refused. Requires no open operation.
   */
  auto BeginMaskRangeEdit(const NodeId& grade_id, const MaskId& mask_id, std::string field_key)
      -> EditorMaskCreationResult;

  /**
   * @brief Apply @p after_value for @p field_key.
   *
   * While a matching field edit is open this updates the live Grade and runs
   * Interactive without committing. With no open field edit it performs one
   * atomic live apply plus settle commit; equal values commit nothing.
   */
  auto ApplyMaskFieldValue(std::string field_key, nlohmann::json after_value)
      -> EditorMaskCreationResult;

  /**
   * @brief Apply @p after_value for @p field_key on an explicit (NodeId, MaskId).
   *
   * Same commit semantics as the selected-Mask overload: a matching open field
   * edit updates live, otherwise one atomic live apply plus settle commit runs.
   * The explicit overload targets any stored Mask — the Mask Groups lock button
   * uses it so a row lock edit cannot land on a different selected Mask.
   * Detached writes require the Mask tool to be idle or merely Selected; an
   * open operation on another Mask is rejected.
   */
  auto ApplyMaskFieldValue(const NodeId& grade_id, const MaskId& mask_id,
                           std::string field_key, nlohmann::json after_value)
      -> EditorMaskCreationResult;

  /**
   * @brief Set `invert` of (@p grade_id, @p mask_id) to the opposite of its stored value.
   *
   * Reads the value when the command runs, so each queued toggle changes the Mask once.
   * Commits one SetMaskField through the explicit ApplyMaskFieldValue overload.
   */
  auto ToggleMaskInvert(const NodeId& grade_id, const MaskId& mask_id) -> EditorMaskCreationResult;

  /**
   * @brief Start a creation drag at @p sample.
   *
   * Press outside the photograph is rejected. Degenerate zero-area input stays
   * transient until a later valid sample. @p expected_source must equal the
   * armed kind so a stale queued dispatch cannot drive the wrong Mask type.
   */
  auto BeginMaskInput(MaskCreationSample sample, MaskPointerIdentity identity,
                      MaskSourceKind expected_source) -> EditorMaskCreationResult;

  /**
   * @brief Start an existing-mask handle drag. Shape fields stay fixed for center/origin.
   *
   * @p expected_source must equal the selected Mask kind; a mismatch rejects
   * the stale dispatch without opening an operation.
   */
  auto BeginMaskMove(AnalyticMaskHandle handle, MaskCreationSample sample,
                     MaskPointerIdentity identity, MaskSourceKind expected_source)
      -> EditorMaskCreationResult;

  /**
   * @brief Continue the captured sequence. Applies provisional fields and Interactive.
   */
  auto AppendMaskInput(MaskCreationSample sample, MaskPointerIdentity identity)
      -> EditorMaskCreationResult;

  /**
   * @brief Seal the open operation once. Valid work publishes one commit and Quality.
   *
   * Degenerate creation and unchanged placement publish neither a commit nor Quality.
   */
  auto               FinishMaskInput() -> EditorMaskCreationResult;

  /**
   * @brief Restore the live Grade to the captured before-state. Zero commits.
   */
  auto               CancelMaskInput() -> EditorMaskCreationResult;

  /**
   * @brief Leave creation mode. Seals a valid open operation once, then clears mode.
   */
  auto               FinishCreationMode() -> EditorMaskCreationResult;

  /**
   * @brief Cancel any open operation and leave creation mode. Zero new commits.
   */
  auto               CancelCreationMode() -> EditorMaskCreationResult;

  [[nodiscard]] auto state() const -> EditorMaskCreationState { return state_; }
  [[nodiscard]] auto source_kind() const -> MaskSourceKind { return kind_; }
  [[nodiscard]] auto selected_mask_id() const -> const MaskId& { return mask_id_; }
  [[nodiscard]] auto active_handle() const -> AnalyticMaskHandle { return handle_; }
  [[nodiscard]] auto node_id() const -> const NodeId& { return node_id_; }
  /**
   * @brief True while an input sequence is open: a creation drag, an existing-mask handle move,
   *        or a Mask value edit. The live document may then hold values that are not committed.
   *
   * Armed creation without a press is false.
   */
  [[nodiscard]] auto HasOpenOperation() const -> bool { return open_ || range_edit_.has_value(); }
  [[nodiscard]] auto OverlayIsCreating() const -> bool;
  /**
   * @brief Live or draft source for overlay layout. Empty when Inactive/hidden.
   */
  [[nodiscard]] auto CurrentSource() const -> std::optional<MaskSource>;
  [[nodiscard]] auto last_removed_mask_id() const -> const MaskId& { return last_removed_mask_id_; }

 private:
  auto               Reject(std::string error) const -> EditorMaskCreationResult;
  auto               Ok() const -> EditorMaskCreationResult;
  [[nodiscard]] auto Grade() -> ColorGradeNodeModel*;
  [[nodiscard]] auto Grade() const -> const ColorGradeNodeModel*;
  [[nodiscard]] auto IdentityMatches(const MaskPointerIdentity& identity) const -> bool;
  [[nodiscard]] auto AllocateMaskId() const -> MaskId;
  [[nodiscard]] auto MakeCreationMask(const MaskSource& source) const -> MaskModel;
  [[nodiscard]] auto LiveSourceJson() const -> nlohmann::json;
  [[nodiscard]] auto SourcesEqual(const nlohmann::json& a, const nlohmann::json& b) const -> bool;
  auto               ApplyLiveSource(const MaskSource& source) -> bool;
  auto               InsertProvisional(const MaskSource& source) -> bool;
  void               RestoreLive();
  void               ClearOpenOperation();
  void               ResetMode();
  auto               PublishAddMask() -> EditorMaskCreationResult;
  auto               PublishReplaceSource() -> EditorMaskCreationResult;
  auto PublishSettledBatch(const PipelineEditBatch& batch, std::string* error) -> bool;
  void RequestInteractive(EditorMaskCreationResult& result);
  auto UpdateCreation(const MaskCreationSample& sample) -> EditorMaskCreationResult;
  auto UpdateExisting(const MaskCreationSample& sample) -> EditorMaskCreationResult;
  auto PublishMaskFieldEdit() -> EditorMaskCreationResult;
  auto               PublishMaskRangeEdit() -> EditorMaskCreationResult;
  auto               CancelMaskRangeEdit() -> EditorMaskCreationResult;
  [[nodiscard]] auto RangeEditGrade() -> ColorGradeNodeModel*;
  auto ApplyLiveMaskField(const std::string& field_key, const nlohmann::json& value) -> bool;
  auto ApplyLiveMaskField(ColorGradeNodeModel* grade, const MaskId& mask_id,
                          const std::string& field_key, const nlohmann::json& value) -> bool;

  PipelineDocument*      document_ = nullptr;
  MiniGitWorkingHistory* history_  = nullptr;
  std::function<void()>  interactive_preview_;
  std::function<bool(const PipelineEditBatch&, std::string*)> settle_publisher_;
  EditorMaskCreationState   state_ = EditorMaskCreationState::Inactive;
  MaskSourceKind            kind_  = MaskSourceKind::Radial;
  EditorSessionIdentity     session_{};
  NodeId                    node_id_;
  MaskId                    mask_id_;
  AnalyticMaskHandle        handle_ = AnalyticMaskHandle::None;
  MaskPointerIdentity       pointer_{};
  Vector2                   press_normalized_{};
  float                     unwrapped_rotation_ = 0.0f;
  nlohmann::json            before_source_;
  std::uint32_t             display_index_ = 0;
  bool                      open_          = false;
  bool                      creating_      = false;
  bool                      inserted_      = false;
  bool                      terminated_    = false;
  std::optional<MaskSource> draft_source_;
  MaskId                    last_removed_mask_id_;
  std::string               field_edit_key_;
  nlohmann::json            before_field_value_;

  /// The open range edit of @ref BeginMaskRangeEdit. It is separate from the selection and the
  /// mode, so the edit can target any Mask kind (an AI Mask too) and leaves both unchanged.
  struct OpenRangeEdit {
    NodeId         grade_id;
    MaskId         mask_id;
    std::string    field_key;
    nlohmann::json before_value;
  };
  std::optional<OpenRangeEdit> range_edit_;
};

}  // namespace alcedo
