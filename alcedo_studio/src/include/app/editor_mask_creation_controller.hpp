//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

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
  Inactive  = 0,
  Selected  = 1,
  Creating  = 2,
  Editing   = 3,
  Painting  = 4,
  Settling  = 5,
  Failed    = 6,
};

/**
 * @brief One pointer sample already mapped into Mask ReferenceSpace.
 *
 * Mapping is the UI/item boundary (@c MaskEditGeometry). This service does not
 * reapply crop, zoom, or DPR. Pointer samples have no extra half-pixel offset.
 * @p inside_photograph is the press gate; captured drags may set it false.
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
  bool          accepted              = false;
  bool          interactive_preview   = false;
  bool          committed             = false;
  bool          quality_requested     = false;
  std::string   error;
  MaskId        mask_id;
};

/**
 * @brief Queued Mask-creation operation for the session owner thread.
 *
 * GUI mapping produces these; the serial consumer applies them under the live
 * pipeline lock. Latest Append samples with the same pointer identity coalesce.
 */
enum class EditorMaskCreationCommandKind : std::uint8_t {
  BeginCreation = 0,
  SelectMask,
  BeginInput,
  BeginMove,
  Append,
  Finish,
  Cancel,
  CancelMode,
};

struct EditorMaskCreationCommand {
  EditorMaskCreationCommandKind kind        = EditorMaskCreationCommandKind::BeginCreation;
  MaskSourceKind                source_kind = MaskSourceKind::Radial;
  NodeId                        node_id;
  MaskId                        mask_id;
  MaskCreationSample            sample{};
  MaskPointerIdentity           identity{};
  AnalyticMaskHandle            handle = AnalyticMaskHandle::None;
};

/**
 * @brief Application owner of Radial/Linear creation and existing-mask movement.
 *
 * Owns mode, active handle, and captured identities. Does not own the live
 * @ref PipelineDocument, Grade selection, or Mix raster. Provisional source
 * fields are written through the Grade owner before release so Interactive Mix
 * can update; one NM4 AddMask or ReplaceMaskSource commit is published on
 * settle. Escape restores the live Grade and publishes no commit.
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
   * @brief Arm Radial or Linear creation on @p grade_id without document mutation.
   *
   * Brush is rejected until accumulating-stroke UI exists. An open operation must
   * be finished or cancelled first.
   */
  auto BeginCreation(MaskSourceKind kind, const NodeId& grade_id,
                     EditorSessionIdentity session) -> EditorMaskCreationResult;

  /**
   * @brief Select an existing Radial or Linear Mask. Load-only; no Mix or commit.
   */
  auto SelectMask(const NodeId& grade_id, const MaskId& mask_id,
                  EditorSessionIdentity session) -> EditorMaskCreationResult;

  /**
   * @brief Start a creation drag at @p sample.
   *
   * Press outside the photograph is rejected. Degenerate zero-area input stays
   * transient until a later valid sample.
   */
  auto BeginMaskInput(MaskCreationSample sample, MaskPointerIdentity identity)
      -> EditorMaskCreationResult;

  /**
   * @brief Start an existing-mask handle drag. Shape fields stay fixed for center/origin.
   */
  auto BeginMaskMove(AnalyticMaskHandle handle, MaskCreationSample sample,
                     MaskPointerIdentity identity) -> EditorMaskCreationResult;

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
  auto FinishMaskInput() -> EditorMaskCreationResult;

  /**
   * @brief Restore the live Grade to the captured before-state. Zero commits.
   */
  auto CancelMaskInput() -> EditorMaskCreationResult;

  /**
   * @brief Leave creation mode. Seals a valid open operation once, then clears mode.
   */
  auto FinishCreationMode() -> EditorMaskCreationResult;

  /**
   * @brief Cancel any open operation and leave creation mode. Zero new commits.
   */
  auto CancelCreationMode() -> EditorMaskCreationResult;

  [[nodiscard]] auto state() const -> EditorMaskCreationState { return state_; }
  [[nodiscard]] auto source_kind() const -> MaskSourceKind { return kind_; }
  [[nodiscard]] auto selected_mask_id() const -> const MaskId& { return mask_id_; }
  [[nodiscard]] auto active_handle() const -> AnalyticMaskHandle { return handle_; }
  [[nodiscard]] auto node_id() const -> const NodeId& { return node_id_; }
  /**
   * @brief True while an initial Radial/Linear drawing sequence is open.
   *
   * Existing-mask edits are false. Armed creation without a press is false.
   */
  [[nodiscard]] auto HasOpenOperation() const -> bool { return open_; }
  [[nodiscard]] auto OverlayIsCreating() const -> bool;
  /**
   * @brief Live or draft source for overlay layout. Empty when Inactive/hidden.
   */
  [[nodiscard]] auto CurrentSource() const -> std::optional<MaskSource>;

 private:
  auto Reject(std::string error) const -> EditorMaskCreationResult;
  auto Ok() const -> EditorMaskCreationResult;
  [[nodiscard]] auto Grade() -> ColorGradeNodeModel*;
  [[nodiscard]] auto Grade() const -> const ColorGradeNodeModel*;
  [[nodiscard]] auto IdentityMatches(const MaskPointerIdentity& identity) const -> bool;
  [[nodiscard]] auto AllocateMaskId() const -> MaskId;
  [[nodiscard]] auto MakeCreationMask(const MaskSource& source) const -> MaskModel;
  [[nodiscard]] auto LiveSourceJson() const -> nlohmann::json;
  [[nodiscard]] auto SourcesEqual(const nlohmann::json& a, const nlohmann::json& b) const -> bool;
  auto ApplyLiveSource(const MaskSource& source) -> bool;
  auto InsertProvisional(const MaskSource& source) -> bool;
  void RestoreLive();
  void ClearOpenOperation();
  void ResetMode();
  auto PublishAddMask() -> EditorMaskCreationResult;
  auto PublishReplaceSource() -> EditorMaskCreationResult;
  auto PublishSettledBatch(const PipelineEditBatch& batch, std::string* error) -> bool;
  void RequestInteractive(EditorMaskCreationResult& result);
  auto UpdateCreation(const MaskCreationSample& sample) -> EditorMaskCreationResult;
  auto UpdateExisting(const MaskCreationSample& sample) -> EditorMaskCreationResult;

  PipelineDocument*        document_ = nullptr;
  MiniGitWorkingHistory*   history_  = nullptr;
  std::function<void()>    interactive_preview_;
  std::function<bool(const PipelineEditBatch&, std::string*)> settle_publisher_;
  EditorMaskCreationState  state_    = EditorMaskCreationState::Inactive;
  MaskSourceKind           kind_     = MaskSourceKind::Radial;
  EditorSessionIdentity    session_{};
  NodeId                   node_id_;
  MaskId                   mask_id_;
  AnalyticMaskHandle       handle_ = AnalyticMaskHandle::None;
  MaskPointerIdentity      pointer_{};
  Vector2                  press_normalized_{};
  float                    unwrapped_rotation_ = 0.0f;
  nlohmann::json           before_source_;
  std::uint32_t            display_index_ = 0;
  bool                     open_          = false;
  bool                     creating_      = false;
  bool                     inserted_      = false;
  bool                     terminated_    = false;
  std::optional<MaskSource> draft_source_;
};

}  // namespace alcedo
