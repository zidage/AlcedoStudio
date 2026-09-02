//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <string>

#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/pipeline_history_format.hpp"
#include "json.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Immutable image-root envelope stored in `PipelineRoot`.
 *
 * The envelope owns the full default `PipelineDocument` after image-specific
 * Develop data is bound. `root_id` is computed from this payload and the image
 * owner; it is the DuckDB primary key, not a field inside the blob.
 */
struct PipelineRootState {
  sl_element_id_t               element_id = 0;
  PipelineDocument              document;
  std::optional<nlohmann::json> raw_color_context;
};

/**
 * @brief Saved acceleration document labeled with one history tip.
 *
 * Use the checkpoint only when `root_id`, `head_commit_hash`, and
 * `transaction_chain_hash` match the loaded `CommitGraph` active Version.
 */
struct PipelineDocumentCheckpoint {
  root_id_t                root_id{};
  head_commit_hash_t       head_commit_hash = std::nullopt;
  transaction_chain_hash_t transaction_chain_hash{};
  PipelineDocument         document;
};

/**
 * @brief Content-addressed root identity bound to the image owner and root document.
 *
 * Hash input is little-endian and includes the root and document format versions,
 * @p element_id, the canonical document dump, and the canonical raw-color JSON
 * (the text `null` when @p raw_color_context is empty).
 *
 * @param element_id Image owner stored with the root.
 * @param document Full default document, including image-specific Develop data.
 * @param raw_color_context Optional runtime color JSON stored beside the document.
 * @return Stable `root_id` for this owner and payload.
 */
[[nodiscard]] auto ComputeRootId(sl_element_id_t element_id, const PipelineDocument& document,
                                 const std::optional<nlohmann::json>& raw_color_context)
    -> root_id_t;

/**
 * @brief Encode the immutable root envelope.
 *
 * Writes every required field, including an explicit JSON null for a missing
 * raw-color context. Rejects a document whose format version is not
 * @ref kPipelineDocumentFormatVersion.
 *
 * @throws std::runtime_error when the document cannot be validated.
 */
[[nodiscard]] auto EncodePipelineRootState(sl_element_id_t element_id,
                                           const PipelineDocument& document,
                                           const std::optional<nlohmann::json>& raw_color_context)
    -> nlohmann::json;

/**
 * @brief Parse and require a canonical immutable root envelope.
 *
 * Unknown keys, wrong format versions, a mismatched nested document dump, and
 * a mismatched `element_id` type are rejected. Does not convert older envelopes.
 *
 * @throws std::runtime_error when @p json is not a canonical root envelope.
 */
[[nodiscard]] auto DecodePipelineRootState(const nlohmann::json& json) -> PipelineRootState;

/**
 * @brief Encode one checkpoint document with its history labels.
 *
 * Label fields must come from one `CommitGraphMaterialization`. The nested
 * document is the live DAG, not a CPU parameter table.
 *
 * @throws std::runtime_error when the document cannot be validated.
 */
[[nodiscard]] auto EncodePipelineDocumentCheckpoint(const root_id_t& root_id,
                                                    head_commit_hash_t head,
                                                    const transaction_chain_hash_t& chain,
                                                    const PipelineDocument& document)
    -> nlohmann::json;

/**
 * @brief Parse and require a canonical checkpoint envelope.
 *
 * Unknown keys, wrong format versions, and a mismatched nested document dump
 * are rejected. Does not convert older `pipeline_params` checkpoints.
 *
 * @throws std::runtime_error when @p json is not a canonical checkpoint.
 */
[[nodiscard]] auto DecodePipelineDocumentCheckpoint(const nlohmann::json& json)
    -> PipelineDocumentCheckpoint;

/**
 * @brief True when @p json looks like a current checkpoint envelope.
 *
 * Distinguishes the document checkpoint from a CPU-parameter snapshot without
 * fully decoding the nested DAG.
 */
[[nodiscard]] auto IsPipelineDocumentCheckpointJson(const nlohmann::json& json) -> bool;

}  // namespace alcedo
