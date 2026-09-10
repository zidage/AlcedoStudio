//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "app/brush_mask_input.hpp"
#include "app/editor_session_types.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/analytic_mask_edit.hpp"
#include "edit/mask/brush_raster_encoding.hpp"
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
  Painting = 4,
  Settling = 5,
  Failed   = 6,
};

/**
 * @brief Explicit Brush interaction. Paint and erase append strokes; Move does not.
 *
 * Drawer selection leaves this Idle so a handle drag cannot paint. The header Brush
 * action arms Paint. Erase and Move are set through @ref SetBrushTool.
 */
enum class EditorBrushTool : std::uint8_t {
  Idle  = 0,
  Paint = 1,
  Erase = 2,
  Move  = 3,
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
 * pipeline lock. Latest analytic or Brush-move Append samples with the same
 * pointer identity coalesce. Brush paint/erase Append sets @p ordered_append.
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
  SetBrushTool,
  SetBrushStrokeParameters,
  BeginMaskField,
  SetMaskField,
};

/// Mask value keys used by BeginMaskField / SetMaskField commands. The first
/// four are Mask fields committed through SetMaskField; `brush.feather`
/// rewrites the Brush source feather (reference pixels) and settles through
/// ReplaceMaskSource.
inline constexpr std::string_view kMaskFieldEnabled      = "enabled";
inline constexpr std::string_view kMaskFieldInvert       = "invert";
inline constexpr std::string_view kMaskFieldOpacity      = "opacity";
inline constexpr std::string_view kMaskFieldDisplayName  = "display_name";
inline constexpr std::string_view kMaskFieldBrushFeather = "brush.feather";

struct EditorMaskCreationCommand {
  EditorMaskCreationCommandKind kind        = EditorMaskCreationCommandKind::BeginCreation;
  MaskSourceKind                source_kind = MaskSourceKind::Radial;
  NodeId                        node_id;
  MaskId                        mask_id;
  MaskCreationSample            sample{};
  MaskPointerIdentity           identity{};
  AnalyticMaskHandle            handle         = AnalyticMaskHandle::None;
  EditorBrushTool               brush_tool     = EditorBrushTool::Idle;
  float                         brush_radius   = 0.0f;
  float                         brush_strength = 1.0f;
  float                         brush_hardness = 1.0f;
  bool                          ordered_append = false;
  /**
   * @brief Mask-level value edit target.
   *
   * `enabled`, `invert`, `opacity`, and `display_name` settle as SetMaskField;
   * `brush.feather` rewrites the Brush source and settles as ReplaceMaskSource.
   */
  std::string                   field_key;
  nlohmann::json                field_value;
};

/**
 * @brief Application owner of Mask creation, Brush strokes, and existing-mask movement.
 *
 * Owns mode, active handle, Brush tool settings, and captured identities. Does
 * not own the live @ref PipelineDocument, Grade selection, or Mix raster.
 * Provisional Grade fields are written before release so Interactive Mix can
 * update. First Brush release publishes AddMask; later strokes publish
 * AppendBrushStroke; Brush moves publish SetBrushTranslation. Analytic settle
 * stays AddMask or ReplaceMaskSource. Escape restores the live Grade and
 * publishes no commit. New Brush work never calls MaskStore::Put or
 * ReplaceMaskAsset.
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
   * @brief Arm Radial, Linear, or Brush creation on @p grade_id without a commit.
   *
   * Brush with no existing Mask stays Creating until the first valid stroke.
   * One existing Brush is resumed. Several existing Brushes require @p resume_mask
   * to name one of them; the document is not flattened. An open operation must
   * be finished or cancelled first.
   */
  auto BeginCreation(MaskSourceKind kind, const NodeId& grade_id, EditorSessionIdentity session,
                     MaskId resume_mask = {}) -> EditorMaskCreationResult;

  /**
   * @brief Select an existing Brush, Radial, or Linear Mask. Load-only; no Mix or commit.
   *
   * Brush selection does not arm paint or movement. Analytic kinds load handles
   * for later movement.
   */
  auto SelectMask(const NodeId& grade_id, const MaskId& mask_id, EditorSessionIdentity session)
      -> EditorMaskCreationResult;

  /**
   * @brief Arm Paint, Erase, or Move on the selected or creating Brush.
   *
   * Rejected while a stroke or move is open. Move requires an existing Brush.
   */
  auto SetBrushTool(EditorBrushTool tool) -> EditorMaskCreationResult;

  /**
   * @brief Size/strength/hardness for subsequent dabs, including an open stroke.
   *
   * Mid-stroke changes emit a parameter-boundary sample. Mask opacity is not
   * Brush strength. Radius is in reference pixels; strength and hardness stay
   * in `[0, 1]`.
   */
  auto SetBrushStrokeParameters(float radius, float strength, float hardness)
      -> EditorMaskCreationResult;

  /**
   * @brief Remove @p mask_id from @p grade_id with one typed history operation.
   *
   * Cancels an unfinished operation on that Mask first. Other Masks' open
   * edits are left alone. Failed publish leaves the committed Mask in place.
   */
  auto RemoveMask(const NodeId& grade_id, const MaskId& mask_id) -> EditorMaskCreationResult;

  /**
   * @brief Open a Mask value edit on the selected Mask.
   *
   * Valid keys: `enabled`, `invert`, `opacity`, `display_name`, and
   * `brush.feather` (Brush source feather, reference pixels). The live value is
   * captured for Finish/Cancel. Requires a selected existing Mask with no open
   * operation.
   */
  auto BeginMaskFieldEdit(std::string field_key) -> EditorMaskCreationResult;

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
   * @brief True while an initial Radial/Linear drawing sequence is open.
   *
   * Existing-mask edits are false. Armed creation without a press is false.
   */
  [[nodiscard]] auto HasOpenOperation() const -> bool { return open_; }
  [[nodiscard]] auto OverlayIsCreating() const -> bool;
  [[nodiscard]] auto brush_tool() const -> EditorBrushTool { return brush_tool_; }
  [[nodiscard]] auto brush_radius() const -> float { return brush_input_.radius(); }
  [[nodiscard]] auto brush_strength() const -> float { return brush_input_.strength(); }
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
  auto BeginBrushStroke(const MaskCreationSample& sample) -> EditorMaskCreationResult;
  auto UpdateBrushPaint(const MaskCreationSample& sample) -> EditorMaskCreationResult;
  auto UpdateBrushMove(const MaskCreationSample& sample) -> EditorMaskCreationResult;
  auto FinishBrushStroke() -> EditorMaskCreationResult;
  auto PublishAppendStroke(BrushStroke stroke) -> EditorMaskCreationResult;
  auto PublishSetTranslation() -> EditorMaskCreationResult;
  auto PublishMaskFieldEdit() -> EditorMaskCreationResult;
  auto ApplyLiveMaskField(const std::string& field_key, const nlohmann::json& value) -> bool;
  auto ApplyLiveBrushDraft() -> bool;
  [[nodiscard]] auto     AllocateStrokeId() const -> StrokeId;
  [[nodiscard]] auto     ComposeDraftBrush() const -> BrushMaskSource;
  [[nodiscard]] auto     BrushStrokeModeFromTool() const -> BrushStrokeMode;

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
  EditorBrushTool           brush_tool_ = EditorBrushTool::Idle;
  BrushMaskInput            brush_input_;
  BrushMaskSource           committed_brush_{};
  Vector2                   press_reference_pixels_{};
  Vector2                   before_translation_{};
  std::string               field_edit_key_;
  nlohmann::json            before_field_value_;
};

}  // namespace alcedo
