//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "edit/history/editor_transaction_journal.hpp"
#include "json.hpp"

namespace alcedo {

/// DuckDB recovery metadata state for one image/journal generation.
struct EditorRecoveryMetadata {
  sl_element_id_t element_id                      = 0;
  Hash128         version_id{};
  std::uint64_t   journal_generation              = 0;
  std::uint64_t   materialized_operation_sequence = 0;
  Hash128         transaction_chain_hash{};
  Hash128         pipeline_parameter_hash{};
};

struct EditorJournalRecoveryResult {
  bool                    accepted              = false;
  bool                    requires_materialize  = false;
  bool                    diagnostic_emitted    = false;
  std::uint64_t           durable_operation_sequence = 0;
  std::uint64_t           durable_batch_commit_sequence = 0;
  std::uint64_t           last_valid_sequence    = 0;
  JournalTimelineSimulator recovered_state;
  std::optional<std::filesystem::path> diagnostic_path;
  std::string             message;
};

/// Pure journal recovery: decode the record chain, stop at the first malformed
/// frame, locate the last valid JournalBatchCommit, seed from the stored
/// materialized head, and REDO later journal-committed edit-history operations.
/// Does not write DuckDB; the caller materializes through EditorHistoryMaterializer.
[[nodiscard]] auto RecoverEditorJournal(
    const EditorTransactionJournal& journal, const EditorJournalIdentity& identity,
    const EditorRecoveryMetadata& stored_metadata,
    std::vector<EditTransaction> materialized_transactions, std::size_t materialized_cursor,
    std::optional<nlohmann::json> materialized_pipeline_params) -> EditorJournalRecoveryResult;

/// Preserve a journal byte image and reason next to the active journal path.
[[nodiscard]] auto WriteEditorJournalDiagnosticBundle(
    const std::filesystem::path& journal_path, const std::vector<std::uint8_t>& journal_bytes,
    const std::string& reason, std::string* error = nullptr)
    -> std::optional<std::filesystem::path>;

[[nodiscard]] auto ComputePipelineParameterHash(const nlohmann::json& params) -> Hash128;

}  // namespace alcedo
