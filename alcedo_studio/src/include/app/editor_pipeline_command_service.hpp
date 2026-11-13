//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/editor_adjustment_types.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief Validate and apply one parameter patch to the existing live Model.
 *
 * The JSON input is the application/history boundary only. Supplied keys, types, finite numbers,
 * and array dimensions are checked before a typed Model update is built; the update then reaches
 * the owning Model through its focused operation without a full Model JSON read/merge/reload.
 * Model operations normalize values and stamp changed fields; topology and other Models stay
 * intact.
 * Compound input is parsed completely before any owner field is changed.
 * @pre Caller is the only writer of @p document (the editor session owner thread for its
 *      working document); target is complete and is not Mask.
 * @param document Live document to mutate in place.
 * @param target Explicit node/adjustment identity; no missing identity is inferred.
 * @param params Field-specific JSON boundary object. Persistence/history JSON is accepted at this
 *        boundary and converted to a typed operation before mutation.
 * @param error Optional failure detail.
 * @return false for an invalid target or parameter; no history or persistence is performed.
 */
auto ApplyEditorParameterPatch(PipelineDocument& document, const EditorParameterTarget& target,
                               const nlohmann::json& params, std::string* error) -> bool;

/**
 * @brief Apply a complete stored value of one field to the live document owner, as a history head
 *        move restores it: @ref ParseEditorParameterValue, then the same write as
 *        @ref ApplyEditorParameterPatch. Each field of @p value is set, and a key that @p value does
 *        not have takes the default of the Model.
 * @return false for an invalid target or parameter; no history or persistence is performed.
 */
auto ApplyEditorParameterValue(PipelineDocument& document, const EditorParameterTarget& target,
                               const nlohmann::json& value, std::string* error) -> bool;

/**
 * @brief Read the current model JSON for @p target at the persistence/history boundary.
 *
 * ApplyEditorParameterPatch does not use this function to update a live Model. It remains the
 * JSON boundary required by history, project transfer, and persistence callers.
 *
 * @return false when the node or instance is missing.
 */
auto ReadEditorParameterJson(const PipelineDocument& document, const EditorParameterTarget& target,
                             nlohmann::json* json, std::string* error) -> bool;

/// Saturation of the Default Grade after the method changes to the film print.
inline constexpr float kFilmPrintEntrySaturation = 1.0f;

/**
 * @brief Default Grade saturation that an `odt` edit resets when it turns on the film print.
 *
 * The negative and the print already give the colour of this method, so the saturation of the
 * RAW import look stays out of it (lab U80). The rule applies only when @p odt_before has another
 * method and @p odt_after has `film_print`, and only while the saturation is still
 * @ref kDefaultPipelineSaturation. A value that the user selected stays.
 *
 * @param odt_before Output transform JSON before the edit.
 * @param odt_after Output transform JSON after the edit.
 * @return The target to write @ref kFilmPrintEntrySaturation to, or nullopt when the rule does not
 *         apply.
 */
[[nodiscard]] auto FilmPrintEntrySaturationTarget(const PipelineDocument& document,
                                                  const nlohmann::json&   odt_before,
                                                  const nlohmann::json&   odt_after)
    -> std::optional<EditorParameterTarget>;

/**
 * @brief Default Grade curve that an `odt` edit resets when it turns on the film print.
 *
 * The Film-like curve of the RAW import look (@ref kDefaultPipelineToneCurvePresetId) is the
 * Adobe tone on top of ACES 2.0; the negative and the print give the tone of the film print. The
 * rule applies on the entry of @ref FilmPrintEntrySaturationTarget, and only while the RGB curve
 * still has the points of the preset. The write sets the RGB curve to the identity; the channel
 * curves stay.
 *
 * @return The `curve` target, or nullopt when the rule does not apply.
 */
[[nodiscard]] auto FilmPrintEntryCurveTarget(const PipelineDocument& document,
                                             const nlohmann::json&   odt_before,
                                             const nlohmann::json&   odt_after)
    -> std::optional<EditorParameterTarget>;

/// Contrast of Color Grade 1 after a profile tone entry: the catalog identity.
inline constexpr float kProfileToneEntryContrast   = 0.0f;
/// Saturation of Color Grade 1 after a profile tone entry, as @ref kFilmPrintEntrySaturation.
inline constexpr float kProfileToneEntrySaturation = 1.0f;

/// One Grade write of an entry reset: the field target and its patch JSON.
struct EditorEntryResetWrite {
  EditorParameterTarget target;
  nlohmann::json        patch;
};

/**
 * @brief Grade writes that a `camera_profile_file` edit makes when it turns on the profile tone
 *        stage (dcp_profile_tone_curve_study.md, section 6.5).
 *
 * The profile curve and LookTable carry the camera's contrast and color, so the RAW import look
 * stays out of them. The rule applies only on an entry: the stage runs after the edit and stayed
 * out of the render before it. The stage runs when the reference is bound, its profile has a
 * `ProfileToneCurve`, `profile_tone` is true, and the DRT method is ACES 2.0 or OpenDRT.
 *
 * On an entry, when the document has Color Grade 1 (`grade.primary`), the result holds a write of
 * @ref kProfileToneEntryContrast while its contrast still equals
 * @ref kImportLookContrastBeforeFilmLike (a document imported before #891), a write of
 * @ref kProfileToneEntrySaturation while its saturation still equals
 * @ref kDefaultPipelineSaturation, and a write of the identity RGB curve while the RGB curve still
 * has the points of the Film-like preset (@ref kDefaultPipelineToneCurvePresetId). A value that the
 * user selected stays; a document without `grade.primary` gets no write. Exposure is never written
 * (the anchor of section 3.3).
 *
 * @param document Live document after the edit; its Develop reference is bound by the write.
 * @param develop_before Develop Model JSON before the edit (the history before-side). Its stored
 *        reference is resolved through DcpProfileLibrary::Shared() to read whether its profile has
 *        a curve; an identity that the library does not hold had no stage.
 * @return The writes in the order contrast, saturation, curve; empty when the rule does not apply.
 * @throws std::exception when @p develop_before holds a malformed reference.
 */
[[nodiscard]] auto ProfileToneEntryResetTargets(const PipelineDocument& document,
                                                const nlohmann::json&   develop_before)
    -> std::vector<EditorEntryResetWrite>;

/**
 * @brief Grade writes of the entry resets of the RAW import look that a settled edit of
 *        @p field_key makes: @ref FilmPrintEntrySaturationTarget and
 *        @ref FilmPrintEntryCurveTarget for `odt`, and @ref ProfileToneEntryResetTargets for
 *        `camera_profile_file`.
 *
 * The caller applies them in the history batch of the edit, so one Undo restores them with it.
 * @param document Live document after the edit.
 * @param before Model JSON of the edited field before the edit.
 * @param after Model JSON of the edited field after the edit.
 * @return The writes; empty for another field or when no rule applies.
 * @throws std::exception when @p before holds a malformed `camera_profile_file` reference.
 */
[[nodiscard]] auto EntryResetWrites(const PipelineDocument& document, std::string_view field_key,
                                    const nlohmann::json& before, const nlohmann::json& after)
    -> std::vector<EditorEntryResetWrite>;

/**
 * @brief Stable JSON dump of @p document for comparisons in tests.
 */
[[nodiscard]] auto CanonicalPipelineDocumentJson(const PipelineDocument& document) -> std::string;

/**
 * @brief True when graph Validate and ValidateImageBackbone are both empty.
 */
auto PipelineDocumentPassesValidation(const PipelineDocument& document, std::string* error) -> bool;

/**
 * @brief Fill a current-panel target from @p field_key and the live document.
 *
 * Ordinary Grade fields target the Default Grade (`grade.primary`, else the first
 * backbone Grade). Clarity, Sharpen, Halation, and Film Grain target DRT/Post.
 * Develop, geometry, and `odt` target their existing owners. Instance ids come from
 * the live Models, not from concatenating the field key.
 *
 * @param document Live document used to resolve node and instance ids.
 * @param field_key Current-panel field key, such as `exposure` or `clarity`.
 * @param error Optional failure detail.
 * @return Complete target, or nullopt when the field or owner Model is missing.
 */
[[nodiscard]] auto CompleteCurrentPanelParameterTarget(const PipelineDocument& document,
                                                       std::string field_key, std::string* error)
    -> std::optional<EditorParameterTarget>;

}  // namespace alcedo
