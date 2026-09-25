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
 * Project metadata 0.10.0 stores no DNG color profile tables: the Image metadata
 * and the Develop JSON keep only a profile fingerprint, and the pipeline service
 * binds the tables from the source file at load. Root ids and checkpoints of 0.9.0
 * projects hashed the complete tables, so 0.9.0 projects are rejected without
 * conversion. The format identities below are unchanged from 0.9.0.
 *
 * | Identity | Constant | Value |
 * | --- | --- | --- |
 * | Project metadata | @ref kProjectFileVersion | 0.10.0 |
 * | Packed project header | @ref kPackedProjectFormatVersion | 7 |
 * | Pipeline document JSON | @ref kPipelineDocumentFormatVersion | 7 |
 * | Image edit schema | @ref kImageEditSchemaVersion | 5 |
 * | Commit hash input | @ref kCommitFormatVersion | 5 |
 * | Chain-fold hash input | @ref kChainFormatVersion | 5 |
 * | Typed batch payload | @ref kPipelineEditBatchFormatVersion | 4 |
 * | Immutable root serialized pipeline state | @ref kRootStateFormatVersion | 5 |
 * | Checkpoint serialized pipeline state | @ref kCheckpointStateFormatVersion | 5 |
 * | Mini-Git WAL record | @ref kMiniGitJournalRecordFormatVersion | 6 |
 * | Transfer package schema | @ref kAdjustmentTransferSchema | alcedo.adjustment_transfer.v6 |
 */

/// Project metadata version written by SaveProject and required on open.
inline constexpr std::string_view kProjectFileVersion = "0.10.0";
/// Inclusive lower bound of accepted project metadata. Equals @ref kProjectFileVersion.
inline constexpr std::string_view kMinSupportedProjectFileVersion = "0.10.0";
/// Inclusive upper bound of accepted project metadata. Equals @ref kProjectFileVersion.
inline constexpr std::string_view kMaxSupportedProjectFileVersion = "0.10.0";
/// Packed `.alcd` header version. Independent of the metadata string.
inline constexpr std::uint32_t kPackedProjectFormatVersion = 7;

/// `PipelineDocument` JSON `format_version`.
inline constexpr std::uint32_t kPipelineDocumentFormatVersion = 7;

/// Per-image history schema stored on `ImageEditState.project_schema_version`.
inline constexpr std::uint32_t kImageEditSchemaVersion = 5;

/// Commit object hash-input layout. Typed `PipelineEditBatch` payloads use this value.
inline constexpr std::uint32_t kCommitFormatVersion = 5;

/// First-parent chain-fold hash-input layout.
inline constexpr std::uint32_t kChainFormatVersion = 5;

/// Typed batch payload schema stored inside a commit. Independent of @ref kCommitFormatVersion.
inline constexpr std::uint32_t kPipelineEditBatchFormatVersion = 4;

/// Immutable root serialized pipeline state stored in `PipelineRoot.serialized_pipeline_state`.
inline constexpr std::uint32_t kRootStateFormatVersion = 5;

/// Checkpoint serialized pipeline state stored in `ImageEditState.serialized_pipeline_state`.
inline constexpr std::uint32_t kCheckpointStateFormatVersion = 5;

/// Mini-Git WAL record JSON `format_version`.
inline constexpr std::uint32_t kMiniGitJournalRecordFormatVersion = 6;

/// Adjustment Transfer package `schema` string. v5 packages are rejected without
/// conversion.
inline constexpr std::string_view kAdjustmentTransferSchema = "alcedo.adjustment_transfer.v6";

}  // namespace alcedo
