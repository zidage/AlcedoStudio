//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string_view>

namespace alcedo {

/**
 * @brief Published format identities for pipeline-document history.
 *
 * One incompatible project release. Loaders accept only these values. Older
 * project, document, history, root, checkpoint, WAL, and transfer identities
 * are rejected without conversion.
 *
 * | Identity | Constant | Value |
 * | --- | --- | --- |
 * | Project metadata | @ref kProjectFileVersion | 0.4.0 |
 * | Packed project header | @ref kPackedProjectFormatVersion | 4 |
 * | Pipeline document JSON | @ref kPipelineDocumentFormatVersion | 4 |
 * | Image edit schema | @ref kImageEditSchemaVersion | 2 |
 * | Commit hash input | @ref kCommitFormatVersion | 2 |
 * | Chain-fold hash input | @ref kChainFormatVersion | 2 |
 * | Typed batch payload | @ref kPipelineEditBatchFormatVersion | 1 |
 * | Immutable root envelope | @ref kRootStateFormatVersion | 2 |
 * | Checkpoint envelope | @ref kCheckpointStateFormatVersion | 2 |
 * | Mini-Git WAL record | @ref kMiniGitJournalRecordFormatVersion | 3 |
 * | Transfer package schema | @ref kAdjustmentTransferSchema | alcedo.adjustment_transfer.v2 |
 */

/// Project metadata version written by SaveProject and required on open.
inline constexpr std::string_view kProjectFileVersion = "0.4.0";
/// Inclusive lower bound of accepted project metadata. Equals @ref kProjectFileVersion.
inline constexpr std::string_view kMinSupportedProjectFileVersion = "0.4.0";
/// Inclusive upper bound of accepted project metadata. Equals @ref kProjectFileVersion.
inline constexpr std::string_view kMaxSupportedProjectFileVersion = "0.4.0";
/// Packed `.alcd` header version. Independent of the metadata string.
inline constexpr std::uint32_t kPackedProjectFormatVersion = 4;

/// `PipelineDocument` JSON `format_version`.
inline constexpr std::uint32_t kPipelineDocumentFormatVersion = 4;

/// Per-image history schema stored on `ImageEditState.project_schema_version`.
inline constexpr std::uint32_t kImageEditSchemaVersion = 2;

/// Commit object hash-input layout. Typed `PipelineEditBatch` payloads use this value.
inline constexpr std::uint32_t kCommitFormatVersion = 2;

/// First-parent chain-fold hash-input layout.
inline constexpr std::uint32_t kChainFormatVersion = 2;

/// Typed batch payload schema stored inside a commit. Independent of @ref kCommitFormatVersion.
inline constexpr std::uint32_t kPipelineEditBatchFormatVersion = 1;

/// Immutable root envelope stored in `PipelineRoot.serialized_pipeline_state`.
inline constexpr std::uint32_t kRootStateFormatVersion = 2;

/// Checkpoint envelope stored in `ImageEditState.serialized_pipeline_state`.
inline constexpr std::uint32_t kCheckpointStateFormatVersion = 2;

/// Mini-Git WAL record JSON `format_version`.
inline constexpr std::uint32_t kMiniGitJournalRecordFormatVersion = 3;

/// Adjustment Transfer package `schema` string.
inline constexpr std::string_view kAdjustmentTransferSchema = "alcedo.adjustment_transfer.v2";

}  // namespace alcedo
