//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#include "edit/geometry/types.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "json.hpp"

namespace alcedo::pipeline_edit_json {

/**
 * @brief JSON collection helpers for typed history payloads.
 *
 * This is the persistence / nested-document boundary: finite numbers, exact
 * object keys, scene edges, parameter targets, and canonical Color Grade / Mask
 * / Brush JSON. It does not apply a change to a live document.
 */

[[noreturn]] void Fail(const std::string& message);

void              RequireObject(const nlohmann::json& json, std::string_view context);
void RequireExactObjectKeys(const nlohmann::json& json, std::initializer_list<const char*> keys,
                            std::string_view context);
void RequireCanonicalDump(const nlohmann::json& stored, const nlohmann::json& canonical,
                          std::string_view context);
void RejectNonFiniteNumbers(const nlohmann::json& value, std::string_view context);
void RequireModelObject(const nlohmann::json& value, std::string_view context);

[[nodiscard]] auto RequireString(const nlohmann::json& json, const char* key,
                                 std::string_view context) -> std::string;
[[nodiscard]] auto RequireNonEmptyString(const nlohmann::json& json, const char* key,
                                         std::string_view context) -> std::string;
[[nodiscard]] auto RequireBool(const nlohmann::json& json, const char* key,
                               std::string_view context) -> bool;
[[nodiscard]] auto RequireFiniteFloat(const nlohmann::json& json, const char* key,
                                      std::string_view context) -> float;
[[nodiscard]] auto RequireNormalizedMix(const nlohmann::json& json, const char* key,
                                        std::string_view context) -> float;
[[nodiscard]] auto OptionalIdFromJson(const nlohmann::json& value, std::string_view context,
                                      const char* key) -> std::string;
[[nodiscard]] auto RequiredIdFromJson(const nlohmann::json& json, const char* key,
                                      std::string_view context) -> std::string;
[[nodiscard]] auto RequirePositiveUint64(const nlohmann::json& json, const char* key,
                                         std::string_view context) -> std::uint64_t;
[[nodiscard]] auto RequireNonNegativeUint32(const nlohmann::json& json, const char* key,
                                            std::string_view context) -> std::uint32_t;
[[nodiscard]] auto IdToJson(std::string_view value) -> nlohmann::json;

[[nodiscard]] auto OwnerKindText(PipelineParameterOwnerKind kind) -> std::string_view;
[[nodiscard]] auto OwnerKindFromText(std::string_view text) -> PipelineParameterOwnerKind;
[[nodiscard]] auto NodeKindText(PipelineEditNodeKind kind) -> std::string_view;
[[nodiscard]] auto NodeKindFromText(std::string_view text) -> PipelineEditNodeKind;

[[nodiscard]] auto EdgeToJson(const PipelineSceneEdge& edge) -> nlohmann::json;
[[nodiscard]] auto EdgeFromJson(const nlohmann::json& json, std::string_view context)
    -> PipelineSceneEdge;
void RequireEdgeEndpoints(const PipelineSceneEdge& edge, const NodeId& from, const NodeId& to,
                          std::string_view context);

[[nodiscard]] auto CanonicalColorGradeNodeJson(const nlohmann::json& node, std::string_view context)
    -> nlohmann::json;
[[nodiscard]] auto CanonicalMaskJson(const nlohmann::json& mask, std::string_view context)
    -> nlohmann::json;
[[nodiscard]] auto CanonicalMaskSourceJson(const nlohmann::json& source, std::string_view context)
    -> nlohmann::json;
[[nodiscard]] auto CanonicalBrushStrokeJson(const BrushStroke& stroke, std::string_view context)
    -> nlohmann::json;
[[nodiscard]] auto StrokeFromCanonicalJson(const nlohmann::json& json, std::string_view context)
    -> BrushStroke;
[[nodiscard]] auto TranslationVectorFromJson(const nlohmann::json& json, std::string_view context)
    -> Vector2;
[[nodiscard]] auto TranslationVectorToJson(Vector2 value) -> nlohmann::json;
[[nodiscard]] auto AssetKeyFromBrushSource(const nlohmann::json& source, std::string_view context)
    -> std::string;

void               ValidateParameterTarget(const PipelineParameterTarget& target);
[[nodiscard]] auto TargetToJson(const PipelineParameterTarget& target) -> nlohmann::json;
[[nodiscard]] auto TargetFromJson(const nlohmann::json& json) -> PipelineParameterTarget;

}  // namespace alcedo::pipeline_edit_json
