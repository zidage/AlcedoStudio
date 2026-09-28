//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "decoders/processor/raw_color_context.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/pipeline_document_checkpoint.hpp"
#include "json.hpp"
#include "sleeve/storage.hpp"
#include "type/type.hpp"

/**
 * @file
 * @brief Build pipeline documents from an image's stored history.
 *
 * Shared by the editor load path and the committed snapshot loader, so a document at one history
 * state is built the same way for every consumer: decode the immutable root, take the matching
 * checkpoint or replay the first-parent chain, and bind the image camera profile.
 */
namespace alcedo {

/// Decoded immutable root of one image: its document and optional RAW color context.
struct LoadedRootState {
  PipelineDocument                      document;
  std::optional<RawRuntimeColorContext> raw_color_context;
};

/// Throw when @p document is not a valid product graph with an image backbone.
void               ValidateProductDocument(const PipelineDocument& document, sl_element_id_t id);

/**
 * @brief Decode a stored root state and check that it belongs to @p element_id and
 *        @p expected_root_id.
 * @return The decoded root, or nullopt when decoding fails or an identity differs.
 */
[[nodiscard]] auto TryDecodeRootState(const nlohmann::json& encoded, sl_element_id_t element_id,
                                      const root_id_t& expected_root_id)
    -> std::optional<LoadedRootState>;

/// Decode a stored checkpoint; nullopt when it cannot be decoded.
[[nodiscard]] auto TryDecodeCheckpoint(const nlohmann::json& encoded)
    -> std::optional<PipelineDocumentCheckpoint>;

/// Bind the Rec.709 working-space camera profile onto the Develop node of @p document.
void BindWorkingSpaceDevelopData(PipelineDocument& document);

/**
 * @brief Bind Rec.709 onto a document that cannot resolve CameraToAp1 and has no RAW color
 *        context. RAW roots keep missing matrices so decode fails closed.
 */
void EnsureRenderableCameraProfile(PipelineDocument&                            document,
                                   const std::optional<RawRuntimeColorContext>& raw_color_context);

/**
 * @brief Bind the DNG profile of a decoded root state: its RAW color context and root document.
 * @pre The caller holds no database connection lock.
 */
void BindSourceDngProfiles(Storage& storage, sl_element_id_t id, LoadedRootState& root);

/**
 * @brief Bind the image camera profile onto a document that nothing renders yet. RAW roots bind
 *        the stored RAW color context; roots without one use the working-space profile.
 */
void BindRootCameraProfile(PipelineDocument&                            document,
                           const std::optional<RawRuntimeColorContext>& raw_color_context);

/**
 * @brief Replay the immutable root through the first-parent chain of @p head and bind the camera
 *        profile. Touches no shared state, so a caller may drop the result on failure.
 * @return The new document, or null with @p error set.
 */
[[nodiscard]] auto BuildDocumentFromRoot(const CommitGraph&     graph,
                                         const LoadedRootState& root_state, head_commit_hash_t head,
                                         std::string* error) -> std::shared_ptr<PipelineDocument>;

/**
 * @brief Copy the document of a checkpoint whose labels match the history tip and bind the camera
 *        profile of @p root_state.
 */
[[nodiscard]] auto BuildDocumentFromCheckpoint(const PipelineDocumentCheckpoint& checkpoint,
                                               const LoadedRootState&            root_state)
    -> std::shared_ptr<PipelineDocument>;

}  // namespace alcedo
