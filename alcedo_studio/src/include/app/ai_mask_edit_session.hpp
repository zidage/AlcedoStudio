//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "app/ai_mask_coverage_composition.hpp"
#include "app/ai_mask_model_output_store.hpp"
#include "app/ai_mask_segmentation_service.hpp"
#include "edit/geometry/types.hpp"
#include "edit/mask/ai_mask_recipe.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_model.hpp"
#include "sidecar_client/dto/segmentation.hpp"

namespace alcedo {

/// The input of the AI Mask tool: prompts (strokes, clicks, boxes) or task masks.
enum class AiMaskInputMode : std::uint8_t { Prompt, Task };

/**
 * @brief Where a prompt step goes (SM9, issue #519).
 *
 * JoinOpenGroup adds the step to the open prompt group, or starts a group when none is open (M3).
 * NewGroup always starts a group: the AI Mask tool selects a new object with a Box or a Path.
 */
enum class AiMaskPromptGroupStart : std::uint8_t { JoinOpenGroup, NewGroup };

/**
 * @brief The image of one AI Mask edit session.
 *
 * @p extent is the size of the segmentation session image (SM5 decision M14, from the start
 * response). @p model_set_id and @p geometry_fingerprint identify the models and the Lens and RAW
 * decode settings of the analysis image; a commit stores them in the source. @p guide is the
 * analysis image that guides the upscale of the alpha grids (master plan D23); null upscales
 * them without it.
 */
struct AiMaskSessionImage {
  Extent2D                                   extent;
  std::string                                model_set_id;
  std::string                                geometry_fingerprint;
  std::shared_ptr<const AiMaskCoverageGuide> guide;
};

/**
 * @brief One AI Mask edit session: its own step history, the model outputs of its steps, the
 * composed Candidate Mask coverage, and the commit payload (SM5.2, decisions D16, D21, M3 to M9,
 * M15).
 *
 * A session state is the recipe, the edge adjustment, the input mode, and whether the last
 * component is an open prompt group (M3). The states form a stack with a current index; a new
 * state removes the states after the current index. A state enters the stack only after all its
 * model outputs exist and its coverage is composed, so Undo and Redo need no model call. A step
 * that fails, finds nothing, or is not valid keeps the current state and coverage.
 *
 * The session does not change the document. A commit takes PrepareCommit() and publishes one main
 * history operation through the editor (SM5.3).
 *
 * A session opened from a stored AI Mask shows the stored asset as the coverage of every state
 * whose recipe equals the stored recipe (M15). Such a state also commits the stored model set and
 * geometry fingerprint, because its coverage comes from them.
 *
 * Threading: not thread-safe; one thread (the AI Mask tool worker) calls every method. Runner
 * calls block.
 */
class AiMaskEditSession {
 public:
  /**
   * @brief Start a session for a new AI Mask: no component and an empty coverage.
   * @p runner must outlive the session.
   * @throws std::invalid_argument when the image size is empty or an axis exceeds 4096.
   */
  AiMaskEditSession(IAiMaskSegmentationRunner& runner, AiMaskSessionImage image);

  /**
   * @brief Start a session for a stored AI Mask (M15). No model call runs.
   *
   * The first state has the stored recipe and edge and shows @p stored_asset at its bounds. The
   * input mode is Prompt with an open group when the last component is a prompt group, else Task.
   *
   * @throws std::runtime_error when @p stored is not valid, @p stored_asset is not the asset of
   *         @p stored, or the analysis size of @p stored differs from the session image size.
   */
  AiMaskEditSession(IAiMaskSegmentationRunner& runner, AiMaskSessionImage image,
                    const AiSegmentationMaskSource& stored, MaskAsset stored_asset);

  /**
   * @brief Add one prompt step (a stroke, a click, or a box) with tool mode @p mode (M3, M4).
   *
   * In an open prompt group the step joins the group and @p mode is the label of its points. Else
   * the step starts a new group with operation @p mode and Add labels. The labels and the level
   * of @p step are replaced; the level becomes the decoder `selected_index`. With @p start
   * NewGroup the step starts a new group also when a group is open.
   *
   * @return Applied; Failed with the sidecar message; Rejected for an invalid step or a group of
   *         more than 64 points.
   */
  auto AddPromptStep(AiMaskPromptStep step, AiMaskComponentOperation mode,
                     AiMaskPromptGroupStart start = AiMaskPromptGroupStart::JoinOpenGroup)
      -> AiMaskStepResult;

  /// Select @p level of the last step; valid only when the last component is a prompt group that
  /// is one point without a box. Needs no model call for a state with its outputs.
  auto SelectLevel(std::uint32_t level) -> AiMaskStepResult;

  /// Add the task mask @p task with @p operation as a new component. Closes the prompt group.
  auto AddTask(AiMaskTaskKind task, AiMaskComponentOperation operation) -> AiMaskStepResult;

  /**
   * @brief Add or remove person @p person_index in the last component, which must be People
   * (M5): Subtract when the person is included, else Add. Closes the prompt group.
   *
   * @return Rejected when the last component is not People or the index is outside the People
   *         list; NotFound when the person result has no mask.
   */
  auto TogglePerson(std::uint32_t person_index) -> AiMaskStepResult;

  /// Set Feather and Expand (M8). No model call. Rejected for an invalid or unchanged value.
  auto SetEdgeAdjustment(AiMaskEdgeAdjustment edge) -> AiMaskStepResult;

  /// Change the input mode. No model call. Closes the prompt group. Rejected when unchanged.
  auto SetInputMode(AiMaskInputMode mode) -> AiMaskStepResult;

  /// Go to the previous state. Returns false at the first state. No model call.
  auto Undo() -> bool;
  /// Go to the next state. Returns false at the last state. No model call.
  auto Redo() -> bool;

  [[nodiscard]] auto CanUndo() const -> bool { return current_ > 0; }
  [[nodiscard]] auto CanRedo() const -> bool { return current_ + 1 < states_.size(); }
  /// Number of states in the stack, the first state included.
  [[nodiscard]] auto StateCount() const -> std::size_t { return states_.size(); }

  /// The composed coverage of the current state: R8 at the session size, before Feather and
  /// Expand. The view is valid until the next call that changes the state.
  [[nodiscard]] auto Coverage() const -> std::span<const std::uint8_t> { return *coverage_; }
  /**
   * @brief Shared read of the composed coverage of the current state (SM8 section 3).
   *
   * Returns the same allocation that @ref Coverage views; no pixel copy. The session never writes
   * to an allocation after it shares it: a step that changes the recipe composes a new allocation,
   * and a step that changes only the edge adjustment or the input mode keeps the current one. An
   * upload on another executor holds the read until its GPU upload is complete. The allocation is
   * released when the session and every read no longer refer to it.
   *
   * Thread: call on the session thread. The returned pointer may move to and be released on any
   * thread.
   */
  [[nodiscard]] auto AcquireCoveragePixels() const
      -> std::shared_ptr<const std::vector<std::uint8_t>> {
    return coverage_;
  }
  /// The People list of the People task; empty until the session ran it.
  [[nodiscard]] auto People() const -> std::span<const SegmentationPerson> {
    return outputs_.People();
  }
  /**
   * @brief Whether person @p person_index is shown as included (M5): the operation of its last
   * toggle in the last People component, else `in_subject` of the People list (false outside it).
   */
  [[nodiscard]] auto PersonIncluded(std::uint32_t person_index) const -> bool;
  [[nodiscard]] auto Recipe() const -> const AiMaskRecipe& { return states_[current_].recipe; }
  [[nodiscard]] auto Edge() const -> const AiMaskEdgeAdjustment& { return states_[current_].edge; }
  [[nodiscard]] auto InputMode() const -> AiMaskInputMode { return states_[current_].input_mode; }
  /// Whether the last component is a prompt group that the next prompt step joins (M3).
  [[nodiscard]] auto PromptGroupOpen() const -> bool { return states_[current_].open_prompt_group; }
  [[nodiscard]] auto Image() const -> const AiMaskSessionImage& { return image_; }

  /**
   * @brief The commit payload of the current state (M9): the coverage cropped to its values > 0
   * plus a 1-pixel border, and the source that refers to it.
   *
   * @throws std::runtime_error "The AI Mask has no coverage." when no value is > 0, or the
   *         source validation message.
   */
  [[nodiscard]] auto PrepareCommit() const -> std::pair<MaskAsset, AiSegmentationMaskSource>;

 private:
  struct SessionState {
    AiMaskRecipe         recipe;
    AiMaskEdgeAdjustment edge;
    AiMaskInputMode      input_mode        = AiMaskInputMode::Prompt;
    bool                 open_prompt_group = false;
  };

  /// The stored AI Mask of an opened session and its coverage at the session size.
  struct StoredMask {
    AiSegmentationMaskSource                         source;
    std::shared_ptr<const std::vector<std::uint8_t>> coverage;
  };

  /// Computes the outputs of @p state, composes its coverage, and pushes it.
  auto                      Apply(SessionState state) -> AiMaskStepResult;
  /// Computes every missing output of @p recipe; no call for the stored recipe.
  auto                      EnsureOutputs(const AiMaskRecipe& recipe) -> AiMaskStepResult;
  /// Pushes @p state with the outputs that exist; drops the Redo states.
  void                      Push(SessionState state);
  /// Makes the coverage of @p recipe from existing outputs.
  void                      Compose(const AiMaskRecipe& recipe);
  [[nodiscard]] auto        UsesStoredCoverage(const AiMaskRecipe& recipe) const -> bool;
  [[nodiscard]] auto        Current() const -> const SessionState& { return states_[current_]; }

  AiMaskSessionImage        image_;
  AiMaskModelOutputStore    outputs_;
  std::optional<StoredMask> stored_;
  std::vector<SessionState> states_;
  std::size_t               current_ = 0;
  /// The coverage of each component of the current state, with the component that made it.
  std::vector<std::pair<AiMaskComponent, std::vector<std::uint8_t>>> component_coverages_;
  /// Immutable after it is assigned; see @ref AcquireCoveragePixels.
  std::shared_ptr<const std::vector<std::uint8_t>>                   coverage_;
};

}  // namespace alcedo
